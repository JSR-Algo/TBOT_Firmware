#include "speaking_arm_gesture.h"

#include <cassert>
#include <cstdint>
#include <limits>

void RequiresResponseAndPlayback() {
    SpeakingArmGesture gesture;
    assert(!gesture.Observe(1, true, true, 0));
    gesture.BeginResponse(1);
    assert(!gesture.Observe(1, true, false, 0));
    assert(!gesture.Observe(1, true, false, 5000));
    const auto target = gesture.Observe(1, true, true, 5000);
    assert(target && target->response == 1 && target->left && target->percent == 20);
}

void CadenceAndBoundedPattern() {
    SpeakingArmGesture gesture;
    gesture.BeginResponse(7);
    for (uint64_t step = 0; step < 100; ++step) {
        const auto target = gesture.Observe(7, true, true, step * 1000);
        assert(target && target->response == 7);
        assert(target->left == (step % 4 < 2));
        assert(target->percent == (step % 2 == 0 ? 20 : 0));
        assert(target->percent >= 0 && target->percent <= 20);
        assert(!gesture.Observe(7, true, true, step * 1000));
        assert(!gesture.Observe(7, true, true, step * 1000 + 999));
    }
}

void LatePollDoesNotCatchUp() {
    SpeakingArmGesture gesture;
    gesture.BeginResponse(1);
    assert(gesture.Observe(1, true, true, 0));
    const auto target = gesture.Observe(1, true, true, 10000);
    assert(target && target->left && target->percent == 0);
    assert(!gesture.Observe(1, true, true, 10000));
    assert(!gesture.Observe(1, true, true, 10999));
    assert(gesture.Observe(1, true, true, 11000));
}

void ReplacementPreservesGlobalCadenceAndRejectsStaleWork() {
    SpeakingArmGesture gesture;
    gesture.BeginResponse(1);
    assert(gesture.Observe(1, true, true, 300));
    gesture.BeginResponse(2);
    assert(!gesture.Observe(2, true, true, 301));
    assert(!gesture.Observe(1, false, true, 1300));
    gesture.Cancel(1);
    const auto target = gesture.Observe(2, true, true, 1300);
    assert(target && target->response == 2 && target->left && target->percent == 20);
    assert(!gesture.Observe(1, true, true, 2300));
}

void ExplicitCancellationCannotRearmSameResponse() {
    SpeakingArmGesture gesture;
    gesture.BeginResponse(1);
    assert(gesture.Observe(1, true, true, 0));
    gesture.Cancel(1);
    assert(!gesture.Observe(1, true, true, 1000));
    assert(!gesture.Observe(1, true, false, 2000));
    gesture.BeginResponse(1);
    assert(!gesture.Observe(1, true, true, 3000));
    gesture.BeginResponse(2);
    assert(gesture.Observe(2, true, true, 3000));
}

void StateDepartureInvalidatesResponse() {
    SpeakingArmGesture gesture;
    gesture.BeginResponse(1);
    assert(!gesture.Observe(1, false, true, 0));
    assert(!gesture.Observe(1, true, true, 1000));
    gesture.BeginResponse(2);
    assert(gesture.Observe(2, true, true, 1000));
    assert(!gesture.Observe(2, false, false, 2000));
    assert(!gesture.Observe(2, true, true, 3000));
}

void PlaybackPauseDoesNotGenerateTargets() {
    SpeakingArmGesture gesture;
    gesture.BeginResponse(1);
    assert(gesture.Observe(1, true, true, 0));
    assert(!gesture.Observe(1, true, false, 1000));
    assert(!gesture.Observe(1, true, false, 9000));
    const auto target = gesture.Observe(1, true, true, 10000);
    assert(target && target->left && target->percent == 0);
}

void ClockBoundaryDoesNotOverflowOrRunBackwards() {
    SpeakingArmGesture gesture;
    gesture.BeginResponse(0);
    const uint64_t start = std::numeric_limits<uint64_t>::max() - 1000;
    assert(gesture.Observe(0, true, true, start));
    assert(!gesture.Observe(0, true, true, start - 1));
    assert(!gesture.Observe(0, true, true, start + 999));
    assert(gesture.Observe(0, true, true, start + 1000));
    assert(!gesture.Observe(0, true, true, 0));
}

int main() {
    RequiresResponseAndPlayback();
    CadenceAndBoundedPattern();
    LatePollDoesNotCatchUp();
    ReplacementPreservesGlobalCadenceAndRejectsStaleWork();
    ExplicitCancellationCannotRearmSameResponse();
    StateDepartureInvalidatesResponse();
    PlaybackPauseDoesNotGenerateTargets();
    ClockBoundaryDoesNotOverflowOrRunBackwards();
}
