#pragma once
// Host stub for main/display/lvgl_display/lvgl_image.h.
//
// LvglAllocatedImage is the decoded-bytes wrapper lesson_handler.cc constructs after
// fetch/decode. The real ctor THROWS std::runtime_error when LVGL cannot decode the
// bytes (and, per the source contract, does NOT free `data` on throw). We reproduce
// BOTH ctors and that throw-without-free contract so the renderer's try/catch +
// heap_caps_free(data) on the catch branch is exercised honestly.
//
// HostImageDecodeShouldThrow() lets a test force the decode-failure branch. We do NOT
// pull in real LVGL: software-only, real pixel decode is hardware/driver-coupled
// (CP-7); the genuine on-device decode is a documented stubbed boundary.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#define LV_COLOR_FORMAT_RGB565 17

// When true, the next LvglAllocatedImage ctor throws (simulates an undecodable image,
// e.g. an HTML error page served as 200 / truncated bytes). Auto-resets to false after
// each throw so a test sets it per-construction.
inline bool& HostImageDecodeShouldThrow() {
    static bool v = false;
    return v;
}
// Records the most recent successful single-arg (raw bytes) construction so a test can
// assert the renderer handed the fetched buffer to the wrapper.
inline const void*& HostLastDecodedDataPtr() {
    static const void* p = nullptr;
    return p;
}
inline size_t& HostLastDecodedSize() {
    static size_t s = 0;
    return s;
}

class LvglImage {
public:
    virtual ~LvglImage() = default;
};

class LvglAllocatedImage : public LvglImage {
public:
    // Single-arg ctor: LVGL-native (PNG etc.) decode probe. Throws on undecodable
    // bytes WITHOUT freeing `data` (matches the real contract the catch relies on).
    LvglAllocatedImage(void* data, size_t size) {
        if (HostImageDecodeShouldThrow()) {
            HostImageDecodeShouldThrow() = false;
            throw std::runtime_error("host: undecodable image");
        }
        HostLastDecodedDataPtr() = data;
        HostLastDecodedSize() = size;
        owned_ = data;  // takes ownership on success (freed in dtor)
    }
    // Six-arg ctor: pre-decoded RGB565 buffer (JPEG path). Same throw contract.
    LvglAllocatedImage(void* data, size_t size, int /*w*/, int /*h*/, int /*stride*/,
                       int /*color_format*/) {
        if (HostImageDecodeShouldThrow()) {
            HostImageDecodeShouldThrow() = false;
            throw std::runtime_error("host: decoded jpeg rejected");
        }
        HostLastDecodedDataPtr() = data;
        HostLastDecodedSize() = size;
        owned_ = data;
    }
    ~LvglAllocatedImage() override {
        // On success the wrapper owns the buffer; free it like the real dtor so the
        // host has no leak on the success path.
        if (owned_) std::free(owned_);
    }

private:
    void* owned_ = nullptr;
};
