#include <esp_check.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <stdbool.h>
#include <sys/param.h>

#include "jpeg_decoder.h"

#include "jpeg_to_image.h"

#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL MAX(CONFIG_LOG_DEFAULT_LEVEL, ESP_LOG_DEBUG)
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#include <esp_log.h>

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
#include "driver/jpeg_decode.h"
#endif

#define TAG "jpeg_to_image"
#define JPEG_MAX_DIMENSION 4096U
#define JPEG_MAX_DECODED_BYTES (4U * 1024U * 1024U)

// esp_jpeg 1.3.1 uses 3100 bytes unless external TJPG selects FASTDECODE=2.
// ESP32-S3 must stay on its fixed ROM configuration; fail instead of silently
// using a work-buffer size that belongs to a different decoder configuration.
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#if !defined(CONFIG_JD_USE_ROM) || !CONFIG_JD_USE_ROM
#error "ESP32-S3 JPEG decoding requires CONFIG_JD_USE_ROM=y"
#endif
#if defined(CONFIG_JD_FASTDECODE) && CONFIG_JD_FASTDECODE == 2
#error "ESP32-S3 ROM JPEG decoding is incompatible with CONFIG_JD_FASTDECODE=2"
#endif
#define JPEG_ROM_WORK_BUFFER_SIZE 3100U
#elif defined(CONFIG_JD_FASTDECODE) && CONFIG_JD_FASTDECODE == 2
#define JPEG_ROM_WORK_BUFFER_SIZE 65472U
#else
#define JPEG_ROM_WORK_BUFFER_SIZE 3100U
#endif

typedef struct {
    size_t width;
    size_t height;
    size_t stride;
    size_t decoded_size;
} validated_jpeg_info_t;

static uint16_t read_be16(const uint8_t* bytes) {
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static esp_err_t validate_baseline_jpeg(const uint8_t* src, size_t src_len, validated_jpeg_info_t* info) {
    if (src == NULL || info == NULL || src_len < 4 || src[0] != 0xff || src[1] != 0xd8) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 2;
    bool found_sof = false;
    uint8_t sof_components = 0;
    while (offset < src_len) {
        if (src[offset] != 0xff) {
            return ESP_FAIL;
        }
        while (offset < src_len && src[offset] == 0xff) {
            ++offset;
        }
        if (offset >= src_len) {
            return ESP_FAIL;
        }

        const uint8_t marker = src[offset++];
        if (marker == 0x00 || marker == 0xd8 || marker == 0xd9 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7)) {
            return ESP_FAIL;
        }
        if (src_len - offset < 2) {
            return ESP_FAIL;
        }

        const size_t segment_len = read_be16(src + offset);
        if (segment_len < 2 || segment_len > src_len - offset) {
            return ESP_FAIL;
        }

        if (marker == 0xda) {
            if (!found_sof || segment_len < 6) {
                return ESP_FAIL;
            }
            const uint8_t scan_components = src[offset + 2];
            if (scan_components == 0 || scan_components > sof_components ||
                segment_len != 6U + 2U * scan_components) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            return ESP_OK;
        }

        const bool is_sof = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (is_sof) {
            if (found_sof || marker != 0xc0 || segment_len < 8) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            const uint8_t* sof = src + offset + 2;
            const uint8_t components = sof[5];
            if (sof[0] != 8 || (components != 1 && components != 3) || segment_len != 8U + 3U * components) {
                return ESP_ERR_NOT_SUPPORTED;
            }

            const size_t height = read_be16(sof + 1);
            const size_t width = read_be16(sof + 3);
            if (width == 0 || height == 0 || width > JPEG_MAX_DIMENSION || height > JPEG_MAX_DIMENSION ||
                width > SIZE_MAX / 2) {
                return ESP_ERR_INVALID_SIZE;
            }
            const size_t stride = width * 2;
            if (height > SIZE_MAX / stride) {
                return ESP_ERR_INVALID_SIZE;
            }
            const size_t decoded_size = stride * height;
            if (decoded_size == 0 || decoded_size > JPEG_MAX_DECODED_BYTES || decoded_size > UINT32_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }

            info->width = width;
            info->height = height;
            info->stride = stride;
            info->decoded_size = decoded_size;
            sof_components = components;
            found_sof = true;
        }

        offset += segment_len;
    }

    return ESP_FAIL;
}

