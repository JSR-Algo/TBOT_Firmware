#pragma once

#include "lesson_tvideo_template.h"

#include <cstddef>
#include <cstdint>
#ifdef TBOT_LESSON_MEMORY_TEST
#include <string>
#endif

struct LessonRendererMemorySample {
    size_t internal_free;
    size_t largest_internal_block;
    size_t psram_free;
    size_t live_decoded_layers;
    size_t live_lvgl_animations;
    size_t live_animation_contexts;
};

struct LessonRendererAllocatorSample {
    size_t internal_free;
    size_t largest_internal_block;
    size_t psram_free;
};

struct LessonRendererAnimationFrameResult {
    lesson_tvideo::Rect robot;
    bool hidden;
    bool complete;
    bool timed_out;
};

using LessonRendererAnimationBoundsWriter =
    void (*)(void* context, const lesson_tvideo::Rect& robot, bool hidden);

LessonRendererAnimationFrameResult AdvanceLessonRendererAnimationFrame(
    lesson_tvideo::StateMachine* machine, uint32_t* elapsed_ms,
    uint32_t tick_ms, uint32_t timeout_ms,
    LessonRendererAnimationBoundsWriter bounds_writer, void* bounds_context);

enum class LessonRendererMemoryPhase : uint8_t {
    kStart,
    kPeak,
    kComplete,
    kCancel,
};

struct LessonRendererMemoryThresholds {
    size_t max_frame_allocations;
    size_t max_live_decoded_layers;
    size_t max_settled_animations_and_contexts;
    size_t max_internal_heap_loss;
    size_t max_psram_loss;
    size_t min_largest_internal_block;
    size_t min_internal_free;
};

struct LessonRendererMemoryReport {
    bool passed = false;
    bool forbidden_markers_found = false;
    size_t cancel_cycles = 0;
    size_t complete_cycles = 0;
    size_t frame_allocations = 0;
    size_t max_live_decoded_layers = 0;
    size_t max_settled_animations = 0;
    size_t max_settled_contexts = 0;
    size_t internal_heap_loss = 0;
    size_t psram_loss = 0;
    size_t largest_internal_block = 0;
    size_t min_internal_free = 0;
    size_t settled_observations_at_500ms = 0;
    LessonRendererMemoryThresholds thresholds{};

#ifdef TBOT_LESSON_MEMORY_TEST
    std::string ToJson(const char* measurement_kind) const;
#endif
};

#ifdef TBOT_LESSON_MEMORY_TEST
using LessonRendererMemoryReader = LessonRendererAllocatorSample (*)(void* context);

struct LessonRendererMemoryLiveCounters {
    size_t decoded_layers;
    size_t lvgl_animations;
    size_t animation_contexts;
};

LessonRendererMemoryLiveCounters LessonRendererMemoryLiveCountersForTest();
#endif

class LessonRendererMemoryProbe {
public:
    LessonRendererMemoryProbe();
#ifdef TBOT_LESSON_MEMORY_TEST
    LessonRendererMemoryProbe(LessonRendererMemoryReader reader, void* context);
#endif

    void Capture(LessonRendererMemoryPhase phase, uint32_t elapsed_since_terminal_ms = 0);
    void RecordFrameAllocations(size_t allocations);
    LessonRendererMemoryReport Evaluate(const LessonRendererMemoryThresholds& thresholds,
                                        bool forbidden_markers_found) const;

private:
    LessonRendererMemorySample Read() const;

#ifdef TBOT_LESSON_MEMORY_TEST
    LessonRendererMemoryReader reader_ = nullptr;
    void* reader_context_ = nullptr;
#endif
    LessonRendererMemorySample baseline_{};
    LessonRendererMemorySample latest_complete_{};
    bool has_baseline_ = false;
    bool has_complete_ = false;
    size_t cancel_cycles_ = 0;
    size_t complete_cycles_ = 0;
    size_t frame_allocations_ = 0;
    size_t max_live_decoded_layers_ = 0;
    size_t max_settled_animations_ = 0;
    size_t max_settled_contexts_ = 0;
    size_t min_internal_free_ = static_cast<size_t>(-1);
    size_t settled_observations_at_500ms_ = 0;
};

void LessonRendererMemoryDecodedLayerOpened();
void LessonRendererMemoryDecodedLayerClosed();
void LessonRendererMemoryAnimationStarted();
void LessonRendererMemoryAnimationStopped();
void LessonRendererMemoryContextOpened();
void LessonRendererMemoryContextClosed();
