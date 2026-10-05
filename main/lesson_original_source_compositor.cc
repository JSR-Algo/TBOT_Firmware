#include "lesson_original_source_compositor.h"

#include <algorithm>
#include <cmath>

namespace tbot {
namespace {

struct Rgba { float r, g, b, a; };

float Clamp255(float value) { return std::fmin(255.0f, std::fmax(0.0f, value)); }

// Bilinear sample of one 8-bit plane at continuous coordinates (pixel centres at
// integer + 0.5), clamped to the edge as canvas drawImage does.
float SamplePlane(const std::uint8_t* plane, int stride, int width, int height, float x, float y) {
    const float fx = std::fmin(std::fmax(x - 0.5f, 0.0f), static_cast<float>(width - 1));
    const float fy = std::fmin(std::fmax(y - 0.5f, 0.0f), static_cast<float>(height - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float tx = fx - x0, ty = fy - y0;
    const float top = plane[y0 * stride + x0] + (plane[y0 * stride + x1] - plane[y0 * stride + x0]) * tx;
    const float bottom = plane[y1 * stride + x0] + (plane[y1 * stride + x1] - plane[y1 * stride + x0]) * tx;
    return top + (bottom - top) * ty;
}

// Straight-alpha RGBA sample at source coordinates (u, v) in source pixels.
Rgba Sample(const ComposeSource& source, float u, float v) {
    if (source.rgba) {
        // Interpolate premultiplied colour so transparent texels do not bleed, then
        // return straight colour (canvas filtering is premultiplied).
        const std::uint8_t* plane = source.planes[0];
        const int stride = source.strides[0];
        const float fx = std::fmin(std::fmax(u - 0.5f, 0.0f), static_cast<float>(source.width - 1));
        const float fy = std::fmin(std::fmax(v - 0.5f, 0.0f), static_cast<float>(source.height - 1));
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, source.width - 1), y1 = std::min(y0 + 1, source.height - 1);
        const float tx = fx - x0, ty = fy - y0;
        const float weights[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
        const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
        float r = 0, g = 0, b = 0, a = 0;
        for (int index = 0; index < 4; ++index) {
            const std::uint8_t* texel = plane + ys[index] * stride + xs[index] * 4;
            const float alpha = texel[3] / 255.0f * weights[index];
            r += texel[0] * alpha;
            g += texel[1] * alpha;
            b += texel[2] * alpha;
            a += alpha;
        }
        if (a <= 0) return {0, 0, 0, 0};
        return {r / a, g / a, b / a, a};
    }
    const float y = SamplePlane(source.planes[0], source.strides[0], source.width, source.height, u, v);
    const int chroma_width = (source.width + 1) / 2, chroma_height = (source.height + 1) / 2;
    const float cb = SamplePlane(source.planes[1], source.strides[1], chroma_width, chroma_height, u / 2, v / 2);
    const float cr = SamplePlane(source.planes[2], source.strides[2], chroma_width, chroma_height, u / 2, v / 2);
    // BT.601, limited (TV) range.
    const float luma = 1.164383f * (y - 16.0f);
    Rgba out{Clamp255(luma + 1.596027f * (cr - 128.0f)),
             Clamp255(luma - 0.391762f * (cb - 128.0f) - 0.812968f * (cr - 128.0f)),
             Clamp255(luma + 2.017232f * (cb - 128.0f)), 1.0f};
    if (source.alpha) {
        out.a = SamplePlane(source.planes[3], source.strides[3], source.width, source.height, u, v) / 255.0f;
    }
    return out;
}

}  // namespace

void DrawComposeSource(const ComposeSource& source, double x, double y, double w, double h, double opacity,
                       std::uint8_t* rgb) {
    if (w <= 0 || h <= 0 || opacity <= 0 || source.width <= 0 || source.height <= 0) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(x))), x1 = std::min(kTVideoStageWidth, static_cast<int>(std::ceil(x + w)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y))), y1 = std::min(kTVideoStageHeight, static_cast<int>(std::ceil(y + h)));
    const float scale_x = static_cast<float>(source.width / w), scale_y = static_cast<float>(source.height / h);
    for (int py = y0; py < y1; ++py) {
        const double cy = py + 0.5;
        if (cy < y || cy >= y + h) continue;
        for (int px = x0; px < x1; ++px) {
            const double cx = px + 0.5;
            if (cx < x || cx >= x + w) continue;
            const Rgba texel = Sample(source, static_cast<float>((cx - x) * scale_x), static_cast<float>((cy - y) * scale_y));
            const float alpha = texel.a * static_cast<float>(opacity);
            if (alpha <= 0) continue;
            std::uint8_t* pixel = rgb + (py * kTVideoStageWidth + px) * 3;
            pixel[0] = static_cast<std::uint8_t>(std::lround(texel.r * alpha + pixel[0] * (1 - alpha)));
            pixel[1] = static_cast<std::uint8_t>(std::lround(texel.g * alpha + pixel[1] * (1 - alpha)));
            pixel[2] = static_cast<std::uint8_t>(std::lround(texel.b * alpha + pixel[2] * (1 - alpha)));
        }
    }
}

void ComposeTVideoMediaLayers(const TVideoFrameLayout& layout, const ComposeSource& background,
                              const ComposeSource& teaching_object, const ComposeSource& robot, std::uint8_t* rgb) {
    // Stage colour #fff7df under everything, as the canonical renderer fills first.
    for (int index = 0; index < kTVideoStageWidth * kTVideoStageHeight; ++index) {
        rgb[index * 3] = 0xff;
        rgb[index * 3 + 1] = 0xf7;
        rgb[index * 3 + 2] = 0xdf;
    }
    DrawComposeSource(background, 0, 0, kTVideoStageWidth, kTVideoStageHeight, 1, rgb);
    DrawComposeSource(teaching_object, layout.object.x, layout.object.y, layout.object.size, layout.object.size,
              layout.object.opacity, rgb);
    const double base = layout.robot.base_size;
    DrawComposeSource(robot, layout.robot.anchor_x + (-base / 2) * layout.robot.scale_x,
              layout.robot.anchor_y + (-base) * layout.robot.scale_y, base * layout.robot.scale_x,
              base * layout.robot.scale_y, layout.robot.opacity, rgb);
}

std::size_t SelectFrameAtSeconds(const std::vector<std::int64_t>& pts, int time_base_num, int time_base_den,
                                 double seconds) {
    std::size_t selected = 0;
    for (std::size_t index = 0; index < pts.size(); ++index) {
        // Exact rational comparison: pts * num / den <= seconds.
        if (static_cast<double>(pts[index]) * time_base_num <= seconds * time_base_den + 1e-9) selected = index;
        else break;
    }
    return selected;
}

double CanonicalMediaSeconds(double media_time_ms, double duration_seconds) {
    return std::fmin(std::fmax(0.0, media_time_ms / 1000), std::fmax(0.0, duration_seconds - 0.001));
}

void ConvertRgb888ToRgb565(const std::uint8_t* rgb, std::uint16_t* out, std::size_t pixels) {
    for (std::size_t index = 0; index < pixels; ++index) {
        const std::uint8_t* pixel = rgb + index * 3;
        out[index] = static_cast<std::uint16_t>(((pixel[0] & 0xf8) << 8) | ((pixel[1] & 0xfc) << 3) | (pixel[2] >> 3));
    }
}

}  // namespace tbot
