#include "jpeg_decoder.c"

_Static_assert(__builtin_types_compatible_p(
    __typeof__(&jpeg_decode_in_cb), __typeof__(((JDEC *)0)->infunc)),
    "JPEG input callback must match the decoder ABI");

void* heap_caps_malloc(size_t bytes, uint32_t caps)
{
    (void)caps;
    return malloc(bytes);
}

void heap_caps_free(void* pointer)
{
    free(pointer);
}

int main(void)
{
    uint8_t input[] = {1, 2, 3, 4};
    uint8_t output[] = {0, 0, 0, 0};
    esp_jpeg_image_cfg_t cfg = {.indata = input, .indata_size = sizeof(input)};
    JDEC decoder = {.device = &cfg};
    assert(jpeg_decode_in_cb(&decoder, output, 0) == 0);
    assert(cfg.priv.read == 0);
    assert(jpeg_decode_in_cb(&decoder, output, 2) == 2);
    assert(output[0] == 1 && output[1] == 2 && output[2] == 0);
    assert(jpeg_decode_in_cb(&decoder, NULL, 1) == 1);
    assert(jpeg_decode_in_cb(&decoder, output, 2) == 1);
    assert(output[0] == 4 && cfg.priv.read == 4);
    assert(jpeg_decode_in_cb(&decoder, output, 1) == 0);
#if SIZE_MAX > UINT32_MAX
    cfg.priv.read = 0;
    assert(jpeg_decode_in_cb(&decoder, NULL, (size_t)UINT32_MAX + 2) == 0);
    assert(cfg.priv.read == 0);
    assert(jpeg_decode_in_cb(&decoder, output, (size_t)UINT32_MAX + 2) == 0);
    assert(cfg.priv.read == 0 && output[0] == 4);
#endif
    return 0;
}
