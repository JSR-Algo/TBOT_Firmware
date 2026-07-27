#include "lesson_tvideo_template.h"

#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "lesson visual animation host test FAILED: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    using namespace lesson_tvideo;

    StateMachine animation({"tvideoFlyWalk", 1, "centerRoad", 1, true, true, false});
    animation.Advance(100 + 1200 + 700 + 350);
    require(animation.phase() == Phase::kWalkToward, "walk phase starts after settle");
    const Rect walk_start = animation.geometry().robot;

    animation.Advance(900);
    const Rect walk_middle = animation.geometry().robot;
    require(animation.phase() == Phase::kWalkToward, "half walk stays in walk phase");
    require(walk_middle.left < walk_start.left,
            "walk interpolates horizontally instead of snapping to arrived geometry");
    require(walk_middle.top > walk_start.top,
            "walk interpolates vertically instead of snapping to arrived geometry");

    animation.Advance(900);
    require(animation.phase() == Phase::kArriveNear, "walk finishes at arrive phase");
    require(animation.geometry().robot.left == 184 && animation.geometry().robot.top == 184,
            "arrive phase lands on reviewed centerRoad geometry");

    for (int cycle = 0; cycle < 100; ++cycle) {
        StateMachine repeated({"tvideoFlyWalk", 1, "leftApproach", 1, true, true, false});
        repeated.Advance(5150);
        require(repeated.phase() == Phase::kRevealTeachingContent,
                "repeated entrance reaches reveal without stale phase state");
    }

    std::cout << "lesson visual animation host test passed: " << checks << " checks\n";
    return 0;
}
