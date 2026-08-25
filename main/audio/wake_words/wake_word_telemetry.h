#ifndef WAKE_WORD_TELEMETRY_H_
#define WAKE_WORD_TELEMETRY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

enum class WakeDecisionCategory : uint8_t {
    kNone,
    kTransition,
    kDetected,
    kOther,
};

struct WakeTelemetrySnapshot {
    uint32_t chunk_count;
    uint32_t rms_min;
    uint32_t rms_max;
    uint32_t peak_max;
    uint32_t above_floor_count;
    uint32_t above_floor_total;
    int64_t last_above_floor_us;
    uint32_t state_none;
    uint32_t state_transition;
    uint32_t state_detected;
    uint32_t state_other;
    int32_t last_valid_model_index;
    uint32_t invalid_model_index_count;
};

// One producer records feed/state observations and one consumer takes snapshots.
class WakeWordTelemetry {
public:
    void ObserveFeedChunk(const int16_t* samples, size_t sample_count,
                          uint32_t speech_floor_rms, int64_t now_us) {
        if (samples == nullptr || sample_count == 0) {
            return;
        }

        uint64_t total_square = 0;
        uint64_t total_samples = 0;
        uint64_t block_square = 0;
        uint32_t block_samples = 0;
        uint32_t peak = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            const int32_t sample = samples[i];
            const uint32_t magnitude = sample < 0
                ? static_cast<uint32_t>(-sample)
                : static_cast<uint32_t>(sample);
            if (magnitude > peak) {
                peak = magnitude;
            }
            block_square += static_cast<uint64_t>(magnitude) * magnitude;
            ++block_samples;
            if (block_samples == kMeanSquareBlockSamples) {
                FoldBlock(block_square, block_samples, total_square, total_samples);
                block_square = 0;
                block_samples = 0;
            }
        }
        if (block_samples != 0) {
            FoldBlock(block_square, block_samples, total_square, total_samples);
        }

        const uint32_t rms = IntegerSquareRoot(total_square / total_samples);
        const uint32_t slot = AcquireWriteSlot();
        IntervalData& interval = intervals_[slot];
        ++interval.chunk_count;
        if (rms < interval.rms_min) {
            interval.rms_min = rms;
        }
        if (rms > interval.rms_max) {
            interval.rms_max = rms;
        }
        if (peak > interval.peak_max) {
            interval.peak_max = peak;
        }
        if (rms > speech_floor_rms) {
            ++interval.above_floor_count;
            interval.last_above_floor_us = now_us;
        }
        ReleaseWriteSlot(slot);
    }

    void ObserveWakeState(WakeDecisionCategory category, int model_index, int model_count) {
        const uint32_t slot = AcquireWriteSlot();
        IntervalData& interval = intervals_[slot];
        switch (category) {
            case WakeDecisionCategory::kNone:
                ++interval.state_none;
                ReleaseWriteSlot(slot);
                return;
            case WakeDecisionCategory::kTransition:
                ++interval.state_transition;
                break;
            case WakeDecisionCategory::kDetected:
                ++interval.state_detected;
                break;
            case WakeDecisionCategory::kOther:
                ++interval.state_other;
                break;
        }

        if (model_index > 0 && model_index <= model_count) {
            interval.last_valid_model_index = model_index;
        } else if (model_index > 0) {
            ++interval.invalid_model_index_count;
        }
        ReleaseWriteSlot(slot);
    }

    WakeTelemetrySnapshot TakeSnapshot() {
        const uint32_t drained_slot = active_slot_.load();
        active_slot_.store(drained_slot ^ 1U);
        while (writer_active_[drained_slot].load() != 0) {
        }

        const IntervalData interval = intervals_[drained_slot];
        intervals_[drained_slot] = IntervalData{};
        above_floor_total_ += interval.above_floor_count;
        if (interval.above_floor_count != 0) {
            last_above_floor_us_ = interval.last_above_floor_us;
        }
        if (interval.last_valid_model_index > 0) {
            last_valid_model_index_ = interval.last_valid_model_index;
        }

        return {
            interval.chunk_count,
            interval.rms_min == kRmsMinSentinel ? 0 : interval.rms_min,
            interval.rms_max,
            interval.peak_max,
            interval.above_floor_count,
            above_floor_total_,
            last_above_floor_us_,
            interval.state_none,
            interval.state_transition,
            interval.state_detected,
            interval.state_other,
            last_valid_model_index_,
            interval.invalid_model_index_count,
        };
    }

private:
    static constexpr uint32_t kRmsMinSentinel = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t kMeanSquareBlockSamples = 1024;
    static constexpr uint64_t kMaximumSampleSquare = uint64_t{32768} * 32768;
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "telemetry handoff requires native 32-bit atomics");
    static_assert(kMeanSquareBlockSamples <=
                      std::numeric_limits<uint64_t>::max() / kMaximumSampleSquare,
                  "mean-square block accumulation must not overflow");

    struct IntervalData {
        uint32_t chunk_count = 0;
        uint32_t rms_min = kRmsMinSentinel;
        uint32_t rms_max = 0;
        uint32_t peak_max = 0;
        uint32_t above_floor_count = 0;
        int64_t last_above_floor_us = 0;
        uint32_t state_none = 0;
        uint32_t state_transition = 0;
        uint32_t state_detected = 0;
        uint32_t state_other = 0;
        int32_t last_valid_model_index = 0;
        uint32_t invalid_model_index_count = 0;
    };

    static void FoldBlock(uint64_t block_square, uint32_t block_samples,
                          uint64_t& total_square, uint64_t& total_samples) {
        // Scaling is unreachable for bounded ESP-SR chunks, but keeps arbitrary size_t safe.
        while (total_square > std::numeric_limits<uint64_t>::max() - block_square ||
               total_samples > std::numeric_limits<uint64_t>::max() - block_samples) {
            total_square >>= 1;
            total_samples >>= 1;
        }
        total_square += block_square;
        total_samples += block_samples;
    }

    static uint32_t IntegerSquareRoot(uint64_t value) {
        uint32_t low = 0;
        uint32_t high = 32768;
        uint32_t result = 0;
        while (low <= high) {
            const uint32_t middle = low + (high - low) / 2;
            if (static_cast<uint64_t>(middle) * middle <= value) {
                result = middle;
                low = middle + 1;
            } else {
                high = middle - 1;
            }
        }
        return result;
    }

    uint32_t AcquireWriteSlot() {
        while (true) {
            const uint32_t slot = active_slot_.load();
            writer_active_[slot].store(1);
            if (active_slot_.load() == slot) {
                return slot;
            }
            writer_active_[slot].store(0);
        }
    }

    void ReleaseWriteSlot(uint32_t slot) {
        writer_active_[slot].store(0);
    }

    IntervalData intervals_[2];
    std::atomic<uint32_t> active_slot_{0};
    std::atomic<uint32_t> writer_active_[2] = {0, 0};
    uint32_t above_floor_total_ = 0;
    int64_t last_above_floor_us_ = 0;
    int32_t last_valid_model_index_ = 0;
};

#endif  // WAKE_WORD_TELEMETRY_H_
