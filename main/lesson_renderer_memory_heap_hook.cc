#include <cstddef>
#include <cstdint>
#include <sdkconfig.h>

#if defined(TBOT_RENDERER_MEMORY_DIAGNOSTICS) && defined(CONFIG_HEAP_USE_HOOKS)

#include <esp_attr.h>

extern "C" {
volatile uint32_t g_lesson_renderer_frame_allocation_count = 0;
volatile bool g_lesson_renderer_frame_allocation_measurement_active = false;

IRAM_ATTR void esp_heap_trace_alloc_hook(void* ptr, size_t, uint32_t) {
    if (ptr != nullptr &&
        __atomic_load_n(&g_lesson_renderer_frame_allocation_measurement_active,
                        __ATOMIC_RELAXED)) {
        __atomic_add_fetch(&g_lesson_renderer_frame_allocation_count, 1,
                           __ATOMIC_RELAXED);
    }
}
}

#endif
