#include "lesson_renderer_memory_probe.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
constexpr char kTag[] = "LessonRenderer";

std::atomic<size_t> g_live_decoded_layers{0};
std::atomic<size_t> g_live_lvgl_animations{0};
std::atomic<size_t> g_live_animation_contexts{0};

void DecrementIfPositive(std::atomic<size_t>* counter) {
    size_t value = counter->load(std::memory_order_relaxed);
    while (value != 0 && !counter->compare_exchange_weak(
               value, value - 1, std::memory_order_relaxed)) {}
}

const char* PhaseName(LessonRendererMemoryPhase phase) {
    switch (phase) {
        case LessonRendererMemoryPhase::kStart: return "start";
        case LessonRendererMemoryPhase::kPeak: return "peak";
        case LessonRendererMemoryPhase::kComplete: return "complete";
        case LessonRendererMemoryPhase::kCancel: return "cancel";
    }
    return "unknown";
}

size_t Loss(size_t baseline, size_t current) {
    return current < baseline ? baseline - current : 0;
}
}  // namespace

LessonRendererMemoryProbe::LessonRendererMemoryProbe() = default;

#ifdef TBOT_LESSON_MEMORY_TEST
LessonRendererMemoryProbe::LessonRendererMemoryProbe(
    LessonRendererMemoryReader reader, void* context)
    : reader_(reader), reader_context_(context) {}
#endif

LessonRendererMemorySample LessonRendererMemoryProbe::Read() const {
#ifdef TBOT_LESSON_MEMORY_TEST
    if (reader_ != nullptr) return reader_(reader_context_);
    return {};
#else
    return {
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        g_live_decoded_layers.load(std::memory_order_relaxed),
        g_live_lvgl_animations.load(std::memory_order_relaxed),
        g_live_animation_contexts.load(std::memory_order_relaxed),
    };
#endif
}

void LessonRendererMemoryProbe::Capture(LessonRendererMemoryPhase phase,
                                        uint32_t elapsed_since_terminal_ms) {
    const LessonRendererMemorySample sample = Read();
    if (!has_baseline_) {
        baseline_ = sample;
        has_baseline_ = true;
    }
    min_internal_free_ = std::min(min_internal_free_, sample.internal_free);
    max_live_decoded_layers_ =
        std::max(max_live_decoded_layers_, sample.live_decoded_layers);

    if (phase == LessonRendererMemoryPhase::kCancel) ++cancel_cycles_;
    if (phase == LessonRendererMemoryPhase::kComplete) {
        ++complete_cycles_;
        latest_complete_ = sample;
        has_complete_ = true;
    }
    if ((phase == LessonRendererMemoryPhase::kCancel ||
         phase == LessonRendererMemoryPhase::kComplete) &&
        elapsed_since_terminal_ms >= 500) {
        ++settled_observations_at_500ms_;
        max_settled_animations_ =
            std::max(max_settled_animations_, sample.live_lvgl_animations);
        max_settled_contexts_ =
            std::max(max_settled_contexts_, sample.live_animation_contexts);
    }

    ESP_LOGI(kTag,
             "renderer_mem phase=%s ifree=%u ilargest=%u psfree=%u layers=%u "
             "anim=%u ctx=%u frame_alloc=%u",
             PhaseName(phase), static_cast<unsigned>(sample.internal_free),
             static_cast<unsigned>(sample.largest_internal_block),
             static_cast<unsigned>(sample.psram_free),
             static_cast<unsigned>(sample.live_decoded_layers),
             static_cast<unsigned>(sample.live_lvgl_animations),
             static_cast<unsigned>(sample.live_animation_contexts),
             static_cast<unsigned>(frame_allocations_));
}

void LessonRendererMemoryProbe::RecordFrameAllocations(size_t allocations) {
    frame_allocations_ += allocations;
}

