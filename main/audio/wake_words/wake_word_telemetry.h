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
        BeginPublication(feed_channel_);
        AdvanceSequence(feed_sequence_);
        FeedInterval* interval = CurrentProducerInterval(feed_channel_);
        SaturatingIncrement(interval->chunk_count);
        if (rms < interval->rms_min) {
            interval->rms_min = rms;
        }
        if (rms > interval->rms_max) {
            interval->rms_max = rms;
        }
        if (peak > interval->peak_max) {
            interval->peak_max = peak;
        }
        if (rms > speech_floor_rms) {
            SaturatingIncrement(interval->above_floor_count);
            interval->last_above_floor_us = now_us;
            interval->last_above_floor_sequence = feed_sequence_;
        }
        EndPublication(feed_channel_);
    }

    void ObserveWakeState(WakeDecisionCategory category, int model_index, int model_count) {
        BeginPublication(state_channel_);
        AdvanceSequence(state_sequence_);
        StateInterval* interval = CurrentProducerInterval(state_channel_);
        switch (category) {
            case WakeDecisionCategory::kNone:
                SaturatingIncrement(interval->state_none);
                EndPublication(state_channel_);
                return;
            case WakeDecisionCategory::kTransition:
                SaturatingIncrement(interval->state_transition);
                break;
            case WakeDecisionCategory::kDetected:
                SaturatingIncrement(interval->state_detected);
                break;
            case WakeDecisionCategory::kOther:
                SaturatingIncrement(interval->state_other);
                break;
        }

        if (model_index > 0 && model_index <= model_count) {
            interval->last_valid_model_index = model_index;
            interval->last_valid_model_sequence = state_sequence_;
        } else if (model_index > 0) {
            SaturatingIncrement(interval->invalid_model_index_count);
        }
        EndPublication(state_channel_);
    }

    WakeTelemetrySnapshot TakeSnapshot() {
        FeedInterval feed;
        StateInterval state;
        DrainOne(feed_channel_, feed_pending_slot_, feed);
        DrainOne(state_channel_, state_pending_slot_, state);

        above_floor_total_ = SaturatingAdd(above_floor_total_, feed.above_floor_count);
        if (feed.last_above_floor_sequence > last_above_floor_sequence_) {
            last_above_floor_sequence_ = feed.last_above_floor_sequence;
            last_above_floor_us_ = feed.last_above_floor_us;
        }
        if (state.last_valid_model_sequence > last_valid_model_sequence_) {
            last_valid_model_sequence_ = state.last_valid_model_sequence;
            last_valid_model_index_ = state.last_valid_model_index;
        }

        return {
            feed.chunk_count,
            feed.rms_min == kRmsMinSentinel ? 0 : feed.rms_min,
            feed.rms_max,
            feed.peak_max,
            feed.above_floor_count,
            above_floor_total_,
            last_above_floor_us_,
            state.state_none,
            state.state_transition,
            state.state_detected,
            state.state_other,
            last_valid_model_index_,
            state.invalid_model_index_count,
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

    struct FeedInterval {
        uint32_t chunk_count = 0;
        uint32_t rms_min = kRmsMinSentinel;
        uint32_t rms_max = 0;
        uint32_t peak_max = 0;
        uint32_t above_floor_count = 0;
        int64_t last_above_floor_us = 0;
        uint64_t last_above_floor_sequence = 0;
    };

    struct StateInterval {
        uint32_t state_none = 0;
        uint32_t state_transition = 0;
        uint32_t state_detected = 0;
        uint32_t state_other = 0;
        int32_t last_valid_model_index = 0;
        uint64_t last_valid_model_sequence = 0;
        uint32_t invalid_model_index_count = 0;
    };

    template <typename Interval>
    struct SpscChannel {
        Interval intervals[2];
        // Sequential consistency prevents both sides from missing a concurrent handoff.
        std::atomic<uint32_t> requested_slot{0};
        std::atomic<uint32_t> producer_active{0};
        uint32_t producer_slot = 0;
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

    template <typename Interval>
    static void BeginPublication(SpscChannel<Interval>& channel) {
        channel.producer_active.store(1);
        const uint32_t requested_slot = channel.requested_slot.load();
        if (channel.producer_slot != requested_slot) {
            channel.producer_slot = requested_slot;
        }
    }

    template <typename Interval>
    static Interval* CurrentProducerInterval(SpscChannel<Interval>& channel) {
        return &channel.intervals[channel.producer_slot];
    }

    template <typename Interval>
    static void EndPublication(SpscChannel<Interval>& channel) {
        channel.producer_active.store(0);
    }

    template <typename Interval>
    static void DrainOne(SpscChannel<Interval>& channel, int32_t& pending_slot,
                         Interval& drained) {
        if (pending_slot < 0) {
            const uint32_t old_slot = channel.requested_slot.load();
            const uint32_t new_slot = old_slot ^ 1U;
            channel.requested_slot.store(new_slot);
            pending_slot = static_cast<int32_t>(old_slot);
        }
        if (channel.producer_active.load() != 0) {
            return;
        }
        const uint32_t slot = static_cast<uint32_t>(pending_slot);
        drained = channel.intervals[slot];
        channel.intervals[slot] = Interval{};
        pending_slot = -1;
    }

    static uint32_t SaturatingAdd(uint32_t left, uint32_t right) {
        const uint32_t maximum = std::numeric_limits<uint32_t>::max();
        return right > maximum - left ? maximum : left + right;
    }

    static void SaturatingIncrement(uint32_t& value) {
        if (value != std::numeric_limits<uint32_t>::max()) {
            ++value;
        }
    }

    static void AdvanceSequence(uint64_t& sequence) {
        if (sequence != std::numeric_limits<uint64_t>::max()) {
            ++sequence;
        }
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

    SpscChannel<FeedInterval> feed_channel_;
    SpscChannel<StateInterval> state_channel_;
    uint64_t feed_sequence_ = 0;
    uint64_t state_sequence_ = 0;
    int32_t feed_pending_slot_ = -1;
    int32_t state_pending_slot_ = -1;
    uint32_t above_floor_total_ = 0;
    uint64_t last_above_floor_sequence_ = 0;
    int64_t last_above_floor_us_ = 0;
    uint64_t last_valid_model_sequence_ = 0;
    int32_t last_valid_model_index_ = 0;
};

#endif  // WAKE_WORD_TELEMETRY_H_
