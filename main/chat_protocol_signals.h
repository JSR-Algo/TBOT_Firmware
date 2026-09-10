#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include "protocols/connection_source.h"
#include "chat_playout_intake.h"
#include "chat_start_handoff.h"

// One app era writer; callbacks only publish small retained flags. The mutex
// never spans application callbacks, allocation, audio work, or transport I/O.
class ChatProtocolSignals {
public:
    ChatPlayoutIntake intake;
    ChatStartHandoff start;
    // Serialized source receiver only; application never reads or writes this.
    ChatPlayoutIntake::Response start_audio;
    uint64_t lesson_audio_epoch = 0;
    enum : uint32_t { Error = 1, Closed = 2 };
    struct Failure { ConnectionSource source; uint32_t connect_generation = 0, flags = 0; };
    bool PublishConnectionFault(ConnectionSource source, uint32_t connect_generation, uint32_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!source.Valid() || !connect_generation || source.source_id < source_id_.load() ||
            source.source_id < failure_.source.source_id) return false;
        if (failure_.source.source_id != source.source_id) failure_ = {source, connect_generation, 0};
        failure_.flags |= flags;
        failed_source_.store(source.source_id, std::memory_order_release);
        return true;
    }
    bool ReadFailure(Failure& failure) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !failure_.source.Valid() || failure_.source.source_id < source_id_.load() ||
            failure_.source.source_id < opened_source_.load()) return false;
        failure = failure_;
        return true;
    }
    struct Opened {
        ConnectionSource source;
        uint32_t connect_generation = 0;
        int sample_rate = 0;
        uint64_t deadline_us = 0;
    };
    bool PublishOpened(ConnectionSource source, uint32_t connect_generation, int sample_rate, uint64_t deadline_us = 0) {
        if (!source.Valid() || !connect_generation || source.source_id <= opened_source_.load()) return false;
        opened_published_.store(0);
        opened_source_.store(source.source_id);
        opened_epoch_.store(source.connection_epoch);
        opened_connect_.store(connect_generation);
        opened_rate_.store(static_cast<uint32_t>(sample_rate));
        opened_deadline_lo_.store(static_cast<uint32_t>(deadline_us));
        opened_deadline_hi_.store(static_cast<uint32_t>(deadline_us >> 32));
        opened_published_.store(source.source_id);
        return true;
    }
    bool ReadOpened(Opened& opened) {
        const auto published = opened_published_.load();
        if (!published) return false;
        const Opened value{{opened_source_.load(), opened_epoch_.load()}, opened_connect_.load(),
            static_cast<int>(opened_rate_.load()), uint64_t(opened_deadline_lo_.load()) | (uint64_t(opened_deadline_hi_.load()) << 32)};
        if (published != opened_published_.load()) return false;
        opened = value;
        return true;
    }
    void SelectDeferred() { deferred_.store(true, std::memory_order_release); }
    bool Deferred() const { return deferred_.load(std::memory_order_acquire); }
    bool SourceSelected() const { return source_mode_.load(std::memory_order_acquire); }
    uint32_t Capture() const {
        const auto era = era_.load(std::memory_order_acquire);
        return (era & 1U) ? 0 : era;
    }
    void Disable() {
        const auto era = era_.load(std::memory_order_relaxed);
        if (!(era & 1U)) era_.store(era + 1, std::memory_order_release);
        source_id_.store(0, std::memory_order_release);
    }
    bool Enable() {
        const auto era = era_.load(std::memory_order_relaxed);
        if (era == UINT32_MAX) return false;
        if (era & 1U) era_.store(era + 1, std::memory_order_release);
        return true;
    }
    // Application-only: caller supplies a proven source, never a late lookup.
    bool EnableForSource(ConnectionSource source) {
        Disable();
        if (!source.Valid() || source.source_id <= last_source_id_ || source.source_id <= failed_source_.load()) return false;
        last_source_id_ = source.source_id;
        source_mode_.store(true, std::memory_order_release);
        source_epoch_.store(source.connection_epoch, std::memory_order_release);
        source_id_.store(source.source_id, std::memory_order_release);
        return Enable();
    }
    bool MatchesSource(ConnectionSource source) const {
        const auto era = Capture();
        return era && source.Valid() && source.source_id > failed_source_.load(std::memory_order_acquire) && source.source_id == source_id_.load(std::memory_order_acquire) &&
            source.connection_epoch == source_epoch_.load(std::memory_order_acquire) && era == Capture();
    }
    bool TrySource(ConnectionSource& source) const {
        const ConnectionSource value{source_id_.load(std::memory_order_acquire), source_epoch_.load(std::memory_order_acquire)};
        if (!MatchesSource(value)) return false;
        source = value;
        return true;
    }
    bool PublishSource(uint32_t captured, ConnectionSource source, uint32_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!captured || Capture() != captured || !MatchesSource(source)) return false;
        if (payload_era_ != captured) flags_ = 0;
        payload_era_ = captured;
        flags_ |= flags;
        return true;
    }
    bool Publish(uint32_t captured, uint32_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!captured || Capture() != captured || source_mode_.load(std::memory_order_acquire)) return false;
        if (payload_era_ != captured) flags_ = 0;
        payload_era_ = captured;
        flags_ |= flags;
        return true;
    }
    bool Collect(uint32_t& flags) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        flags = payload_era_ == Capture() ? flags_ : 0;
        flags_ = 0;
        return true;
    }
private:
    std::atomic<uint32_t> era_{2};
    std::atomic<bool> deferred_{false};
    std::atomic<uint32_t> source_id_{0}, source_epoch_{0};
    uint32_t last_source_id_ = 0;
    std::atomic<bool> source_mode_{false};
    std::mutex mutex_;
    uint32_t payload_era_ = 0, flags_ = 0;
    std::atomic<uint32_t> opened_published_{0}, opened_source_{0}, opened_epoch_{0};
    std::atomic<uint32_t> opened_connect_{0}, opened_rate_{0}, opened_deadline_lo_{0}, opened_deadline_hi_{0};
    Failure failure_;
    std::atomic<uint32_t> failed_source_{0};
};
