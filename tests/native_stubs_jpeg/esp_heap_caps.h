#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_DEFAULT 0x1000u

#ifdef __cplusplus
extern "C" {
#endif
void* heap_caps_malloc(size_t size, uint32_t caps);
void* heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, uint32_t caps);
void heap_caps_free(void* ptr);
#ifdef __cplusplus
}
#endif
