#include "lesson_chroma_compositor.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace tbot {
namespace {

bool Valid(const LessonRgb888View& view) {
    return view.pixels != nullptr && view.width != 0 && view.height != 0 &&
           view.stride >= static_cast<std::size_t>(view.width) * 3;
}

bool Valid(const LessonRgb565Surface& surface) {
    return surface.pixels != nullptr && surface.width != 0 && surface.height != 0 &&
           surface.stride_pixels >= surface.width;
}

bool Valid(const LessonRgb565View& view) {
    return view.pixels != nullptr && view.width != 0 && view.height != 0 &&
           view.stride_pixels >= view.width;
}

std::uint16_t ToRgb565(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return static_cast<std::uint16_t>(((red & 0xf8u) << 8) |
                                      ((green & 0xfcu) << 3) | (blue >> 3));
}

std::uint8_t Red565(std::uint16_t pixel) {
    const std::uint8_t value = static_cast<std::uint8_t>((pixel >> 11) & 0x1f);
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

std::uint8_t Green565(std::uint16_t pixel) {
    const std::uint8_t value = static_cast<std::uint8_t>((pixel >> 5) & 0x3f);
    return static_cast<std::uint8_t>((value << 2) | (value >> 4));
}

std::uint8_t Blue565(std::uint16_t pixel) {
    const std::uint8_t value = static_cast<std::uint8_t>(pixel & 0x1f);
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

std::uint8_t Difference(std::uint8_t left, std::uint8_t right) {
    return left >= right ? static_cast<std::uint8_t>(left - right)
                         : static_cast<std::uint8_t>(right - left);
}

std::uint16_t Blend(std::uint16_t destination, const std::uint8_t* source,
                    std::uint16_t alpha_256) {
    const std::uint16_t inverse = static_cast<std::uint16_t>(256 - alpha_256);
    const auto blend_channel = [alpha_256, inverse](std::uint8_t foreground,
                                                     std::uint8_t background) {
        return static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(foreground) * alpha_256 +
             static_cast<std::uint32_t>(background) * inverse + 128) >> 8);
    };
    return ToRgb565(blend_channel(source[0], Red565(destination)),
                    blend_channel(source[1], Green565(destination)),
                    blend_channel(source[2], Blue565(destination)));
}

}  // namespace

bool LessonCopyRgb888Background(const LessonRgb888View& source,
                                const LessonRgb565Surface& destination) {
    if (!Valid(source) || !Valid(destination) || source.width != destination.width ||
        source.height != destination.height) {
        return false;
    }
    for (std::size_t y = 0; y < source.height; ++y) {
        const std::uint8_t* input = source.pixels + y * source.stride;
        std::uint16_t* output = destination.pixels + y * destination.stride_pixels;
        for (std::size_t x = 0; x < source.width; ++x) {
            output[x] = ToRgb565(input[x * 3], input[x * 3 + 1], input[x * 3 + 2]);
        }
    }
    return true;
}