LessonRendererMemoryReport LessonRendererMemoryProbe::Evaluate(
    const LessonRendererMemoryThresholds& thresholds,
    bool forbidden_markers_found) const {
    LessonRendererMemoryReport report;
    report.forbidden_markers_found = forbidden_markers_found;
    report.cancel_cycles = cancel_cycles_;
    report.complete_cycles = complete_cycles_;
    report.frame_allocations = frame_allocations_;
    report.max_live_decoded_layers = max_live_decoded_layers_;
    report.max_settled_animations = max_settled_animations_;
    report.max_settled_contexts = max_settled_contexts_;
    report.internal_heap_loss =
        has_complete_ ? Loss(baseline_.internal_free, latest_complete_.internal_free) : 0;
    report.psram_loss =
        has_complete_ ? Loss(baseline_.psram_free, latest_complete_.psram_free) : 0;
    report.largest_internal_block =
        has_complete_ ? latest_complete_.largest_internal_block : 0;
    report.min_internal_free =
        min_internal_free_ == static_cast<size_t>(-1) ? 0 : min_internal_free_;
    report.settled_observations_at_500ms = settled_observations_at_500ms_;
    report.thresholds = thresholds;
    report.passed = has_baseline_ && has_complete_ && cancel_cycles_ == 100 &&
                    complete_cycles_ == 100 && settled_observations_at_500ms_ == 200 &&
                    frame_allocations_ <= thresholds.max_frame_allocations &&
                    max_live_decoded_layers_ <= thresholds.max_live_decoded_layers &&
                    max_settled_animations_ <=
                        thresholds.max_settled_animations_and_contexts &&
                    max_settled_contexts_ <=
                        thresholds.max_settled_animations_and_contexts &&
                    report.internal_heap_loss <= thresholds.max_internal_heap_loss &&
                    report.psram_loss <= thresholds.max_psram_loss &&
                    report.largest_internal_block >= thresholds.min_largest_internal_block &&
                    report.min_internal_free >= thresholds.min_internal_free &&
                    !forbidden_markers_found;
    return report;
}

#ifdef TBOT_LESSON_MEMORY_TEST
std::string LessonRendererMemoryReport::ToJson(const char* measurement_kind) const {
    char json[2048];
    std::snprintf(
        json, sizeof(json),
        "{\"schemaVersion\":1,\"measurementKind\":\"%s\",\"passed\":%s,"
        "\"cycles\":{\"cancel\":%u,\"complete\":%u},"
        "\"actual\":{\"frameAllocations\":%u,\"maxLiveDecodedLayers\":%u,"
        "\"maxSettledAnimations\":%u,\"maxSettledContexts\":%u,"
        "\"internalHeapLossBytes\":%u,\"psramLossBytes\":%u,"
        "\"largestInternalBlockBytes\":%u,\"minInternalFreeBytes\":%u,"
        "\"settledObservationsAt500ms\":%u,\"forbiddenMarkersFound\":%s},"
        "\"thresholds\":{\"maxFrameAllocations\":%u,"
        "\"maxLiveDecodedLayers\":%u,\"maxSettledAnimationsAndContexts\":%u,"
        "\"maxInternalHeapLossBytes\":%u,\"maxPsramLossBytes\":%u,"
        "\"minLargestInternalBlockBytes\":%u,\"minInternalFreeBytes\":%u}}",
        measurement_kind == nullptr ? "unknown" : measurement_kind,
        passed ? "true" : "false", static_cast<unsigned>(cancel_cycles),
        static_cast<unsigned>(complete_cycles), static_cast<unsigned>(frame_allocations),
        static_cast<unsigned>(max_live_decoded_layers),
        static_cast<unsigned>(max_settled_animations),
        static_cast<unsigned>(max_settled_contexts),
        static_cast<unsigned>(internal_heap_loss), static_cast<unsigned>(psram_loss),
        static_cast<unsigned>(largest_internal_block),
        static_cast<unsigned>(min_internal_free),
        static_cast<unsigned>(settled_observations_at_500ms),
        forbidden_markers_found ? "true" : "false",
        static_cast<unsigned>(thresholds.max_frame_allocations),
        static_cast<unsigned>(thresholds.max_live_decoded_layers),
        static_cast<unsigned>(thresholds.max_settled_animations_and_contexts),
        static_cast<unsigned>(thresholds.max_internal_heap_loss),
        static_cast<unsigned>(thresholds.max_psram_loss),
        static_cast<unsigned>(thresholds.min_largest_internal_block),
        static_cast<unsigned>(thresholds.min_internal_free));
    return json;
}
#endif

void LessonRendererMemoryDecodedLayerOpened() {
    g_live_decoded_layers.fetch_add(1, std::memory_order_relaxed);
}

void LessonRendererMemoryDecodedLayerClosed() {
    DecrementIfPositive(&g_live_decoded_layers);
}

void LessonRendererMemoryAnimationStarted() {
    g_live_lvgl_animations.fetch_add(1, std::memory_order_relaxed);
}

void LessonRendererMemoryAnimationStopped() {
    DecrementIfPositive(&g_live_lvgl_animations);
}

void LessonRendererMemoryContextOpened() {
    g_live_animation_contexts.fetch_add(1, std::memory_order_relaxed);
}

void LessonRendererMemoryContextClosed() {
    DecrementIfPositive(&g_live_animation_contexts);
}
