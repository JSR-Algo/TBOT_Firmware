#ifndef ESP_DNS_RESOLVER_H
#define ESP_DNS_RESOLVER_H

#include "transport_deadline.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <string>

struct DnsLookupState {
    std::mutex mutex;
    std::condition_variable completed_cv;
    bool completed = false;
    bool abandoned = false;
    bool resolved = false;
    in_addr address = {};
};

struct DnsLookupRequest {
    std::shared_ptr<DnsLookupState> state;
    std::string host;
};

inline std::atomic<bool> g_dns_lookup_in_flight{false};

inline bool ResolveHostIpv4WithDeadline(const std::string& host, int64_t deadline_us,
                                        in_addr* address, int* error_code) {
    const int remaining_ms =
        RemainingTransportTimeoutMs(deadline_us, esp_timer_get_time());
    if (remaining_ms <= 0) {
        *error_code = ETIMEDOUT;
        return false;
    }

    if (inet_pton(AF_INET, host.c_str(), address) == 1) {
        *error_code = 0;
        return true;
    }

    bool expected_idle = false;
    if (!g_dns_lookup_in_flight.compare_exchange_strong(expected_idle, true)) {
        *error_code = EBUSY;
        return false;
    }

    auto state = std::shared_ptr<DnsLookupState>(new (std::nothrow) DnsLookupState());
    if (!state) {
        g_dns_lookup_in_flight.store(false);
        *error_code = ENOMEM;
        return false;
    }
    auto request = std::unique_ptr<DnsLookupRequest>(
        new (std::nothrow) DnsLookupRequest{state, host});
    if (!request) {
        g_dns_lookup_in_flight.store(false);
        *error_code = ENOMEM;
        return false;
    }

    DnsLookupRequest* request_ptr = request.get();
    BaseType_t created = xTaskCreate([](void* arg) {
        std::unique_ptr<DnsLookupRequest> owned_request(
            static_cast<DnsLookupRequest*>(arg));
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const int resolve_error =
            getaddrinfo(owned_request->host.c_str(), nullptr, &hints, &result);

        in_addr resolved_address = {};
        const bool resolved = resolve_error == 0 && result != nullptr;
        if (resolved) {
            resolved_address =
                reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        }
        if (result != nullptr) {
            freeaddrinfo(result);
        }

        auto state = owned_request->state;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->abandoned) {
                state->resolved = resolved;
                state->address = resolved_address;
                state->completed = true;
            }
        }
        g_dns_lookup_in_flight.store(false);
        state->completed_cv.notify_one();
        owned_request.reset();
        state.reset();
        vTaskDelete(nullptr);
    }, "dns_resolve", 4096, request_ptr, tskIDLE_PRIORITY + 1, nullptr);
    if (created != pdPASS) {
        g_dns_lookup_in_flight.store(false);
        *error_code = ENOMEM;
        return false;
    }
    request.release();

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool completed = state->completed_cv.wait_for(
        lock, std::chrono::milliseconds(remaining_ms),
        [&state]() { return state->completed; });
    if (!completed) {
        state->abandoned = true;
        *error_code = ETIMEDOUT;
        return false;
    }
    if (!state->resolved) {
        *error_code = EHOSTUNREACH;
        return false;
    }
    *address = state->address;
    *error_code = 0;
    return true;
}

#endif  // ESP_DNS_RESOLVER_H
