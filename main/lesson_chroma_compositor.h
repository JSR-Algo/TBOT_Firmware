#ifndef LESSON_CHROMA_COMPOSITOR_H
#define LESSON_CHROMA_COMPOSITOR_H

#include <cstddef>
#include <cstdint>

namespace tbot {

struct LessonRgbColor {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

struct LessonChromaKey {
    LessonRgbColor color;
    std::uint8_t tolerance;
    std::uint8_t feather;
};

struct LessonCinematicRect {
    std::int32_t x;
    std::int32_t y;
    std::uint16_t width;
    std::uint16_t height;
};

struct LessonRgb888View {
    const std::uint8_t* pixels;
    std::uint16_t width;
    std::uint16_t height;
    std::size_t stride;
};

struct LessonRgb565Surface {
    std::uint16_t* pixels;
    std::uint16_t width;
    std::uint16_t height;
    std::size_t stride_pixels;
};

struct LessonRgb565View {
    const std::uint16_t* pixels;
    std::uint16_t width;
    std::uint16_t height;
    std::size_t stride_pixels;
};

bool LessonCopyRgb888Background(const LessonRgb888View& source,
                                const LessonRgb565Surface& destination);
bool LessonCompositeRgb888(const LessonRgb888View& source,
                           const LessonRgb565Surface& destination,
                           const LessonCinematicRect& rect,
                           const LessonChromaKey& chroma);
bool LessonCopyRgb565Background(const LessonRgb565View& source,
                                const LessonRgb565Surface& destination);
bool LessonCompositeRgb565(const LessonRgb565View& source,
                           const LessonRgb565Surface& destination,
                           const LessonCinematicRect& rect,
                           const LessonChromaKey& chroma);

}  // namespace tbot

#endif  // LESSON_CHROMA_COMPOSITOR_H
