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

class WakeWordTelemetry {
public:
    void ObserveFeedChunk(const int16_t* samples, size_t sample_count,
                          uint32_t speech_floor_rms, int64_t now_us) {
        if (samples == nullptr || sample_count == 0) {
            return;
        }

        uint64_t mean_square = 0;
        uint64_t mean_remainder = 0;
        uint64_t observed_samples = 0;
        uint32_t peak = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            const int32_t sample = samples[i];
            const uint32_t magnitude = sample < 0
                ? static_cast<uint32_t>(-sample)
                : static_cast<uint32_t>(sample);
            if (magnitude > peak) {
                peak = magnitude;
            }

            const uint64_t square = static_cast<uint64_t>(magnitude) * magnitude;
            UpdateExactMean(square, mean_square, mean_remainder, observed_samples);
        }

        const uint32_t rms = IntegerSquareRoot(mean_square);
        chunk_count_.fetch_add(1, std::memory_order_relaxed);
        AtomicMin(rms_min_, rms);
        AtomicMax(rms_max_, rms);
        AtomicMax(peak_max_, peak);
        if (rms > speech_floor_rms) {
            above_floor_count_.fetch_add(1, std::memory_order_relaxed);
            above_floor_total_.fetch_add(1, std::memory_order_relaxed);
            last_above_floor_us_.store(now_us, std::memory_order_relaxed);
        }
    }

    void ObserveWakeState(WakeDecisionCategory category, int model_index, int model_count) {
        switch (category) {
            case WakeDecisionCategory::kNone:
                state_none_.fetch_add(1, std::memory_order_relaxed);
                return;
            case WakeDecisionCategory::kTransition:
                state_transition_.fetch_add(1, std::memory_order_relaxed);
                break;
            case WakeDecisionCategory::kDetected:
                state_detected_.fetch_add(1, std::memory_order_relaxed);
                break;
            case WakeDecisionCategory::kOther:
                state_other_.fetch_add(1, std::memory_order_relaxed);
                break;
        }

        if (model_index > 0 && model_index <= model_count) {
            last_valid_model_index_.store(model_index, std::memory_order_relaxed);
        } else if (model_index > 0) {
            invalid_model_index_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    WakeTelemetrySnapshot TakeSnapshot() {
        const uint32_t rms_min = rms_min_.exchange(kRmsMinSentinel, std::memory_order_relaxed);
        return {
            chunk_count_.exchange(0, std::memory_order_relaxed),
            rms_min == kRmsMinSentinel ? 0 : rms_min,
            rms_max_.exchange(0, std::memory_order_relaxed),
            peak_max_.exchange(0, std::memory_order_relaxed),
            above_floor_count_.exchange(0, std::memory_order_relaxed),
            above_floor_total_.load(std::memory_order_relaxed),
            last_above_floor_us_.load(std::memory_order_relaxed),
            state_none_.exchange(0, std::memory_order_relaxed),
            state_transition_.exchange(0, std::memory_order_relaxed),
            state_detected_.exchange(0, std::memory_order_relaxed),
            state_other_.exchange(0, std::memory_order_relaxed),
            last_valid_model_index_.load(std::memory_order_relaxed),
            invalid_model_index_count_.exchange(0, std::memory_order_relaxed),
        };
    }

private:
    static constexpr uint32_t kRmsMinSentinel = std::numeric_limits<uint32_t>::max();

    static void UpdateExactMean(uint64_t value, uint64_t& mean, uint64_t& remainder,
                                uint64_t& count) {
        // Keep the running sum as quotient and remainder so squared samples are never accumulated.
        if (count == 0) {
            mean = value;
            remainder = 0;
            count = 1;
            return;
        }

        const uint64_t next_count = count + 1;
        if (value >= mean) {
            const uint64_t difference = value - mean;
            mean += difference / next_count;
            const uint64_t extra_remainder = difference % next_count;
            if (remainder >= next_count - extra_remainder) {
                remainder -= next_count - extra_remainder;
                ++mean;
            } else {
                remainder += extra_remainder;
            }
        } else {
            const uint64_t difference = mean - value;
            if (remainder >= difference) {
                remainder -= difference;
            } else {
                const uint64_t deficit = difference - remainder;
                const uint64_t deficit_remainder = deficit % next_count;
                mean -= deficit / next_count + (deficit_remainder != 0);
                remainder = deficit_remainder == 0 ? 0 : next_count - deficit_remainder;
            }
        }
        count = next_count;
    }

    static uint32_t IntegerSquareRoot(uint64_t value) {
        uint32_t low = 0;
        uint32_t high = 32768;
        uint32_t result = 0;
        while (low <= high) {
            const uint32_t middle = low + (high - low) / 2;
            if (middle == 0 || middle <= value / middle) {
                result = middle;
                low = middle + 1;
            } else {
                high = middle - 1;
            }
        }
        return result;
    }

    static void AtomicMin(std::atomic<uint32_t>& target, uint32_t value) {
        uint32_t current = target.load(std::memory_order_relaxed);
        while (value < current &&
               !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    static void AtomicMax(std::atomic<uint32_t>& target, uint32_t value) {
        uint32_t current = target.load(std::memory_order_relaxed);
        while (value > current &&
               !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    std::atomic<uint32_t> chunk_count_{0};
    std::atomic<uint32_t> rms_min_{kRmsMinSentinel};
    std::atomic<uint32_t> rms_max_{0};
    std::atomic<uint32_t> peak_max_{0};
    std::atomic<uint32_t> above_floor_count_{0};
    std::atomic<uint32_t> above_floor_total_{0};
    std::atomic<int64_t> last_above_floor_us_{0};
    std::atomic<uint32_t> state_none_{0};
    std::atomic<uint32_t> state_transition_{0};
    std::atomic<uint32_t> state_detected_{0};
    std::atomic<uint32_t> state_other_{0};
    std::atomic<int32_t> last_valid_model_index_{0};
    std::atomic<uint32_t> invalid_model_index_count_{0};
};

#endif  // WAKE_WORD_TELEMETRY_H_
