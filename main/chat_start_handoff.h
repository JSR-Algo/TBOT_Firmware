#pragma once
#include <atomic>
#include <cstdint>
#include "protocols/connection_source.h"

// One serialized receiver request/expiry writer, one application admission
// writer. All shared words are SC atomic32; neither side retries a snapshot.
class ChatStartHandoff {
public:
    static constexpr uint64_t kAdmissionUs = 250000;
    struct Request {
        ConnectionSource source;
        uint64_t protocol_generation = 0;
        uint32_t connect_generation = 0, serial = 0;
        uint64_t received_us = 0;
        uint64_t admission_deadline_us = 0;
    };
    struct Admission { uint32_t response_generation = 0, reset_token = 0; };
    bool Publish(Request& request) {
        if (serial_ == UINT32_MAX) return false;
        published_.store(0);
        if (!request.source.Valid() ||
            !request.protocol_generation || !request.connect_generation) return false;
        if (serial_ != UINT32_MAX) ++serial_;
        request.serial = serial_;
        source_.store(request.source.source_id);
        epoch_.store(request.source.connection_epoch);
        protocol_lo_.store(static_cast<uint32_t>(request.protocol_generation));
        protocol_hi_.store(static_cast<uint32_t>(request.protocol_generation >> 32));
        connect_.store(request.connect_generation);
        received_lo_.store(static_cast<uint32_t>(request.received_us));
        received_hi_.store(static_cast<uint32_t>(request.received_us >> 32));
        deadline_lo_.store(static_cast<uint32_t>(request.admission_deadline_us));
        deadline_hi_.store(static_cast<uint32_t>(request.admission_deadline_us >> 32));
        published_.store(request.serial);
        return request.serial != UINT32_MAX;
    }
    bool TryRequest(Request& out) const {
        const auto serial = published_.load();
        if (!serial) return false;
        Request value{{source_.load(), epoch_.load()},
            uint64_t(protocol_lo_.load()) | (uint64_t(protocol_hi_.load()) << 32),
            connect_.load(), serial,
            uint64_t(received_lo_.load()) | (uint64_t(received_hi_.load()) << 32),
            uint64_t(deadline_lo_.load()) | (uint64_t(deadline_hi_.load()) << 32)};
        if (serial != published_.load()) return false;
        out = value;
        return true;
    }
    bool Current(const Request& request) const {
        Request value;
        return TryRequest(value) && value.serial == request.serial &&
            value.source.source_id == request.source.source_id &&
            value.source.connection_epoch == request.source.connection_epoch &&
            value.protocol_generation == request.protocol_generation &&
            value.connect_generation == request.connect_generation &&
            value.received_us == request.received_us && value.admission_deadline_us == request.admission_deadline_us;
    }
    bool Expired(const Request& request, uint64_t now_us) const {
        return expired_.load() == request.serial || now_us < request.received_us ||
            (request.admission_deadline_us && now_us >= request.admission_deadline_us) ||
            now_us - request.received_us >= kAdmissionUs;
    }
    bool Admit(const Request& request, uint32_t generation, uint32_t reset, uint64_t now_us) {
        if (request.serial == UINT32_MAX || admitted_.load() == request.serial || !generation || generation == UINT32_MAX || !reset || reset == UINT32_MAX ||
            !Current(request) || Expired(request, now_us)) return false;
        admitted_.store(0);
        generation_.store(generation);
        reset_.store(reset);
        admitted_.store(request.serial);
        return Current(request) && !Expired(request, now_us);
    }
    bool TryAdmission(const Request& request, uint64_t now_us, Admission& out) const {
        if (!request.serial || admitted_.load() != request.serial) return false;
        Admission value{generation_.load(), reset_.load()};
        if (admitted_.load() != request.serial || !Current(request) || Expired(request, now_us)) return false;
        out = value;
        return true;
    }
    void Expire(const Request& request) {
        if (Current(request)) expired_.store(request.serial);
    }
    bool Confirm(const Request& request, uint64_t now_us) {
        Admission admission;
        if (!TryAdmission(request, now_us, admission)) return false;
        confirmed_.store(request.serial);
        return true;
    }
    bool Confirmed(const Request& request) const { return confirmed_.load() == request.serial; }
private:
    uint32_t serial_ = 0;
    std::atomic<uint32_t> published_{0}, expired_{0}, admitted_{0};
    std::atomic<uint32_t> confirmed_{0};
    std::atomic<uint32_t> source_{0}, epoch_{0}, protocol_lo_{0}, protocol_hi_{0};
    std::atomic<uint32_t> connect_{0}, received_lo_{0}, received_hi_{0};
    std::atomic<uint32_t> deadline_lo_{0}, deadline_hi_{0};
    std::atomic<uint32_t> generation_{0}, reset_{0};
};
