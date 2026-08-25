#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#define private public
#include "audio/wake_words/wake_word_telemetry.h"
#undef private

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "wake word telemetry test failed: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    {
        WakeWordTelemetry empty_telemetry;
        const int16_t at_floor[] = {100, -100};
        empty_telemetry.ObserveFeedChunk(nullptr, 2, 100, 1000);
        empty_telemetry.ObserveFeedChunk(at_floor, 0, 100, 2000);
        empty_telemetry.ObserveFeedChunk(at_floor, 2, 100, 3000);
        const WakeTelemetrySnapshot empty = empty_telemetry.TakeSnapshot();
        Require(empty.chunk_count == 1, "null and empty chunks are ignored");
        Require(empty.rms_min == 100 && empty.rms_max == 100,
                "an exact-floor chunk still contributes interval energy");
        Require(empty.above_floor_count == 0 && empty.above_floor_total == 0,
                "an exact-floor chunk is not above the speech floor");
        Require(empty.last_above_floor_us == 0,
                "ignored and exact-floor chunks do not publish a timestamp");
    }

    WakeWordTelemetry telemetry;
    const int16_t quiet[] = {0, 0, 0, 0};
    const int16_t speech[] = {-3000, 4000, -3000, 4000};
    const int16_t extremes[] = {INT16_MIN, INT16_MAX};

    telemetry.ObserveFeedChunk(quiet, 4, 100, 1000);
    telemetry.ObserveFeedChunk(speech, 4, 100, 2000);
    telemetry.ObserveFeedChunk(extremes, 2, 100, 3000);
    telemetry.ObserveWakeState(WakeDecisionCategory::kNone, 0, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kTransition, 0, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kDetected, 2, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kDetected, 4, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kDetected, 0, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kDetected, -1, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kOther, 0, 3);

    const WakeTelemetrySnapshot first = telemetry.TakeSnapshot();
    Require(first.chunk_count == 3, "snapshot reports observed chunks");
    Require(first.rms_min == 0, "snapshot reports quiet minimum RMS");
    Require(first.rms_max == 32767, "RMS handles full-scale extremes safely");
    Require(first.peak_max == 32768, "peak handles INT16_MIN safely");
    Require(first.above_floor_count == 2, "snapshot counts chunks above the speech floor");
    Require(first.above_floor_total == 2, "lifetime above-floor count is retained");
    Require(first.last_above_floor_us == 3000, "latest above-floor timestamp is retained");
    Require(first.state_none == 1, "none state is counted");
    Require(first.state_transition == 1, "transition state is counted");
    Require(first.state_detected == 4, "detected state is counted");
    Require(first.state_other == 1, "other state is counted");
    Require(first.last_valid_model_index == 2, "latest valid positive model index is retained");
    Require(first.invalid_model_index_count == 3,
            "every unavailable or out-of-range detected model index is counted");

    const WakeTelemetrySnapshot second = telemetry.TakeSnapshot();
    Require(second.chunk_count == 0, "chunk count drains after a snapshot");
    Require(second.rms_min == 0, "untouched RMS minimum maps to zero");
    Require(second.rms_max == 0, "RMS maximum drains after a snapshot");
    Require(second.peak_max == 0, "peak maximum drains after a snapshot");
    Require(second.above_floor_count == 0, "interval above-floor count drains after a snapshot");
    Require(second.state_none == 0, "none count drains after a snapshot");
    Require(second.state_transition == 0, "transition count drains after a snapshot");
    Require(second.state_detected == 0, "detected count drains after a snapshot");
    Require(second.state_other == 0, "other count drains after a snapshot");
    Require(second.invalid_model_index_count == 0, "invalid model count drains after a snapshot");
    Require(second.above_floor_total == 2, "lifetime above-floor count persists");
    Require(second.last_above_floor_us == 3000, "latest above-floor timestamp persists");
    Require(second.last_valid_model_index == 2, "latest valid model index persists");

    {
        WakeWordTelemetry block_telemetry;
        std::vector<int16_t> cross_block_samples(1025, 4096);
        cross_block_samples.back() = 4095;
        block_telemetry.ObserveFeedChunk(
            cross_block_samples.data(), cross_block_samples.size(), 0, 1);
        const WakeTelemetrySnapshot block_snapshot = block_telemetry.TakeSnapshot();
        Require(block_snapshot.rms_min == 4095 && block_snapshot.rms_max == 4095,
                "cross-block mean-square combination preserves exact RMS truncation");
    }

    {
        WakeWordTelemetry collision_telemetry;
        auto& channel = collision_telemetry.feed_channel_;
        auto& older = channel.intervals[0];
        older.chunk_count = 2;
        older.rms_min = 100;
        older.rms_max = 200;
        older.peak_max = 200;
        older.above_floor_count = 2;
        older.last_above_floor_us = 200;
        older.last_above_floor_sequence = 2;

        // Reproduce a producer that is active before it reads the requested slot.
        channel.producer_active.store(1);
        const WakeTelemetrySnapshot collided = collision_telemetry.TakeSnapshot();
        Require(collided.chunk_count == 0,
                "a snapshot never waits for an active feed producer");

        channel.producer_slot.store(channel.requested_slot.load());
        auto& produced = channel.intervals[channel.producer_slot.load()];
        produced.chunk_count = 1;
        produced.rms_min = 300;
        produced.rms_max = 300;
        produced.peak_max = 300;
        produced.above_floor_count = 1;
        produced.last_above_floor_us = 300;
        produced.last_above_floor_sequence = 3;
        channel.producer_active.store(0);

        const WakeTelemetrySnapshot next = collision_telemetry.TakeSnapshot();
        Require(next.chunk_count == 1 && next.rms_min == 300,
                "the first successful retry drains the collided producer interval");
        const WakeTelemetrySnapshot older_pending = collision_telemetry.TakeSnapshot();
        Require(older_pending.chunk_count == 2 && older_pending.rms_min == 100 &&
                    older_pending.rms_max == 200,
                "a different older pending interval drains exactly once after the collision");
        const WakeTelemetrySnapshot empty_after_collision =
            collision_telemetry.TakeSnapshot();
        Require(empty_after_collision.chunk_count == 0,
                "repeated drains do not duplicate collided or older intervals");
        Require(next.above_floor_total == 1 && older_pending.above_floor_total == 3 &&
                    empty_after_collision.above_floor_total == 3,
                "collision recovery accounts for every feed observation exactly once");
        Require(empty_after_collision.last_above_floor_us == 300,
                "collision recovery retains the newest aggregate timestamp");
    }

    {
        WakeWordTelemetry concurrent_telemetry;
        constexpr uint32_t kObservationCount = 100000;
        const int16_t sample[] = {1000};
        std::atomic<bool> start{false};
        std::atomic<bool> feed_done{false};
        std::atomic<bool> state_done{false};
        std::thread feed_producer([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (uint32_t i = 0; i < kObservationCount; ++i) {
                concurrent_telemetry.ObserveFeedChunk(sample, 1, 100, i + 1);
            }
            feed_done.store(true, std::memory_order_release);
        });
        std::thread state_producer([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (uint32_t i = 0; i < kObservationCount; ++i) {
                switch (i % 5) {
                    case 0:
                        concurrent_telemetry.ObserveWakeState(WakeDecisionCategory::kNone, 0, 3);
                        break;
                    case 1:
                        concurrent_telemetry.ObserveWakeState(
                            WakeDecisionCategory::kTransition, 0, 3);
                        break;
                    case 2:
                        concurrent_telemetry.ObserveWakeState(
                            WakeDecisionCategory::kDetected, 2, 3);
                        break;
                    case 3:
                        concurrent_telemetry.ObserveWakeState(
                            WakeDecisionCategory::kDetected, 4, 3);
                        break;
                    case 4:
                        concurrent_telemetry.ObserveWakeState(WakeDecisionCategory::kOther, 0, 3);
                        break;
                }
            }
            state_done.store(true, std::memory_order_release);
        });

        uint32_t observed_chunks = 0;
        uint32_t observed_above_floor = 0;
        uint32_t observed_none = 0;
        uint32_t observed_transition = 0;
        uint32_t observed_detected = 0;
        uint32_t observed_other = 0;
        uint32_t observed_invalid = 0;
        int32_t last_valid_model_index = 0;
        start.store(true, std::memory_order_release);
        do {
            const WakeTelemetrySnapshot snapshot = concurrent_telemetry.TakeSnapshot();
            Require(snapshot.chunk_count == snapshot.above_floor_count,
                    "a snapshot does not split one chunk across interval counters");
            if (snapshot.chunk_count == 0) {
                Require(snapshot.rms_min == 0 && snapshot.rms_max == 0 &&
                            snapshot.peak_max == 0,
                        "an empty interval has no extrema");
            } else {
                Require(snapshot.rms_min == 1000 && snapshot.rms_max == 1000 &&
                            snapshot.peak_max == 1000,
                        "a non-empty interval contains all extrema from its chunks");
            }
            observed_chunks += snapshot.chunk_count;
            observed_above_floor += snapshot.above_floor_count;
            observed_none += snapshot.state_none;
            observed_transition += snapshot.state_transition;
            observed_detected += snapshot.state_detected;
            observed_other += snapshot.state_other;
            observed_invalid += snapshot.invalid_model_index_count;
            if (snapshot.last_valid_model_index > 0) {
                last_valid_model_index = snapshot.last_valid_model_index;
            }
        } while (!feed_done.load(std::memory_order_acquire) ||
                 !state_done.load(std::memory_order_acquire));

        feed_producer.join();
        state_producer.join();
        const WakeTelemetrySnapshot final_snapshot = concurrent_telemetry.TakeSnapshot();
        const WakeTelemetrySnapshot deferred_snapshot = concurrent_telemetry.TakeSnapshot();
        observed_chunks += final_snapshot.chunk_count;
        observed_chunks += deferred_snapshot.chunk_count;
        observed_above_floor += final_snapshot.above_floor_count;
        observed_above_floor += deferred_snapshot.above_floor_count;
        observed_none += final_snapshot.state_none;
        observed_none += deferred_snapshot.state_none;
        observed_transition += final_snapshot.state_transition;
        observed_transition += deferred_snapshot.state_transition;
        observed_detected += final_snapshot.state_detected;
        observed_detected += deferred_snapshot.state_detected;
        observed_other += final_snapshot.state_other;
        observed_other += deferred_snapshot.state_other;
        observed_invalid += final_snapshot.invalid_model_index_count;
        observed_invalid += deferred_snapshot.invalid_model_index_count;
        if (final_snapshot.last_valid_model_index > 0) {
            last_valid_model_index = final_snapshot.last_valid_model_index;
        }
        if (deferred_snapshot.last_valid_model_index > 0) {
            last_valid_model_index = deferred_snapshot.last_valid_model_index;
        }
        Require(observed_chunks == kObservationCount,
                "coherent drains account for every produced chunk exactly once");
        Require(observed_above_floor == kObservationCount,
                "coherent drains account for every above-floor chunk exactly once");
        Require(deferred_snapshot.above_floor_total == kObservationCount,
                "persistent totals include every drained interval");
        Require(deferred_snapshot.last_above_floor_us == kObservationCount,
                "persistent timestamp includes the latest drained interval");
        Require(observed_none == kObservationCount / 5,
                "concurrent drains preserve every none state");
        Require(observed_transition == kObservationCount / 5,
                "concurrent drains preserve every transition state");
        Require(observed_detected == 2 * kObservationCount / 5,
                "concurrent drains preserve every detected state");
        Require(observed_other == kObservationCount / 5,
                "concurrent drains preserve every other state");
        Require(observed_invalid == kObservationCount / 5,
                "concurrent drains preserve every invalid model index");
        Require(last_valid_model_index == 2,
                "concurrent drains preserve the latest valid model index");
    }

    std::cout << "wake word telemetry test OK\n";
    return 0;
}
