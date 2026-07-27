#include <cstddef>
#include <cstdint>
#include <sdkconfig.h>

#if defined(TBOT_RENDERER_MEMORY_DIAGNOSTICS) && defined(CONFIG_HEAP_USE_HOOKS)

#include <esp_attr.h>

extern "C" {
DRAM_ATTR volatile uint32_t g_lesson_renderer_frame_allocation_observed = 0;
DRAM_ATTR volatile uint32_t g_lesson_renderer_frame_allocation_measurement_active = 0;

IRAM_ATTR void esp_heap_trace_alloc_hook(void* ptr, size_t, uint32_t) {
    if (ptr != nullptr &&
        g_lesson_renderer_frame_allocation_measurement_active != 0) {
        // Allocator hooks are synchronous; concurrent writers only store 1.
        g_lesson_renderer_frame_allocation_observed = 1;
    }
}
}

#endif
