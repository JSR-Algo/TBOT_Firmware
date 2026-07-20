#ifndef TEST_ESP_HEAP_CAPS_H
#define TEST_ESP_HEAP_CAPS_H

#include <cstdlib>

#define MALLOC_CAP_8BIT 1

inline void* heap_caps_malloc(std::size_t size, int) {
    return std::malloc(size);
}

inline void heap_caps_free(void* allocation) {
    std::free(allocation);
}

#endif
