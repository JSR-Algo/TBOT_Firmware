#include "lesson_original_source_compositor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace tbot;

namespace {
int failures = 0;
void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++failures;
    }
}

struct Yuv {
    std::vector<std::uint8_t> bytes;
    ComposeSource source;
};

// Solid YUV 4:2:0 frame (BT.601 limited values), optional constant alpha plane.
Yuv SolidYuv(int width, int height, int y, int cb, int cr, int alpha = -1) {
    Yuv frame;
    const int chroma = ((width + 1) / 2) * ((height + 1) / 2);
    frame.bytes.assign(width * height + 2 * chroma + (alpha >= 0 ? width * height : 0), 0);
    std::fill(frame.bytes.begin(), frame.bytes.begin() + width * height, static_cast<std::uint8_t>(y));
    std::fill(frame.bytes.begin() + width * height, frame.bytes.begin() + width * height + chroma, static_cast<std::uint8_t>(cb));
    std::fill(frame.bytes.begin() + width * height + chroma, frame.bytes.begin() + width * height + 2 * chroma,
              static_cast<std::uint8_t>(cr));
    if (alpha >= 0) std::fill(frame.bytes.begin() + width * height + 2 * chroma, frame.bytes.end(), static_cast<std::uint8_t>(alpha));
    auto& s = frame.source;
    s.planes[0] = frame.bytes.data();
    s.planes[1] = frame.bytes.data() + width * height;
    s.planes[2] = frame.bytes.data() + width * height + chroma;
    s.planes[3] = alpha >= 0 ? frame.bytes.data() + width * height + 2 * chroma : nullptr;
    s.strides[0] = s.strides[3] = width;
    s.strides[1] = s.strides[2] = (width + 1) / 2;
    s.width = width;
    s.height = height;
    s.alpha = alpha >= 0;
    return frame;
}

const std::uint8_t* Pixel(const std::vector<std::uint8_t>& rgb, int x, int y) { return &rgb[(y * 480 + x) * 3]; }
}  // namespace

int main() {
    // BT.601 limited: Y=16 black, Y=235 white, (Y=81,Cb=90,Cr=240) pure red.
    Yuv black = SolidYuv(480, 320, 16, 128, 128);
    Yuv red = SolidYuv(480, 480, 81, 90, 240, 255);
    Yuv half_white = SolidYuv(480, 480, 235, 128, 128, 128);
    std::vector<std::uint8_t> rgba(512 * 512 * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) { rgba[i] = 0; rgba[i + 1] = 255; rgba[i + 2] = 0; rgba[i + 3] = 255; }
    ComposeSource green;
    green.planes[0] = rgba.data();
    green.strides[0] = 512 * 4;
    green.width = green.height = 512;
    green.rgba = true;

    TVideoFrameLayout layout;
    layout.object = {100, 100, 50, 0.5, 125, 125, 0};
    layout.robot.anchor_x = 300;
    layout.robot.anchor_y = 300;
    layout.robot.scale_x = layout.robot.scale_y = 0.6;
    layout.robot.base_size = 150 / .9;
    layout.robot.opacity = 1;
    std::vector<std::uint8_t> rgb(480 * 320 * 3);
    ComposeTVideoMediaLayers(layout, black.source, green, red.source, rgb.data());

    Expect(Pixel(rgb, 5, 5)[0] == 0 && Pixel(rgb, 5, 5)[1] == 0 && Pixel(rgb, 5, 5)[2] == 0, "opaque black background");
    // Object: green at 50% over black -> (0, 128, 0); rect [100,150) x [100,150).
    const auto* object = Pixel(rgb, 125, 125);
    Expect(object[0] == 0 && std::abs(object[1] - 128) <= 1 && object[2] == 0, "object globalAlpha blend");
    Expect(Pixel(rgb, 99, 125)[1] == 0 && Pixel(rgb, 150, 125)[1] == 0, "object rect bounds");
    // Robot: base 166.67 * 0.6 = 100 square, bottom-centre at (300, 300): x [250,350), y [200,300).
    const auto* robot = Pixel(rgb, 300, 250);
    Expect(std::abs(robot[0] - 255) <= 1 && robot[1] <= 1 && robot[2] <= 1, "robot BT.601 red");
    Expect(Pixel(rgb, 300, 299)[0] > 250 && Pixel(rgb, 300, 300)[0] == 0, "robot bottom anchor");
    Expect(Pixel(rgb, 249, 250)[0] == 0 && Pixel(rgb, 250, 250)[0] > 250, "robot left edge");

    // Straight alpha 128/255 white over black.
    ComposeTVideoMediaLayers(layout, black.source, green, half_white.source, rgb.data());
    Expect(std::abs(Pixel(rgb, 300, 250)[0] - 128) <= 1, "robot alpha plane blend");
    // Opacity 0 draws nothing.
    layout.robot.opacity = 0;
    ComposeTVideoMediaLayers(layout, black.source, green, red.source, rgb.data());
    Expect(Pixel(rgb, 300, 250)[0] == 0, "robot opacity 0");

    // Browser frame choice: last pts <= t; clamps to the last frame beyond the end.
    const std::vector<std::int64_t> pts = {0, 42, 83, 125};  // 1/1000 time base
    Expect(SelectFrameAtSeconds(pts, 1, 1000, 0.0) == 0, "frame at 0");
    Expect(SelectFrameAtSeconds(pts, 1, 1000, 0.0419) == 0, "before second frame");
    Expect(SelectFrameAtSeconds(pts, 1, 1000, 0.042) == 1, "exactly on pts");
    Expect(SelectFrameAtSeconds(pts, 1, 1000, 9.0) == 3, "beyond end");
    Expect(CanonicalMediaSeconds(9500, 3.209) == 3.209 - 0.001 && CanonicalMediaSeconds(-5, 3) == 0, "seek clamp");

    std::uint8_t sample[6] = {255, 255, 255, 0x12, 0x34, 0x56};
    std::uint16_t packed[2];
    ConvertRgb888ToRgb565(sample, packed, 2);
    Expect(packed[0] == 0xffff && packed[1] == static_cast<std::uint16_t>(((0x12 & 0xf8) << 8) | ((0x34 & 0xfc) << 3) | (0x56 >> 3)),
           "rgb565 packing");
    if (failures != 0) return 1;
    std::printf("PASS original-source compositor host checks\n");
    return 0;
}
