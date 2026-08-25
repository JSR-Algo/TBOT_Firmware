#include "audio/wake_words/wake_word_telemetry.h"

#include <climits>
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "wake word telemetry test failed: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
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

    std::cout << "wake word telemetry test OK\n";
    return 0;
}
