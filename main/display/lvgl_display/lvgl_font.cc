#include "lvgl_font.h"
#include <cbin_font.h>

extern const lv_font_t tbot_vietnamese_20_4;

LvglCBinFont::LvglCBinFont(void* data) {
    font_ = cbin_font_create(static_cast<uint8_t*>(data));
    if (font_ != nullptr && font_->fallback == nullptr) {
        font_->fallback = &tbot_vietnamese_20_4;
    }
}

LvglCBinFont::~LvglCBinFont() {
    if (font_ != nullptr) {
        cbin_font_delete(font_);
    }
}
