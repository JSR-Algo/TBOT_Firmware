#pragma once

#include <cstdint>
#include <cstring>

// Composite on black and duplicate pixels for the 240x160 conversation GIFs.
inline void UpscaleOpaqueGifRgb565(const std::uint8_t* source,
                                  std::uint16_t* output, int width, int height) {
    for (int y = 0; y < height; ++y) {
        auto* row = output + y * 2 * width * 2;
        for (int x = 0; x < width; ++x) {
            const auto* pixel = source + (y * width + x) * 4;
            const unsigned alpha = pixel[3];
            const unsigned red = pixel[2] * alpha / 255;
            const unsigned green = pixel[1] * alpha / 255;
            const unsigned blue = pixel[0] * alpha / 255;
            const std::uint16_t color = ((red & 0xf8) << 8) |
                                        ((green & 0xfc) << 3) | (blue >> 3);
            row[x * 2] = row[x * 2 + 1] = color;
        }
        std::memcpy(row + width * 2, row, width * 2 * sizeof(*row));
    }
}
