#pragma once

#include <cstdint>
#include <optional>

// Single-owner helper: callers serialize access and fence stale response IDs
// before BeginResponse. IDs are identities, not numerically ordered counters.
class SpeakingArmGesture {
public:
    static constexpr uint64_t kIntervalMs = 1000;

    struct Target {
        uint32_t response;
        bool left;
        int percent;
    };

    void BeginResponse(uint32_t response) {
        if (has_response_ && response_ == response) return;
        has_response_ = true;
        response_ = response;
        cancelled_ = false;
        step_ = 0;
    }

    void Cancel(uint32_t response) {
        if (has_response_ && response_ == response) cancelled_ = true;
    }

    // Ineligibility invalidates the response. Playback gaps only pause it;
    // completion, interruption and explicit commands must call Cancel.
    std::optional<Target> Observe(uint32_t response, bool eligible,
                                  bool playback, uint64_t now_ms) {
        if (!has_response_ || response_ != response || cancelled_) {
            return std::nullopt;
        }
        if (!eligible) {
            Cancel(response);
            return std::nullopt;
        }
        if (!playback) return std::nullopt;
        if (has_target_ && (now_ms < last_target_ms_ ||
                           now_ms - last_target_ms_ < kIntervalMs)) {
            return std::nullopt;
        }

        // Left 100, left 0, right 100, right 0; late polls advance only once.
        const Target target{response_, step_ < 2, step_ % 2 == 0 ? 100 : 0};
        step_ = (step_ + 1) % 4;
        has_target_ = true;
        last_target_ms_ = now_ms;
        return target;
    }

private:
    uint32_t response_ = 0;
    uint64_t last_target_ms_ = 0;
    uint8_t step_ = 0;
    bool has_response_ = false;
    bool has_target_ = false;
    bool cancelled_ = false;
};