static esp_err_t decode_with_rom_jpeg(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                                      size_t* height, size_t* stride, uint32_t output_caps) {
    ESP_LOGD(TAG, "Decoding JPEG with ROM TJPG decoder");
    esp_err_t ret = ESP_FAIL;
    uint8_t* work_buf = NULL;
    uint8_t* out_buf = NULL;
    esp_jpeg_image_output_t out_info = {0};
    validated_jpeg_info_t validated_info = {0};

    if (output_caps == 0) {
        output_caps = MALLOC_CAP_DEFAULT;
    }
    if (src_len > UINT32_MAX) {
        ret = ESP_ERR_INVALID_SIZE;
        goto jpeg_rom_dec_failed;
    }
    ret = validate_baseline_jpeg(src, src_len, &validated_info);
    if (ret != ESP_OK) {
        goto jpeg_rom_dec_failed;
    }

    esp_jpeg_image_cfg_t config = {
        .indata = (uint8_t*)src,
        .indata_size = src_len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {.swap_color_bytes = 0},
    };

    ret = esp_jpeg_get_image_info(&config, &out_info);
    if (ret != ESP_OK || out_info.width != validated_info.width || out_info.height != validated_info.height ||
        out_info.output_len != validated_info.decoded_size) {
        ESP_LOGE(TAG, "Failed to parse JPEG header");
        ret = ESP_FAIL;
        goto jpeg_rom_dec_failed;
    }

    ESP_LOGD(TAG, "JPEG header info: width=%u, height=%u", out_info.width, out_info.height);

    work_buf = heap_caps_malloc(JPEG_ROM_WORK_BUFFER_SIZE, output_caps);
    if (work_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JPEG work buffer");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_rom_dec_failed;
    }

    const size_t decoded_size = validated_info.decoded_size;
    out_buf = heap_caps_aligned_calloc(16, 1, decoded_size, output_caps);
    if (out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_rom_dec_failed;
    }

    config.outbuf = out_buf;
    config.outbuf_size = decoded_size;
    config.advanced.working_buffer = work_buf;
    config.advanced.working_buffer_size = JPEG_ROM_WORK_BUFFER_SIZE;

    esp_jpeg_image_output_t decoded_info = {0};
    ret = esp_jpeg_decode(&config, &decoded_info);
    if (ret != ESP_OK || decoded_info.width != out_info.width || decoded_info.height != out_info.height ||
        decoded_info.output_len != decoded_size) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        ret = ret == ESP_ERR_NO_MEM ? ret : ESP_FAIL;
        goto jpeg_rom_dec_failed;
    }

    ESP_LOG_BUFFER_HEXDUMP(TAG, out_buf, MIN(decoded_size, 256), ESP_LOG_DEBUG);

    *out = out_buf;
    out_buf = NULL;
    *out_len = decoded_size;
    *width = validated_info.width;
    *height = validated_info.height;
    *stride = validated_info.stride;
    heap_caps_free(work_buf);
    work_buf = NULL;

    return ESP_OK;

jpeg_rom_dec_failed:
    if (work_buf) {
        heap_caps_free(work_buf);
        work_buf = NULL;
    }
    if (out_buf) {
        heap_caps_free(out_buf);
        out_buf = NULL;
    }

    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
    return ret;
}

esp_err_t jpeg_reusable_decoder_prepare(jpeg_reusable_decoder_t* decoder, size_t max_width, size_t max_height,
                                        uint32_t output_caps) {
    esp_err_t ret = jpeg_reusable_decoder_prepare_workspace(decoder, max_width, max_height, output_caps);
    if (ret != ESP_OK) return ret;
    uint8_t* output_buffer = heap_caps_aligned_calloc(16, 1, decoder->output_buffer_size, output_caps);
    if (output_buffer == NULL) {
        jpeg_reusable_decoder_destroy(decoder);
        return ESP_ERR_NO_MEM;
    }
    decoder->output_buffer = output_buffer;
    return ESP_OK;
}

