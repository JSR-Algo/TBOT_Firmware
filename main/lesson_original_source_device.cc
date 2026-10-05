#include "lesson_original_source_device.h"

#include "display/lcd_display.h"
#include "lesson_original_source_runtime.h"

#include <lvgl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace tbot {
namespace {

// The board text font's nominal pixel size (tbot_vietnamese_20_4).
constexpr int kNativeFontPx = 20;

std::uint32_t NextCodePoint(const std::string& text, std::size_t* index) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char lead = byte(*index);
    int length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
    if (*index + length > text.size()) length = 1;
    std::uint32_t code = length == 1 ? lead : lead & (0x7f >> length);
    for (int offset = 1; offset < length; ++offset) code = (code << 6) | (byte(*index + offset) & 0x3f);
    *index += length;
    return code;
}

// Glyph metrics and A8 coverage copied out of LVGL once, under the display lock;
// the frame task then draws without touching LVGL state.
class LvglTextRenderer final : public TVideoTextRenderer {
public:
    LvglTextRenderer(Display* display, const lv_font_t* font) : display_(display), font_(font) {}

    double Measure(const std::string& text, int font_px) override {
        const double scale = static_cast<double>(font_px) / kNativeFontPx;
        double width = 0;
        for (std::size_t index = 0; index < text.size();) width += Get(NextCodePoint(text, &index)).advance * scale;
        return width;
    }

    void Render(const std::string& text, int font_px, double scale, double x, double baseline,
                const TVideoCoveragePlot& plot) override {
        const double factor = static_cast<double>(font_px) / kNativeFontPx * scale;
        if (factor <= 0) return;
        double pen = x;
        for (std::size_t index = 0; index < text.size();) {
            const Glyph& glyph = Get(NextCodePoint(text, &index));
            const double left = pen + glyph.ofs_x * factor;
            const double top = baseline - (glyph.ofs_y + glyph.height) * factor;
            const int width = static_cast<int>(glyph.width * factor + 0.5);
            const int height = static_cast<int>(glyph.height * factor + 0.5);
            for (int row = 0; row < height; ++row) {
                const int source_row = std::min(glyph.height - 1, static_cast<int>((row + 0.5) / factor));
                for (int column = 0; column < width; ++column) {
                    const int source_column = std::min(glyph.width - 1, static_cast<int>((column + 0.5) / factor));
                    const std::uint8_t coverage = glyph.a8[source_row * glyph.width + source_column];
                    if (coverage != 0) {
                        plot(static_cast<int>(left) + column, static_cast<int>(top) + row, coverage / 255.0f);
                    }
                }
            }
            pen += glyph.advance * factor;
        }
    }

private:
    struct Glyph {
        int advance = 0, width = 0, height = 0, ofs_x = 0, ofs_y = 0;
        std::vector<std::uint8_t> a8;
    };

    const Glyph& Get(std::uint32_t letter) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = cache_.find(letter);
        if (found != cache_.end()) return found->second;
        Glyph glyph;
        {
            DisplayLockGuard display_lock(display_);
            lv_font_glyph_dsc_t dsc{};
            if (lv_font_get_glyph_dsc(font_, &dsc, letter, 0)) {
                glyph.advance = dsc.adv_w;
                glyph.width = dsc.box_w;
                glyph.height = dsc.box_h;
                glyph.ofs_x = dsc.ofs_x;
                glyph.ofs_y = dsc.ofs_y;
                if (glyph.width > 0 && glyph.height > 0) {
                    lv_draw_buf_t* buffer =
                        lv_draw_buf_create(glyph.width, glyph.height, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
                    const auto* bitmap =
                        buffer != nullptr ? static_cast<const lv_draw_buf_t*>(lv_font_get_glyph_bitmap(&dsc, buffer))
                                          : nullptr;
                    if (bitmap != nullptr && bitmap->data != nullptr) {
                        glyph.a8.resize(static_cast<std::size_t>(glyph.width) * glyph.height);
                        for (int row = 0; row < glyph.height; ++row) {
                            std::memcpy(glyph.a8.data() + row * glyph.width,
                                        bitmap->data + row * bitmap->header.stride, glyph.width);
                        }
                    } else {
                        glyph.width = glyph.height = 0;
                    }
                    lv_font_glyph_release_draw_data(&dsc);
                    if (buffer != nullptr) lv_draw_buf_destroy(buffer);
                }
            }
        }
        return cache_.emplace(letter, std::move(glyph)).first->second;
    }

    Display* display_;
    const lv_font_t* font_;
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, Glyph> cache_;
};

class LessonFramebufferPanel final : public LcdDisplayPresenter {
public:
    explicit LessonFramebufferPanel(::LcdDisplay* display) : display_(display) {}
    bool Present(const std::uint16_t* rgb565, int width, int height) override {
        return display_->PresentLessonFramebuffer(rgb565, static_cast<std::uint16_t>(width),
                                                  static_cast<std::uint16_t>(height));
    }

private:
    ::LcdDisplay* display_;
};

}  // namespace

bool InitializeProductionOriginalSourceDevice(::LcdDisplay* display) {
    if (display == nullptr) return false;
    static LvglTextRenderer text(display, &BUILTIN_TEXT_FONT);
    static LessonFramebufferPanel panel(display);
    if (!InitializeProductionOriginalSourceRuntime(&panel, &text)) return false;
#ifdef CONFIG_TBOT_LESSON_RENDERER_V6_ADVERTISE
    SetOriginalSourceRendererAdvertised(true);
#else
    SetOriginalSourceRendererAdvertised(false);
#endif
    return true;
}

}  // namespace tbot
