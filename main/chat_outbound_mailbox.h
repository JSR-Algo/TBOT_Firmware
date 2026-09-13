#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <string>
#include "protocols/connection_source.h"

// Application alone advances generations; one worker takes/completes jobs.
// Callers own retries on contention. No mailbox lock spans caller I/O.
class ChatOutboundMailbox {
public:
    enum class Kind { ListenStart, ListenStop, Abort, Wake, DrainAck, FullText };
    enum class Result { Sent, Busy, Stale, Failed };
    static constexpr size_t kMaxPayloadSize = 128;
    static constexpr uint32_t kExhausted = std::numeric_limits<uint32_t>::max();

    struct Job {
        uint64_t request_id = 0;
        uint32_t generation = 0;
        uint64_t protocol_generation = 0;
        uint64_t deadline_us = 0;
        uint32_t connection_epoch = 0;
        Kind kind = Kind::ListenStart;
        int32_t argument = 0;
        std::array<char, kMaxPayloadSize + 1> payload{};
        size_t payload_size = 0;
        ConnectionSource source;
        uint32_t connect_generation = 0;
        std::shared_ptr<const std::string> full_text;
        std::shared_ptr<std::atomic<bool>> authorization;

        // Accept up to 128 text bytes plus an owned terminator. Failure preserves
        // the existing payload; nullptr is allowed only for empty text.
        bool SetPayload(const char* text, size_t size) {
            if (size > kMaxPayloadSize || (size != 0 && text == nullptr) ||
                (size != 0 && std::memchr(text, '\0', size) != nullptr)) {
                return false;
            }
            if (size != 0) {
                std::memmove(payload.data(), text, size);
            }
            payload[size] = '\0';
            payload_size = size;
            return true;
        }
    };

    struct Completion {
        Job job{};
        Result result = Result::Failed;
        // Snapshot at collection, not authorization for later external effects.
        bool stale = false;
    };

    // Single writer, atomic32 load/store only: invalidation never waits for the
    // queue mutex. Exhaustion permanently revokes all identities without wrap.
    uint32_t AdvanceGeneration() {
        const auto current = generation_.load(std::memory_order_relaxed);
        const auto next = current == kExhausted ? kExhausted : current + 1;
        generation_.store(next, std::memory_order_release);
        return next;
    }

    bool IsCurrent(uint32_t generation) const {
        return generation != 0 && generation != kExhausted &&
               generation_.load(std::memory_order_acquire) == generation;
    }

    bool TrySubmit(const Job& job) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !Valid(job) || !IsCurrent(job.generation)) {
            return false;
        }
        if ((active_present_ && active_.request_id == job.request_id) ||
            (completion_present_ && completion_.job.request_id == job.request_id)) {
            return false;
        }
        for (size_t i = 0; i < queued_; ++i) {
            if (queue_[i].request_id == job.request_id) {
                return false;
            }
        }
        CompactQueue();
        if (queued_ == queue_.size()) {
            return false;
        }
        queue_[queued_++] = job;
        return true;
    }

    // Taking reserves ownership only. Revalidate before external effects:
    // cancellation may race with this call and cannot retract submitted bytes.
    bool TryTake(Job& output) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || active_present_ || completion_present_) {
            return false;
        }
        CompactQueue();
        if (queued_ == 0) {
            return false;
        }
        active_ = queue_[0];
        active_present_ = true;
        if (--queued_ != 0) {
            queue_[0] = queue_[1];
        }
        output = active_;
        return true;
    }

    bool TryComplete(const Job& job, Result result) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !active_present_ || completion_present_ ||
            !Matches(active_, job)) {
            return false;
        }
        completion_ = Completion{active_, result, false};
        completion_present_ = true;
        active_present_ = false;
        return true;
    }

    bool TryCollect(Completion& output) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !completion_present_) {
            return false;
        }
        output = completion_;
        output.stale = !IsCurrent(output.job.generation);
        completion_present_ = false;
        return true;
    }

    bool TryIdle() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        CompactQueue();
        return queued_ == 0 && !active_present_ && !completion_present_;
    }

private:
    static bool Valid(const Job& job) {
        return job.request_id != 0 && job.protocol_generation != 0 &&
               job.connection_epoch != 0 && job.kind >= Kind::ListenStart &&
               job.kind <= Kind::FullText &&
               (job.kind != Kind::FullText || (job.source.Valid() && job.connect_generation &&
                job.full_text && !job.full_text->empty() && job.full_text->size() <= 65535)) &&
               job.payload_size <= kMaxPayloadSize &&
               job.payload[job.payload_size] == '\0' &&
               std::memchr(job.payload.data(), '\0', job.payload_size) == nullptr;
    }

    static bool Matches(const Job& active, const Job& job) {
        return Valid(job) && active.request_id == job.request_id &&
               active.generation == job.generation &&
               active.protocol_generation == job.protocol_generation &&
               active.deadline_us == job.deadline_us &&
               active.connection_epoch == job.connection_epoch &&
               active.kind == job.kind && active.argument == job.argument &&
               active.source.source_id == job.source.source_id && active.source.connection_epoch == job.source.connection_epoch &&
               active.connect_generation == job.connect_generation && active.full_text == job.full_text &&
               active.payload_size == job.payload_size &&
               std::memcmp(active.payload.data(), job.payload.data(),
                           active.payload_size) == 0;
    }

    void CompactQueue() {
        size_t retained = 0;
        for (size_t i = 0; i < queued_; ++i) {
            if (IsCurrent(queue_[i].generation)) {
                queue_[retained++] = queue_[i];
            }
        }
        queued_ = retained;
    }

    // Target disassembly tests qualify these load/store operations specifically;
    // Xtensa's general atomic trait also covers unsupported read-modify-write.
    std::atomic<uint32_t> generation_{0};
    std::mutex mutex_;
    std::array<Job, 2> queue_{};
    size_t queued_ = 0;
    Job active_{};
    Completion completion_{};
    bool active_present_ = false;
    bool completion_present_ = false;
};
