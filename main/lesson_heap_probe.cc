#include "lesson_heap_probe.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "LessonHeap";
}

void LogLessonHeapBoundary(const char* phase, size_t payload_bytes) {
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t lifetime_min_internal =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(kTag,
             "lesson_heap phase=%s payload_bytes=%u internal_free=%u "
             "lifetime_min_internal=%u",
             phase,
             static_cast<unsigned>(payload_bytes),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(lifetime_min_internal));
}
