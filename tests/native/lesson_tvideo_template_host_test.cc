#include "lesson_tvideo_template.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "tvideo template test FAILED: " << message << "\n"; std::exit(1); }
}
}

int main() {
    using namespace lesson_tvideo;
    require(IsSupported("tvideoFlyWalk", 1, "centerRoad", 1), "centerRoad v1 supported");
    require(!IsSupported("tvideoFlyWalk", 2, "centerRoad", 1), "unknown template version rejected");
    require(!IsSupported("tvideoFlyWalk", 1, "freeform", 1), "raw/freeform layout rejected");
    require(PhaseCount() == 8, "published phase contract has exactly eight phases");
    require(std::string(PhaseName(0)) == "hidden" && PhaseDurationMs(0) == 100,
            "published phase contract starts hidden for 100ms");
    require(std::string(PhaseName(7)) == "revealTeachingContent" && PhaseDurationMs(7) == 100,
            "published phase contract ends at revealTeachingContent for 100ms");
    uint8_t parsed = 0;
    require(ExactVersion(1.0, &parsed) && parsed == 1, "exact integer version accepted");
    require(!ExactVersion(1.5, &parsed), "fractional version rejected");
    require(!ExactVersion(257.0, &parsed), "wrapped version rejected");
    require(!ExactVersion(std::numeric_limits<double>::infinity(), &parsed), "non-finite version rejected");

    for (const char* preset : {"centerRoad", "leftApproach", "rightApproach"}) {
        const LayoutGeometry* geometry = ArrivedGeometry(preset, 1);
        require(geometry != nullptr, "reviewed geometry exists");
        require(!Overlaps(geometry->robot, geometry->teaching_object), "robot does not obscure teaching object");
        require(!Overlaps(geometry->robot, geometry->word_pill), "robot does not obscure word pill");
    }

    StateMachine machine({"tvideoFlyWalk", 1, "centerRoad", 1, true, true, false});
    require(machine.phase() == Phase::kHidden, "starts hidden");
    machine.Advance(100);
    require(machine.phase() == Phase::kFlyIn, "advances to flyIn");
    machine.Advance(1200);
    require(machine.phase() == Phase::kLandFar, "advances to landFar");
    machine.Advance(700 + 350 + 1800 + 250 + 650 + 100);
    require(machine.phase() == Phase::kRevealTeachingContent, "terminates at revealTeachingContent");
    require(machine.content_visible(), "content visible after terminal phase");
    require(machine.geometry().robot.width > 0 && machine.geometry().robot.height > 0, "arrived bounds are reviewed");

    StateMachine no_atlas({"tvideoFlyWalk", 1, "leftApproach", 1, true, false, false});
    require(no_atlas.phase() == Phase::kRevealTeachingContent, "missing atlas skips travel");
    require(no_atlas.degraded_reason() == DegradedReason::kMissingAtlas, "missing atlas reason recorded");
    require(std::string(DegradedReasonName(no_atlas.degraded_reason())) == "missingAtlas", "stable degraded reason name");
    require(no_atlas.content_visible(), "missing atlas reveals content");

    StateMachine no_overlay({"tvideoFlyWalk", 1, "rightApproach", 1, false, true, false});
    require(no_overlay.degraded_reason() == DegradedReason::kMissingOverlay, "missing overlay reason recorded");
    require(no_overlay.content_visible(), "missing overlay reveals content");

    StateMachine reduced({"tvideoFlyWalk", 1, "centerRoad", 1, true, true, true});
    require(reduced.phase() == Phase::kRevealTeachingContent, "reduced motion skips travel");
    require(reduced.degraded_reason() == DegradedReason::kReducedMotion, "reduced motion reason recorded");

    StateMachine timeout({"tvideoFlyWalk", 1, "centerRoad", 1, true, true, false});
    timeout.Timeout();
    require(timeout.phase() == Phase::kRevealTeachingContent, "timeout terminates");
    require(timeout.degraded_reason() == DegradedReason::kPhaseTimeout, "timeout reason recorded");

    StateMachine legacy({nullptr, 0, nullptr, 0, false, false, false});
    require(legacy.bypass(), "lessons without template bypass state machine");

    std::cout << "tvideo template host test passed: " << checks << " checks\n";
    return 0;
}
