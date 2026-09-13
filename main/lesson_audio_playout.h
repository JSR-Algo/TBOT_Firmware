#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include "audio/audio_playback_drain_snapshot.h"

// Output-task observations are owned by one response. Wire START and empty
// queues between chunks cannot publish an audible-start or completion receipt.
class LessonAudioPlayout {
public:
    struct Start { uint32_t generation; uint64_t at_ms; };
    static bool ValidId(std::string_view id) {
        if (id.size() != 32) return false;
        for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    bool Seen(std::string_view id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return id <= high_water_;
    }
    void SetScope(uint64_t protocol, uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scope_protocol_ == protocol && scope_epoch_ == epoch) return;
        scope_protocol_ = protocol;
        scope_epoch_ = epoch;
        high_water_.clear();
        generation_ = 0;
        id_.clear();
    }
    bool Begin(uint32_t generation, std::string_view id, uint64_t reset_epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!generation || !ValidId(id)) return false;
        if (id <= high_water_) return false;
        high_water_ = id;
        id_ = id;
        generation_ = generation;
        reset_epoch_ = reset_epoch;
        output_ = start_taken_ = stopped_ = failed_ = false;
        return true;
    }
    void PublishOutput(uint32_t generation, bool conversation, uint64_t at_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!generation_ || generation != generation_ || !conversation || output_ || failed_) return;
        output_ = true;
        at_ms_ = at_ms;
    }
    std::optional<Start> TakeStart() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!generation_ || !output_ || start_taken_ || failed_) return {};
        start_taken_ = true;
        return Start{generation_, at_ms_};
    }
    void PublishFailure(uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ && generation == generation_) failed_ = true;
    }
    bool Failed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_ && failed_;
    }
    bool Stop(std::string_view id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!generation_ || id != id_) return false;
        stopped_ = true;
        return true;
    }
    bool Current(std::string_view id, uint32_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_ && generation == generation_ && id == id_;
    }
    bool AllowsAudio(uint32_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !generation_ || (generation == generation_ && !stopped_);
    }
    bool Drained(const PlaybackDrainSnapshot& d) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_ && !failed_ && stopped_ && output_ && start_taken_ && !d.stopped &&
            d.reset_epoch == reset_epoch_ && d.playback_generation == generation_ &&
            !d.decode_queue_size && !d.playback_queue_size && !d.decode_in_flight &&
            !d.output_in_flight && d.codec.state == AudioOutputDrainState::Drained;
    }
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_ = 0;
        id_.clear();
    }
private:
    mutable std::mutex mutex_;
    std::string id_, high_water_;
    uint64_t scope_protocol_ = 0, scope_epoch_ = 0;
    uint32_t generation_ = 0;
    uint64_t at_ms_ = 0;
    uint64_t reset_epoch_ = 0;
    bool output_ = false, start_taken_ = false, stopped_ = false, failed_ = false;
};
