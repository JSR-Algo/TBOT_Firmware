#pragma once
// Host allocation bridge only: the bundled LodePNG decoder is compiled unchanged.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define LV_USE_LODEPNG 1
#define LV_STDDEF_INCLUDE <stddef.h>
#define LV_ATTRIBUTE_EXTERN_DATA
#define LV_COLOR_FORMAT_ARGB8888 1
#define lv_malloc malloc
#define lv_realloc realloc
#define lv_free free
#define lv_memcpy memcpy
#define lv_memset memset
typedef struct { unsigned char* data; size_t data_size; } lv_draw_buf_t;
typedef struct { int image_cache_draw_buf_handlers; } lv_global_t;
static inline lv_global_t* actual_media_global(void) {
    static lv_global_t global;
    return &global;
}
#define LV_GLOBAL_DEFAULT actual_media_global
static inline lv_draw_buf_t* lv_draw_buf_create_ex(const void* handlers, unsigned width,
                                                  unsigned height, int format, unsigned stride) {
    (void)handlers;
    (void)width;
    (void)format;
    lv_draw_buf_t* result = (lv_draw_buf_t*)malloc(sizeof(*result));
    if (!result) return NULL;
    result->data_size = (size_t)height * stride;
    result->data = (unsigned char*)malloc(result->data_size);
    if (!result->data) { free(result); return NULL; }
    return result;
}
static inline void lv_draw_buf_destroy(lv_draw_buf_t* buffer) {
    if (buffer) { free(buffer->data); free(buffer); }
}
