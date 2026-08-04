#ifndef TEST_ESP_HEAP_CAPS_H
#define TEST_ESP_HEAP_CAPS_H

#include <cstdlib>

#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2

inline std::size_t g_heap_caps_malloc_calls = 0;
inline std::size_t g_heap_caps_last_size = 0;
inline int g_heap_caps_last_caps = 0;

inline void* heap_caps_malloc(std::size_t size, int caps) {
    ++g_heap_caps_malloc_calls;
    g_heap_caps_last_size = size;
    g_heap_caps_last_caps = caps;
    return std::malloc(size);
}

inline void heap_caps_free(void* allocation) {
    std::free(allocation);
}

#endif
