#pragma once
// Host stub for esp_heap_caps.h. The lesson renderer allocates image buffers with
// heap_caps_malloc/realloc and frees them with heap_caps_free. On the host these
// map to libc malloc/realloc/free so the real ownership/free logic is exercised.
//
// A test-controllable failure hook lets us reach the defensive alloc/realloc ==
// nullptr guards that a host malloc would otherwise never trigger: when
// g_host_heap_fail_after >= 0, the Nth allocation (0-based) returns nullptr.
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 4)

inline size_t heap_caps_get_free_size(int caps) {
    return (caps & MALLOC_CAP_SPIRAM) ? 4 * 1024 * 1024 : 128 * 1024;
}
inline size_t heap_caps_get_minimum_free_size(int) { return 96 * 1024; }

// -1 disables the hook (default). Set to N to fail the Nth heap_caps_malloc/
// heap_caps_realloc call (counting from 0) so OOM guards become reachable.
inline int& HostHeapFailAfter() {
    static int v = -1;
    return v;
}
inline int& HostHeapCallCount() {
    static int v = 0;
    return v;
}

inline void* heap_caps_malloc(size_t size, int /*caps*/) {
    int n = HostHeapCallCount()++;
    if (HostHeapFailAfter() >= 0 && n >= HostHeapFailAfter()) return nullptr;
    return std::malloc(size);
}
inline void* heap_caps_realloc(void* ptr, size_t size, int /*caps*/) {
    int n = HostHeapCallCount()++;
    if (HostHeapFailAfter() >= 0 && n >= HostHeapFailAfter()) return nullptr;
    return std::realloc(ptr, size);
}
inline void heap_caps_free(void* ptr) { std::free(ptr); }

// Host-only libc fread hook. The native lesson coverage harness can compile the
// unit under test with -Dfread=HostLessonFread to force local-file short reads
// deterministically; production firmware still calls libc fread directly.
extern "C" size_t HostLessonFread(void* ptr, size_t size, size_t count, FILE* stream);