bool LessonCompositeRgb888(const LessonRgb888View& source,
                           const LessonRgb565Surface& destination,
                           const LessonCinematicRect& rect,
                           const LessonChromaKey& chroma) {
    if (!Valid(source) || !Valid(destination) || rect.width == 0 || rect.height == 0) {
        return false;
    }
    const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
    const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
    const std::int64_t clipped_left = std::max<std::int64_t>(0, rect.x);
    const std::int64_t clipped_top = std::max<std::int64_t>(0, rect.y);
    const std::int64_t clipped_right = std::min<std::int64_t>(destination.width, right);
    const std::int64_t clipped_bottom = std::min<std::int64_t>(destination.height, bottom);
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) return true;

    for (std::int64_t y = clipped_top; y < clipped_bottom; ++y) {
        const std::uint64_t relative_y = static_cast<std::uint64_t>(y - rect.y);
        const std::size_t source_y = static_cast<std::size_t>(
            relative_y * source.height / rect.height);
        std::uint16_t* output = destination.pixels +
                                static_cast<std::size_t>(y) * destination.stride_pixels;
        for (std::int64_t x = clipped_left; x < clipped_right; ++x) {
            const std::uint64_t relative_x = static_cast<std::uint64_t>(x - rect.x);
            const std::size_t source_x = static_cast<std::size_t>(
                relative_x * source.width / rect.width);
            const std::uint8_t* pixel = source.pixels + source_y * source.stride + source_x * 3;
            const std::uint8_t distance = std::max({
                Difference(pixel[0], chroma.color.red),
                Difference(pixel[1], chroma.color.green),
                Difference(pixel[2], chroma.color.blue),
            });
            if (distance <= chroma.tolerance) continue;
            std::uint16_t alpha = 256;
            if (chroma.feather != 0 &&
                distance < static_cast<unsigned>(chroma.tolerance) + chroma.feather) {
                alpha = static_cast<std::uint16_t>(
                    (static_cast<unsigned>(distance - chroma.tolerance) * 256u) /
                    chroma.feather);
            }
            output[x] = alpha == 256 ? ToRgb565(pixel[0], pixel[1], pixel[2])
                                     : Blend(output[x], pixel, alpha);
        }
    }
    return true;
}

bool LessonCopyRgb565Background(const LessonRgb565View& source,
                                const LessonRgb565Surface& destination) {
    if (!Valid(source) || !Valid(destination) || source.width != destination.width ||
        source.height != destination.height) {
        return false;
    }
    for (std::size_t y = 0; y < source.height; ++y) {
        std::copy_n(source.pixels + y * source.stride_pixels, source.width,
                    destination.pixels + y * destination.stride_pixels);
    }
    return true;
}

bool LessonCompositeRgb565(const LessonRgb565View& source,
                           const LessonRgb565Surface& destination,
                           const LessonCinematicRect& rect,
                           const LessonChromaKey& chroma) {
    if (!Valid(source) || !Valid(destination) || rect.width == 0 || rect.height == 0) {
        return false;
    }
    const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
    const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
    const std::int64_t left = std::max<std::int64_t>(0, rect.x);
    const std::int64_t top = std::max<std::int64_t>(0, rect.y);
    const std::int64_t clipped_right = std::min<std::int64_t>(destination.width, right);
    const std::int64_t clipped_bottom = std::min<std::int64_t>(destination.height, bottom);
    if (left >= clipped_right || top >= clipped_bottom) return true;
    for (std::int64_t y = top; y < clipped_bottom; ++y) {
        const std::size_t source_y = static_cast<std::size_t>(
            static_cast<std::uint64_t>(y - rect.y) * source.height / rect.height);
        std::uint16_t* output = destination.pixels +
                                static_cast<std::size_t>(y) * destination.stride_pixels;
        for (std::int64_t x = left; x < clipped_right; ++x) {
            const std::size_t source_x = static_cast<std::size_t>(
                static_cast<std::uint64_t>(x - rect.x) * source.width / rect.width);
            const std::uint16_t pixel = source.pixels[source_y * source.stride_pixels + source_x];
            const std::uint8_t rgb[3] = {Red565(pixel), Green565(pixel), Blue565(pixel)};
            const std::uint8_t distance = std::max({
                Difference(rgb[0], chroma.color.red), Difference(rgb[1], chroma.color.green),
                Difference(rgb[2], chroma.color.blue)});
            if (distance <= chroma.tolerance) continue;
            std::uint16_t alpha = 256;
            if (chroma.feather != 0 &&
                distance < static_cast<unsigned>(chroma.tolerance) + chroma.feather) {
                alpha = static_cast<std::uint16_t>(
                    static_cast<unsigned>(distance - chroma.tolerance) * 256u / chroma.feather);
            }
            output[x] = alpha == 256 ? pixel : Blend(output[x], rgb, alpha);
        }
    }
    return true;
}

}  // namespace tbot
