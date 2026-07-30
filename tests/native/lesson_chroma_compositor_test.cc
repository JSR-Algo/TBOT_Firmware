#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "lesson_chroma_compositor.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson_chroma_compositor test failed: " << message << '\n';
        std::exit(1);
    }
}

std::uint16_t Rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint16_t>(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

void TestRgb888BackgroundAndChromaBoundaries() {
    constexpr std::uint16_t kCanary = 0xa55a;
    std::array<std::uint16_t, 8> guarded = {kCanary, kCanary, 0, 0, 0, 0, kCanary, kCanary};
    const std::array<std::uint8_t, 12> background = {
        10, 20, 30, 40, 50, 60,
        70, 80, 90, 100, 110, 120,
    };
    tbot::LessonRgb565Surface target{guarded.data() + 2, 2, 2, 2};
    Require(tbot::LessonCopyRgb888Background({background.data(), 2, 2, 6}, target),
            "RGB888 background copies");
    Require(target.pixels[0] == Rgb565(10, 20, 30) &&
                target.pixels[3] == Rgb565(100, 110, 120),
            "RGB888 converts to RGB565");

    const std::array<std::uint8_t, 12> foreground = {
        0, 255, 0,       // transparent: distance 0
        20, 255, 0,      // tolerance boundary: transparent
        25, 255, 0,      // feather: half blend for tolerance=20, feather=10
        40, 0, 200,      // opaque
    };
    const auto before_blend = target.pixels[0];
    Require(tbot::LessonCompositeRgb888(
                {foreground.data(), 2, 2, 6}, target, {0, 0, 2, 2},
                {{0, 255, 0}, 20, 10}),
            "foreground composites");
    Require(target.pixels[0] == before_blend, "exact key is transparent");
    Require(target.pixels[1] == Rgb565(40, 50, 60), "tolerance boundary is transparent");
    Require(target.pixels[2] != Rgb565(70, 80, 90) && target.pixels[2] != Rgb565(25, 255, 0),
            "feather band blends source and destination");
    Require(target.pixels[3] == Rgb565(40, 0, 200), "outside feather is opaque");
    Require(guarded[0] == kCanary && guarded[1] == kCanary &&
                guarded[6] == kCanary && guarded[7] == kCanary,
            "compositor does not write outside framebuffer");
}

void TestClippingScalingAndLayerOrder() {
    constexpr std::uint16_t kCanary = 0x5aa5;
    std::array<std::uint16_t, 20> guarded{};
    guarded.fill(kCanary);
    tbot::LessonRgb565Surface target{guarded.data() + 2, 4, 4, 4};
    const std::array<std::uint8_t, 48> black{};
    Require(tbot::LessonCopyRgb888Background({black.data(), 4, 4, 12}, target),
            "black background copies");

    const std::array<std::uint8_t, 12> object = {
        255, 0, 0, 255, 0, 0,
        255, 0, 0, 255, 0, 0,
    };
    Require(tbot::LessonCompositeRgb888(
                {object.data(), 2, 2, 6}, target, {-1, -1, 4, 4},
                {{0, 255, 0}, 0, 0}),
            "negative rectangle is clipped while nearest-neighbor scaling");
    Require(target.pixels[0] == Rgb565(255, 0, 0) && target.pixels[10] == Rgb565(255, 0, 0),
            "clipped scaled object covers its visible region");

    const std::array<std::uint8_t, 3> robot = {0, 0, 255};
    Require(tbot::LessonCompositeRgb888(
                {robot.data(), 1, 1, 3}, target, {1, 1, 2, 2},
                {{0, 255, 0}, 0, 0}),
            "robot overlay composites after object");
    Require(target.pixels[5] == Rgb565(0, 0, 255) && target.pixels[10] == Rgb565(0, 0, 255),
            "robot overlay wins layer order");
    Require(guarded[0] == kCanary && guarded[1] == kCanary &&
                guarded[18] == kCanary && guarded[19] == kCanary,
            "clipping preserves canaries");

    Require(tbot::LessonCompositeRgb888(
                {robot.data(), 1, 1, 3}, target, {20, 20, 2, 2},
                {{0, 255, 0}, 0, 0}),
            "fully outside rectangle is a safe no-op");
}

}  // namespace

int main() {
    TestRgb888BackgroundAndChromaBoundaries();
    TestClippingScalingAndLayerOrder();
    std::cout << "lesson_chroma_compositor test passed\n";
    return 0;
}
