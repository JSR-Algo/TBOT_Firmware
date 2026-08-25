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

// Feed/state producers and the snapshot consumer serialize only aggregate publication.
class WakeWordTelemetry {
public:
    void ObserveFeedChunk(const int16_t* samples, size_t sample_count,
                          uint32_t speech_floor_rms, int64_t now_us) {
        if (samples == nullptr || sample_count == 0) {
            return;
        }

        ExactMeanSquare mean_square;
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
                mean_square.AddBlock(block_square, block_samples);
                block_square = 0;
                block_samples = 0;
            }
        }
        if (block_samples != 0) {
            mean_square.AddBlock(block_square, block_samples);
        }

        const uint32_t rms = IntegerSquareRoot(mean_square.value());
        PublicationGuard guard(publication_guard_);
        ++interval_.chunk_count;
        if (rms < interval_.rms_min) {
            interval_.rms_min = rms;
        }
        if (rms > interval_.rms_max) {
            interval_.rms_max = rms;
        }
        if (peak > interval_.peak_max) {
            interval_.peak_max = peak;
        }
        if (rms > speech_floor_rms) {
            ++interval_.above_floor_count;
            interval_.last_above_floor_us = now_us;
        }
    }

    void ObserveWakeState(WakeDecisionCategory category, int model_index, int model_count) {
        PublicationGuard guard(publication_guard_);
        switch (category) {
            case WakeDecisionCategory::kNone:
                ++interval_.state_none;
                return;
            case WakeDecisionCategory::kTransition:
                ++interval_.state_transition;
                break;
            case WakeDecisionCategory::kDetected:
                ++interval_.state_detected;
                break;
            case WakeDecisionCategory::kOther:
                ++interval_.state_other;
                break;
        }

        if (model_index > 0 && model_index <= model_count) {
            interval_.last_valid_model_index = model_index;
        } else if (model_index > 0) {
            ++interval_.invalid_model_index_count;
        }
    }

    WakeTelemetrySnapshot TakeSnapshot() {
        PublicationGuard guard(publication_guard_);
        const IntervalData interval = interval_;
        interval_ = IntervalData{};
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
    static_assert(sizeof(size_t) <= sizeof(uint64_t), "sample counts must fit in uint64_t");
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

    class ExactMeanSquare {
    public:
        void AddBlock(uint64_t square_sum, uint32_t sample_count) {
            // mean_ * count_ + remainder_ is the exact accumulated square sum.
            if (count_ == 0) {
                mean_ = square_sum / sample_count;
                remainder_ = square_sum % sample_count;
                count_ = sample_count;
                return;
            }

            const uint64_t next_count = count_ + sample_count;
            const uint64_t baseline_square = mean_ * sample_count;
            if (square_sum >= baseline_square) {
                AddCorrection(square_sum - baseline_square, next_count);
            } else {
                SubtractCorrection(baseline_square - square_sum, next_count);
            }
            count_ = next_count;
        }

        uint64_t value() const {
            return mean_;
        }

    private:
        void AddCorrection(uint64_t correction, uint64_t divisor) {
            mean_ += correction / divisor;
            const uint64_t correction_remainder = correction % divisor;
            if (correction_remainder != 0 &&
                remainder_ >= divisor - correction_remainder) {
                remainder_ -= divisor - correction_remainder;
                ++mean_;
            } else {
                remainder_ += correction_remainder;
            }
        }

        void SubtractCorrection(uint64_t correction, uint64_t divisor) {
            mean_ -= correction / divisor;
            const uint64_t correction_remainder = correction % divisor;
            if (remainder_ >= correction_remainder) {
                remainder_ -= correction_remainder;
            } else {
                --mean_;
                remainder_ = divisor - (correction_remainder - remainder_);
            }
        }

        uint64_t mean_ = 0;
        uint64_t remainder_ = 0;
        uint64_t count_ = 0;
    };

    class PublicationGuard {
    public:
        explicit PublicationGuard(std::atomic_flag& flag) : flag_(flag) {
            while (flag_.test_and_set(std::memory_order_acquire)) {
            }
        }

        ~PublicationGuard() {
            flag_.clear(std::memory_order_release);
        }

        PublicationGuard(const PublicationGuard&) = delete;
        PublicationGuard& operator=(const PublicationGuard&) = delete;

    private:
        std::atomic_flag& flag_;
    };

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

    IntervalData interval_;
    std::atomic_flag publication_guard_ = ATOMIC_FLAG_INIT;
    uint32_t above_floor_total_ = 0;
    int64_t last_above_floor_us_ = 0;
    int32_t last_valid_model_index_ = 0;
};

#endif  // WAKE_WORD_TELEMETRY_H_
