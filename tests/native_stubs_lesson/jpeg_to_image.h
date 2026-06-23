#pragma once
// Host stub for the JPEG decoder. Real jpeg_to_image does hardware/software JPEG
// decode -> RGB565 (driver-coupled, CP-7). On the host we substitute a scriptable
// decoder so the JPEG branch of DecodeLessonImageBytes (success + failure guards) is
// reachable without faking real pixels. Documented stubbed boundary.
#include "esp_err.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Decode outcome controls for the next jpeg_to_image() call.
//   mode 0: success — allocate a small RGB565 buffer with heap_caps-equivalent malloc.
//   mode 1: return non-ESP_OK (decode failed).
//   mode 2: return ESP_OK but with a degenerate (zero) dimension so the renderer's
//           post-decode validation guard rejects it.
//   mode 3: return ESP_OK with a decoded RGB565 footprint above the firmware budget.
inline int& HostJpegDecodeMode() {
    static int v = 0;
    return v;
}

inline esp_err_t jpeg_to_image(const uint8_t* /*src*/, size_t /*src_len*/, uint8_t** out,
                               size_t* out_len, size_t* width, size_t* height,
                               size_t* stride) {
    int mode = HostJpegDecodeMode();
    if (mode == 1) {
        *out = nullptr;
        *out_len = 0;
        *width = *height = *stride = 0;
        return ESP_FAIL;
    }
    if (mode == 2) {
        // ESP_OK but degenerate: data present, zero geometry -> renderer must reject.
        *out = static_cast<uint8_t*>(std::malloc(4));
        *out_len = 4;
        *width = 0;  // triggers the (width==0 ...) guard
        *height = 2;
        *stride = 4;
        return ESP_OK;
    }
    if (mode == 3) {
        // ESP_OK but too large once decoded: 400x400 RGB565 = 320000 bytes.
        const size_t len = 400 * 400 * 2;
        *out = static_cast<uint8_t*>(std::malloc(len));
        std::memset(*out, 0, len);
        *out_len = len;
        *width = 400;
        *height = 400;
        *stride = 400 * 2;
        return ESP_OK;
    }
    // Success: 2x2 RGB565 = 8 bytes. Allocated with malloc so heap_caps_free frees it.
    const size_t len = 8;
    *out = static_cast<uint8_t*>(std::malloc(len));
    std::memset(*out, 0, len);
    *out_len = len;
    *width = 2;
    *height = 2;
    *stride = 4;
    return ESP_OK;
}
