#include "audio/wake_words/wake_word_telemetry.h"

#include <atomic>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <thread>

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
    Require(first.state_detected == 2, "detected state is counted");
    Require(first.state_other == 1, "other state is counted");
    Require(first.last_valid_model_index == 2, "latest valid positive model index is retained");
    Require(first.invalid_model_index_count == 1, "positive out-of-range model index is counted");

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
        WakeWordTelemetry concurrent_telemetry;
        constexpr uint32_t kObservationCount = 200000;
        const int16_t sample[] = {1000};
        std::atomic<bool> start{false};
        std::atomic<bool> producer_done{false};
        std::thread producer([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (uint32_t i = 0; i < kObservationCount; ++i) {
                concurrent_telemetry.ObserveFeedChunk(sample, 1, 100, i + 1);
            }
            producer_done.store(true, std::memory_order_release);
        });

        uint32_t observed_chunks = 0;
        uint32_t observed_above_floor = 0;
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
        } while (!producer_done.load(std::memory_order_acquire));

        const WakeTelemetrySnapshot final_snapshot = concurrent_telemetry.TakeSnapshot();
        producer.join();
        observed_chunks += final_snapshot.chunk_count;
        observed_above_floor += final_snapshot.above_floor_count;
        Require(observed_chunks == kObservationCount,
                "coherent drains account for every produced chunk exactly once");
        Require(observed_above_floor == kObservationCount,
                "coherent drains account for every above-floor chunk exactly once");
        Require(final_snapshot.above_floor_total == kObservationCount,
                "persistent totals include every drained interval");
        Require(final_snapshot.last_above_floor_us == kObservationCount,
                "persistent timestamp includes the latest drained interval");
    }

    std::cout << "wake word telemetry test OK\n";
    return 0;
}
