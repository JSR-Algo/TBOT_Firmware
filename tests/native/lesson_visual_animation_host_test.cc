#include "lesson_tvideo_template.h"

#ifdef TBOT_LESSON_MEMORY_TEST
#include "lesson_renderer_memory_probe.h"
#include <esp_log.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
int checks = 0;
#ifdef TBOT_LESSON_MEMORY_TEST
std::atomic<size_t> frame_allocations{0};
thread_local bool inside_animation_frame = false;
#endif

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "lesson visual animation host test FAILED: " << message << "\n";
        std::exit(1);
    }
}

#ifdef TBOT_LESSON_MEMORY_TEST
struct HostMemoryState {
    size_t internal_free = 128 * 1024;
    size_t largest_internal_block = 96 * 1024;
    size_t psram_free = 4 * 1024 * 1024;
    size_t live_decoded_layers = 0;
    size_t live_lvgl_animations = 0;
    size_t live_animation_contexts = 0;
};

LessonRendererMemorySample ReadHostMemory(void* context) {
    const auto& state = *static_cast<HostMemoryState*>(context);
    return {
        state.internal_free,
        state.largest_internal_block,
        state.psram_free,
        state.live_decoded_layers,
        state.live_lvgl_animations,
        state.live_animation_contexts,
    };
}

void StartEntrance(HostMemoryState* state) {
    state->internal_free = 124 * 1024;
    state->largest_internal_block = 88 * 1024;
    state->psram_free = 4 * 1024 * 1024 - 6 * 1024;
    state->live_decoded_layers = 3;
    state->live_lvgl_animations = 1;
    state->live_animation_contexts = 1;
}

void SettleEntrance(HostMemoryState* state, bool completed) {
    state->internal_free = completed ? 127 * 1024 : 128 * 1024;
    state->largest_internal_block = 88 * 1024;
    state->psram_free = completed ? 4 * 1024 * 1024 - 2 * 1024 : 4 * 1024 * 1024;
    state->live_decoded_layers = completed ? 3 : 0;
    state->live_lvgl_animations = 0;
    state->live_animation_contexts = 0;
}

void AdvanceWithoutAllocations(lesson_tvideo::StateMachine* animation,
                               LessonRendererMemoryProbe* probe, uint32_t elapsed_ms) {
    const size_t before = frame_allocations.load(std::memory_order_relaxed);
    inside_animation_frame = true;
    animation->Advance(elapsed_ms);
    inside_animation_frame = false;
    probe->RecordFrameAllocations(
        frame_allocations.load(std::memory_order_relaxed) - before);
}

void RunMemoryGate() {
    HostEspResetLogs();
    frame_allocations.store(0, std::memory_order_relaxed);

    HostMemoryState state;
    LessonRendererMemoryProbe probe(ReadHostMemory, &state);
    probe.Capture(LessonRendererMemoryPhase::kStart);

    for (int cycle = 0; cycle < 100; ++cycle) {
        lesson_tvideo::StateMachine animation(
            {"tvideoFlyWalk", 1, "leftApproach", 1, true, true, false});
        StartEntrance(&state);
        probe.Capture(LessonRendererMemoryPhase::kPeak);
        AdvanceWithoutAllocations(&animation, &probe, 100 + (cycle % 5) * 400);
        animation.Timeout();
        SettleEntrance(&state, false);
        probe.Capture(LessonRendererMemoryPhase::kCancel, 500);
    }

    for (int cycle = 0; cycle < 100; ++cycle) {
        lesson_tvideo::StateMachine animation(
            {"tvideoFlyWalk", 1, "rightApproach", 1, true, true, false});
        StartEntrance(&state);
        probe.Capture(LessonRendererMemoryPhase::kPeak);
        for (int elapsed = 0; elapsed < 5200; elapsed += 40) {
            AdvanceWithoutAllocations(&animation, &probe, 40);
        }
        require(animation.phase() == lesson_tvideo::Phase::kRevealTeachingContent,
                "completed memory cycle reaches reveal");
        SettleEntrance(&state, true);
        probe.Capture(LessonRendererMemoryPhase::kComplete, 500);
    }

    const LessonRendererMemoryThresholds thresholds{
        0, 3, 0, 4096, 8192, 65536, 49152,
    };
    bool forbidden_markers_found = false;
    for (const auto& log : HostEspLogs()) {
        std::string normalized = log;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char value) { return std::tolower(value); });
        for (const char* marker : {"queue full", "queue-full", "queue_full",
                                   "allocation failed", "allocation-failed",
                                   "allocation_failed", "watchdog", "decoder leak",
                                   "decoder-leak", "decoder_leak", "oom"}) {
            if (normalized.find(marker) != std::string::npos) forbidden_markers_found = true;
        }
    }
    const LessonRendererMemoryReport report =
        probe.Evaluate(thresholds, forbidden_markers_found);
    require(report.cancel_cycles == 100 && report.complete_cycles == 100,
            "memory report includes all soak cycles");
    require(report.frame_allocations == 0, "animation frames allocate no memory");
    require(report.max_live_decoded_layers <= 3, "decoded layer count stays bounded");
    require(report.max_settled_animations == 0 && report.max_settled_contexts == 0,
            "animations and contexts release by 500ms");
    require(report.internal_heap_loss <= 4096 && report.psram_loss <= 8192,
            "simulated heap losses stay within thresholds");
    require(report.largest_internal_block >= 65536 && report.min_internal_free >= 49152,
            "simulated internal heap headroom stays within thresholds");
    require(report.settled_observations_at_500ms == 200,
            "every terminal cycle has a 500ms cleanup observation");
    require(!report.forbidden_markers_found, "memory logs contain no failure markers");
    require(report.passed, "renderer memory thresholds pass");
    std::cout << report.ToJson("simulated-host") << "\n";
}
#endif
}  // namespace

#ifdef TBOT_LESSON_MEMORY_TEST
void* operator new(std::size_t size) {
    if (inside_animation_frame) frame_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size)) return pointer;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
#endif

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

#ifdef TBOT_LESSON_MEMORY_TEST
    RunMemoryGate();
#else
    std::cout << "lesson visual animation host test passed: " << checks << " checks\n";
#endif
    return 0;
}