esp_err_t jpeg_reusable_decoder_prepare_workspace(jpeg_reusable_decoder_t* decoder, size_t max_width,
                                                  size_t max_height, uint32_t output_caps) {
    if (decoder == NULL || decoder->working_buffer != NULL || decoder->output_buffer != NULL || max_width == 0 ||
        max_height == 0 || max_width > JPEG_MAX_DIMENSION || max_height > JPEG_MAX_DIMENSION || output_caps == 0 ||
        max_width > SIZE_MAX / 2) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t stride = max_width * 2;
    if (max_height > SIZE_MAX / stride) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t output_size = stride * max_height;
    if (output_size == 0 || output_size > JPEG_MAX_DECODED_BYTES || output_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t* working_buffer = heap_caps_malloc(JPEG_ROM_WORK_BUFFER_SIZE, output_caps);
    if (working_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    decoder->working_buffer = working_buffer;
    decoder->working_buffer_size = JPEG_ROM_WORK_BUFFER_SIZE;
    decoder->output_buffer = NULL;
    decoder->output_buffer_size = output_size;
    decoder->max_width = max_width;
    decoder->max_height = max_height;
    decoder->output_caps = output_caps;
    return ESP_OK;
}

esp_err_t jpeg_reusable_decoder_decode(jpeg_reusable_decoder_t* decoder, const uint8_t* src, size_t src_len,
                                       uint8_t** out, size_t* out_len, size_t* width, size_t* height, size_t* stride) {
    if (out != NULL) *out = NULL;
    if (out_len != NULL) *out_len = 0;
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (stride != NULL) *stride = 0;
    if (decoder == NULL || decoder->working_buffer == NULL || decoder->output_buffer == NULL || src == NULL ||
        src_len == 0 || src_len > UINT32_MAX || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = jpeg_reusable_decoder_decode_into(decoder, src, src_len, decoder->output_buffer,
                                                      decoder->output_buffer_size, out_len, width,
                                                      height, stride);
    if (ret == ESP_OK) *out = decoder->output_buffer;
    return ret;
}

esp_err_t jpeg_reusable_decoder_decode_into(jpeg_reusable_decoder_t* decoder, const uint8_t* src,
                                            size_t src_len, uint8_t* destination,
                                            size_t destination_size, size_t* out_len,
                                            size_t* width, size_t* height, size_t* stride) {
    if (out_len != NULL) *out_len = 0;
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (stride != NULL) *stride = 0;
    if (decoder == NULL || decoder->working_buffer == NULL || destination == NULL || src == NULL ||
        src_len == 0 || src_len > UINT32_MAX || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    validated_jpeg_info_t validated_info = {0};
    esp_err_t ret = validate_baseline_jpeg(src, src_len, &validated_info);
    if (ret != ESP_OK) {
        return ret;
    }
    if (validated_info.width > decoder->max_width || validated_info.height > decoder->max_height ||
        validated_info.decoded_size > destination_size ||
        decoder->working_buffer_size < JPEG_ROM_WORK_BUFFER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_jpeg_image_cfg_t config = {
        .indata = (uint8_t*)src,
        .indata_size = src_len,
        .outbuf = destination,
        .outbuf_size = destination_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {.swap_color_bytes = 0},
        .advanced = {
            .working_buffer = decoder->working_buffer,
            .working_buffer_size = decoder->working_buffer_size,
        },
    };
    esp_jpeg_image_output_t header_info = {0};
    ret = esp_jpeg_get_image_info(&config, &header_info);
    if (ret != ESP_OK || header_info.width != validated_info.width || header_info.height != validated_info.height ||
        header_info.output_len != validated_info.decoded_size) {
        return ESP_FAIL;
    }

    esp_jpeg_image_output_t decoded_info = {0};
    ret = esp_jpeg_decode(&config, &decoded_info);
    if (ret != ESP_OK || decoded_info.width != validated_info.width || decoded_info.height != validated_info.height ||
        decoded_info.output_len != validated_info.decoded_size) {
        return ret == ESP_ERR_NO_MEM ? ret : ESP_FAIL;
    }

    *out_len = validated_info.decoded_size;
    *width = validated_info.width;
    *height = validated_info.height;
    *stride = validated_info.stride;
    return ESP_OK;
}

void jpeg_reusable_decoder_destroy(jpeg_reusable_decoder_t* decoder) {
    if (decoder == NULL) {
        return;
    }
    if (decoder->working_buffer != NULL) {
        heap_caps_free(decoder->working_buffer);
    }
    if (decoder->output_buffer != NULL) {
        heap_caps_free(decoder->output_buffer);
    }
    *decoder = (jpeg_reusable_decoder_t){0};
}

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
static esp_err_t decode_with_hardware_jpeg(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                                           size_t* width, size_t* height, size_t* stride) {
    ESP_LOGD(TAG, "Decoding JPEG with hardware decoder");
    esp_err_t ret = ESP_OK;

    jpeg_decoder_handle_t jpeg_dec = NULL;
    uint8_t* bit_stream = NULL;
    uint8_t* out_buf = NULL;
    size_t out_buf_len = 0;
    size_t tx_buffer_size = 0;
    size_t rx_buffer_size = 0;

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 1,
        .timeout_ms = 1000,
    };

    jpeg_decode_cfg_t decode_cfg_rgb = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    ret = jpeg_new_decoder_engine(&eng_cfg, &jpeg_dec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder engine");
        goto jpeg_hw_dec_failed;
    }

    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    bit_stream = (uint8_t*)jpeg_alloc_decoder_mem(src_len, &tx_mem_cfg, &tx_buffer_size);
    if (bit_stream == NULL || tx_buffer_size < src_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG bit stream");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    memcpy(bit_stream, src, src_len);

    jpeg_decode_picture_info_t header_info;
    ESP_GOTO_ON_ERROR(jpeg_decoder_get_info(bit_stream, src_len, &header_info), jpeg_hw_dec_failed, TAG,
                      "Failed to get JPEG header info");

    ESP_LOGD(TAG, "JPEG header info: width=%d, height=%d, sample_method=%d", header_info.width, header_info.height,
             (int)header_info.sample_method);

    switch (header_info.sample_method) {
        case JPEG_DOWN_SAMPLING_GRAY:
        case JPEG_DOWN_SAMPLING_YUV444:
            out_buf_len = header_info.width * header_info.height * 2;
            *stride = header_info.width * 2;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
        case JPEG_DOWN_SAMPLING_YUV420:
            out_buf_len = ((header_info.width + 15) & ~15) * ((header_info.height + 15) & ~15) * 2;
            *stride = ((header_info.width + 15) & ~15) * 2;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported JPEG sample method");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto jpeg_hw_dec_failed;
    }

    out_buf = (uint8_t*)jpeg_alloc_decoder_mem(out_buf_len, &rx_mem_cfg, &rx_buffer_size);
    if (out_buf == NULL || rx_buffer_size < out_buf_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    uint32_t out_size = 0;

    ESP_GOTO_ON_ERROR(
        jpeg_decoder_process(jpeg_dec, &decode_cfg_rgb, bit_stream, src_len, out_buf, out_buf_len, &out_size),
        jpeg_hw_dec_failed, TAG, "Failed to decode JPEG");

    ESP_LOGD(TAG, "Expected %d bytes, got %" PRIu32 " bytes", out_buf_len, out_size);

    if (out_size != out_buf_len) {
        ESP_LOGE(TAG, "Decoded image size mismatch: Expected %zu bytes, got %" PRIu32 " bytes", out_buf_len, out_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto jpeg_hw_dec_failed;
    }

    if (header_info.sample_method == JPEG_DOWN_SAMPLING_GRAY) {
        // convert GRAY8 to RGB565
        uint32_t i = header_info.width * header_info.height;
        do {
            --i;
            uint8_t r = (out_buf[i] >> 3) & 0x1F;
            uint8_t g = (out_buf[i] >> 2) & 0x3F;
            // b is same as r
            uint16_t rgb565 = (r << 11) | (g << 5) | r;
            out_buf[2 * i + 1] = (rgb565 >> 8) & 0xFF;
            out_buf[2 * i] = rgb565 & 0xFF;
        } while (i != 0);
        out_size = header_info.width * header_info.height * 2;
        ESP_LOGD(TAG, "Converted GRAY8 to RGB565, new size: %zu", out_size);
    }

    ESP_LOG_BUFFER_HEXDUMP(TAG, out_buf, MIN(out_size, 256), ESP_LOG_DEBUG);

    *out = out_buf;
    out_buf = NULL;
    *out_len = (size_t)out_size;
    jpeg_del_decoder_engine(jpeg_dec);
    jpeg_dec = NULL;
    heap_caps_free(bit_stream);
    bit_stream = NULL;
    *width = header_info.width;
    *height = header_info.height;

    return ret;

jpeg_hw_dec_failed:
    if (out_buf) {
        heap_caps_free(out_buf);
        out_buf = NULL;
    }
    if (bit_stream) {
        heap_caps_free(bit_stream);
        bit_stream = NULL;
    }
    if (jpeg_dec) {
        jpeg_del_decoder_engine(jpeg_dec);
        jpeg_dec = NULL;
    }
    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
    return ret;
}
#endif  // CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER

esp_err_t jpeg_to_image(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                        size_t* height, size_t* stride) {
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
    esp_err_t ret = decode_with_hardware_jpeg(src, src_len, out, out_len, width, height, stride);
    if (ret == ESP_OK) {
        return ret;
    }
    ESP_LOGW(TAG, "Failed to decode with hardware JPEG, fallback to software decoder");
    // Fall back to the ROM-backed software decoder.
#endif
    return decode_with_rom_jpeg(src, src_len, out, out_len, width, height, stride, 0);
}

esp_err_t jpeg_to_image_with_caps(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                                  size_t* width, size_t* height, size_t* stride, uint32_t output_caps) {
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL || output_caps == 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    // Custom-capability output is a software-decoder contract. Hardware JPEG requires
    // driver-owned DMA buffers, so camera callers retain the hardware-first jpeg_to_image path.
    return decode_with_rom_jpeg(src, src_len, out, out_len, width, height, stride, output_caps);
}
