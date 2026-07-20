#ifndef ESP_DNS_RESOLVER_H
#define ESP_DNS_RESOLVER_H

#include "async_lookup_lifecycle.h"
#include "transport_deadline.h"

#include <esp_timer.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <lwip/inet.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

inline constexpr size_t kMaxConcurrentDnsLookups = 8;

struct DnsLookupSlot {
    AsyncLookupLifecycle lifecycle;
    std::mutex host_mutex;
    std::string host;
};

inline std::array<DnsLookupSlot, kMaxConcurrentDnsLookups> g_dns_lookup_slots;

struct DecodedDnsCallbackToken {
    DnsLookupSlot* slot = nullptr;
    uint32_t generation = 0;
};

inline void* EncodeDnsCallbackToken(size_t slot_index, uint32_t generation) {
    constexpr uintptr_t kSlotBits = 4;
    const uintptr_t token =
        (static_cast<uintptr_t>(generation) << kSlotBits) | (slot_index + 1);
    return reinterpret_cast<void*>(token);
}

inline DecodedDnsCallbackToken DecodeDnsCallbackToken(void* arg) {
    constexpr uintptr_t kSlotBits = 4;
    constexpr uintptr_t kSlotMask = (1u << kSlotBits) - 1;
    const uintptr_t token = reinterpret_cast<uintptr_t>(arg);
    const uintptr_t encoded_slot = token & kSlotMask;
    if (encoded_slot == 0 || encoded_slot > kMaxConcurrentDnsLookups) {
        return {};
    }
    return {
        &g_dns_lookup_slots[encoded_slot - 1],
        static_cast<uint32_t>(token >> kSlotBits),
    };
}

inline void CompleteDnsLookup(const DecodedDnsCallbackToken& token,
                              const ip_addr_t* ipaddr) {
    if (token.slot == nullptr) {
        return;
    }
    const bool resolved = ipaddr != nullptr && IP_IS_V4(ipaddr);
    const uint32_t address = resolved
        ? ip4_addr_get_u32(ip_2_ip4(ipaddr))
        : 0;
    token.slot->lifecycle.Complete(token.generation, resolved, address);
}

inline void OnDnsLookupFound(const char*, const ip_addr_t* ipaddr, void* arg) {
    CompleteDnsLookup(DecodeDnsCallbackToken(arg), ipaddr);
}

inline void StartDnsLookup(void* arg) {
    const DecodedDnsCallbackToken token = DecodeDnsCallbackToken(arg);
    if (token.slot == nullptr ||
        !token.slot->lifecycle.IsWaiting(token.generation)) {
        return;
    }

    std::string host;
    {
        std::lock_guard<std::mutex> lock(token.slot->host_mutex);
        host = token.slot->host;
    }
    if (!token.slot->lifecycle.IsWaiting(token.generation)) {
        return;
    }

    ip_addr_t immediate_address = {};
    const err_t error = dns_gethostbyname_addrtype(
        host.c_str(), &immediate_address, OnDnsLookupFound, arg,
        LWIP_DNS_ADDRTYPE_IPV4);
    if (error == ERR_OK) {
        CompleteDnsLookup(token, &immediate_address);
    } else if (error != ERR_INPROGRESS) {
        CompleteDnsLookup(token, nullptr);
    }
}

struct AcquiredDnsLookupSlot {
    DnsLookupSlot* slot = nullptr;
    size_t index = 0;
    uint32_t generation = 0;
};

inline AcquiredDnsLookupSlot AcquireDnsLookupSlot() {
    for (size_t index = 0; index < g_dns_lookup_slots.size(); ++index) {
        auto& slot = g_dns_lookup_slots[index];
        const uint32_t generation = slot.lifecycle.TryAcquire();
        if (generation != 0) {
            return {&slot, index, generation};
        }
    }
    return {};
}

inline bool ResolveHostIpv4WithDeadline(const std::string& host, int64_t deadline_us,
                                        in_addr* address, int* error_code) {
    const int remaining_ms =
        RemainingTransportTimeoutMs(deadline_us, esp_timer_get_time());
    if (remaining_ms <= 0) {
        *error_code = ETIMEDOUT;
        return false;
    }

    if (inet_aton(host.c_str(), address) != 0) {
        *error_code = 0;
        return true;
    }

    const AcquiredDnsLookupSlot acquired = AcquireDnsLookupSlot();
    if (acquired.slot == nullptr) {
        *error_code = EAGAIN;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(acquired.slot->host_mutex);
        acquired.slot->host = host;
    }
    void* callback_token =
        EncodeDnsCallbackToken(acquired.index, acquired.generation);

    if (tcpip_try_callback(StartDnsLookup, callback_token) != ERR_OK) {
        acquired.slot->lifecycle.Complete(acquired.generation, false, 0);
        (void)acquired.slot->lifecycle.WaitFor(
            acquired.generation, std::chrono::milliseconds(0));
        *error_code = EAGAIN;
        return false;
    }

    const AsyncLookupResult result = acquired.slot->lifecycle.WaitFor(
        acquired.generation, std::chrono::milliseconds(remaining_ms));
    if (result.status == AsyncLookupStatus::kTimedOut) {
        *error_code = ETIMEDOUT;
        return false;
    }
    if (result.status == AsyncLookupStatus::kFailed) {
        *error_code = EHOSTUNREACH;
        return false;
    }

    address->s_addr = result.value;
    *error_code = 0;
    return true;
}

#endif  // ESP_DNS_RESOLVER_H
