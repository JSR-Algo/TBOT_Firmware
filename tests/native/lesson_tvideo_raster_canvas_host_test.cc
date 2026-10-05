// Geometry of the renderer-v6 software canvas: antialiased coverage integrates to
// the exact shape areas, strokes are rings of the line width, and clipping, alpha,
// colour parsing, media placement and text placement follow canvas semantics.
#include "lesson_tvideo_raster_canvas.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tbot;

namespace {
int failures = 0, checks = 0;
void Expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition && failures++ < 20) std::fprintf(stderr, "FAIL %s\n", message.c_str());
}
constexpr double kPi = 3.14159265358979323846;

struct Stage {
    std::vector<std::uint8_t> rgb = std::vector<std::uint8_t>(480 * 320 * 3, 0);
    const std::uint8_t* At(int x, int y) const { return rgb.data() + (y * 480 + x) * 3; }
    // Painted area in pixels, from the red channel of white-on-black drawing.
    double Area() const {
        double sum = 0;
        for (std::size_t index = 0; index < rgb.size(); index += 3) sum += rgb[index] / 255.0;
        return sum;
    }
};

bool Near(double actual, double expected, double relative) {
    return std::fabs(actual - expected) <= std::fabs(expected) * relative;
}

class BoxText final : public TVideoTextRenderer {
public:
    double origin_x = 0, origin_y = 0, scale = 0;
    double Measure(const std::string& text, int font_px) override { return text.size() * font_px * .5; }
    void Render(const std::string& text, int font_px, double s, double x, double baseline,
                const TVideoCoveragePlot& plot) override {
        origin_x = x;
        origin_y = baseline;
        scale = s;
        // One fully covered pixel per character at the baseline row above.
        for (std::size_t index = 0; index < text.size(); ++index) {
            plot(static_cast<int>(x + index * font_px * .5 * s), static_cast<int>(baseline) - 1, 1.0f);
        }
    }
};
}  // namespace

int main() {
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetFillStyle("#ffffff");
        canvas.FillRect(10, 10, 20, 10);
        Expect(stage.At(10, 10)[0] == 255 && stage.At(29, 19)[0] == 255 && stage.At(30, 10)[0] == 0 &&
                   stage.At(9, 10)[0] == 0, "pixel-aligned rect is exact");
        Expect(std::fabs(stage.Area() - 200) < 1e-6, "pixel-aligned rect area");
        canvas.FillRect(100.5, 100, 2, 1);
        Expect(std::abs(stage.At(100, 100)[0] - 128) <= 1 && stage.At(101, 100)[0] == 255 &&
                   std::abs(stage.At(102, 100)[0] - 128) <= 1, "half-pixel edges are half covered");
    }
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetFillStyle("#ffffff");
        canvas.BeginPath();
        canvas.Arc(100, 100, 20, 0, kPi * 2);
        canvas.Fill();
        Expect(Near(stage.Area(), kPi * 400, .003), "circle area " + std::to_string(stage.Area()));
        Expect(stage.At(100, 100)[0] == 255 && stage.At(100, 79)[0] < 40 && stage.At(100, 75)[0] == 0, "circle extent");
    }
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetStrokeStyle("#ffffff");
        canvas.SetLineWidth(2);
        canvas.BeginPath();
        canvas.Arc(200, 150, 30, 0, kPi * 2);
        canvas.Stroke();
        Expect(Near(stage.Area(), 2 * kPi * 30 * 2, .01), "circle stroke ring area " + std::to_string(stage.Area()));
        Expect(stage.At(200, 150)[0] == 0, "stroke leaves the inside unpainted");
    }
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetFillStyle("#ffffff");
        canvas.BeginPath();
        canvas.RoundRect(25, 35, 190, 92, 18);
        canvas.Fill();
        Expect(Near(stage.Area(), 190 * 92 - (4 - kPi) * 18 * 18, .002), "round rect area");
        Expect(stage.At(25, 35)[0] == 0 && stage.At(120, 35)[0] == 255, "round rect corner is cut, edge is full");
        Stage pill;
        TVideoRasterCanvas pill_canvas(pill.rgb.data(), nullptr);
        pill_canvas.SetFillStyle("#ffffff");
        pill_canvas.BeginPath();
        pill_canvas.RoundRect(10, 10, 20, 10, 14);  // radius clamps to 5: a stadium
        pill_canvas.Fill();
        Expect(Near(pill.Area(), 10 * 10 + kPi * 25, .01), "oversized radius clamps like canvas");
    }
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetStrokeStyle("#ffffff");
        canvas.SetLineWidth(2);
        canvas.BeginPath();
        canvas.Arc(200, 150, 17, kPi * 1.08, kPi * 1.92);
        canvas.Stroke();
        Expect(Near(stage.Area(), kPi * .84 * 17 * 2, .02), "open arc stroke has butt caps");
        Expect(stage.At(200, 150 + 17)[0] == 0, "open arc is not closed");
    }
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetFillStyle("#ffffff");
        canvas.Save();
        canvas.Translate(240, 160);
        canvas.Rotate(0.7);
        canvas.FillRect(-8, -8, 16, 16);
        canvas.Restore();
        Expect(Near(stage.Area(), 256, .005), "rotated confetti square keeps its area");
        canvas.BeginPath();
        canvas.Ellipse(100, 300, 35, 7, 0, 0, kPi * 2);
        canvas.Fill();
        Expect(Near(stage.Area(), 256 + kPi * 35 * 7, .01), "shadow ellipse area");
    }
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetFillStyle("#ffffff");
        canvas.BeginPath();
        canvas.MoveTo(184, 45);
        canvas.LineTo(201, 51);
        canvas.LineTo(184, 57);
        canvas.ClosePath();
        canvas.Fill();
        Expect(Near(stage.Area(), 17 * 12 / 2.0, .01), "word-transition arrow triangle");
    }
    {
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.Save();
        canvas.BeginPath();
        canvas.Rect(25.5, 35, 10, 10);
        canvas.Clip();
        canvas.SetFillStyle("#ffffff");
        canvas.FillRect(0, 0, 480, 320);
        canvas.Restore();
        Expect(std::fabs(stage.Area() - 100) < .1 && std::abs(stage.At(25, 40)[0] - 128) <= 1 &&
                   stage.At(26, 40)[0] == 255 && stage.At(36, 40)[0] == 0, "antialiased rect clip");
        canvas.SetFillStyle("#ffffff");  // the fill set inside save() was restored too
        canvas.FillRect(0, 0, 1, 1);
        Expect(stage.At(0, 0)[0] == 255, "restore removes the clip");
        canvas.Save();
        canvas.BeginPath();
        canvas.Arc(10, 10, 5, 0, kPi * 2);
        canvas.Clip();
        canvas.Restore();
        Expect(canvas.unsupported() != nullptr, "non-rectangular clip is reported");
    }
    {
        TVideoRasterCanvas::Color color;
        Expect(ParseTVideoColor("rgba(28,44,54,.34)", &color) && color.r == 28 && color.g == 44 && color.b == 54 &&
                   std::fabs(color.a - .34f) < 1e-6, "rgba colour");
        Expect(ParseTVideoColor("#3fbc9d", &color) && color.r == 0x3f && color.g == 0xbc && color.b == 0x9d &&
                   color.a == 1, "hex colour");
        Expect(!ParseTVideoColor("red", &color) && !ParseTVideoColor("#3fbc9", &color) &&
                   !ParseTVideoColor("rgba(1,2,3,2)", &color), "unsupported colours are rejected");
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetFillStyle("#ffffff");
        canvas.SetGlobalAlpha(.5);
        canvas.SetGlobalAlpha(1.4);  // ignored, as canvas ignores it
        canvas.FillRect(0, 0, 1, 1);
        Expect(std::abs(stage.At(0, 0)[0] - 128) <= 1, "globalAlpha outside [0,1] is ignored");
        canvas.SetGlobalAlpha(1);
        canvas.SetFillStyle("rgba(255,255,255,.72)");
        canvas.FillRect(1, 0, 1, 1);
        Expect(std::abs(stage.At(1, 0)[0] - 184) <= 1, "fill colour alpha");
    }
    {
        // Media: an opaque 4x4 white RGBA source placed under translate + scale.
        std::vector<std::uint8_t> white(4 * 4 * 4, 255);
        ComposeSource source;
        source.planes[0] = white.data();
        source.strides[0] = 16;
        source.width = source.height = 4;
        source.rgba = true;
        Stage stage;
        TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
        canvas.SetMedia(TVideoMedia::kRobotWalking, &source);
        canvas.Save();
        canvas.Translate(300, 300);
        canvas.Scale(.6, .6);
        canvas.DrawMedia(TVideoMedia::kRobotWalking, -50, -100, 100, 100);
        canvas.DrawMedia(TVideoMedia::kRobotFlight, -50, -100, 100, 100);  // no source: nothing
        canvas.Restore();
        Expect(Near(stage.Area(), 60 * 60, 1e-6) && stage.At(270, 240)[0] == 255 && stage.At(269, 240)[0] == 0 &&
                   stage.At(300, 300)[0] == 0, "media placed by the transform");
        canvas.Save();
        canvas.Rotate(.1);
        canvas.DrawMedia(TVideoMedia::kRobotWalking, 0, 0, 10, 10);
        canvas.Restore();
        Expect(canvas.unsupported() != nullptr, "rotated media is reported, not approximated");
    }
    {
        Stage stage;
        BoxText text;
        TVideoRasterCanvas canvas(stage.rgb.data(), &text);
        canvas.SetFont("700 18px \"Noto Sans\"");
        Expect(canvas.MeasureText("hay") == 27, "measure uses the current font size");
        canvas.SetTextAlign("center");
        canvas.SetFillStyle("#ffffff");
        canvas.FillText("hay", 67.5, 290);
        Expect(text.origin_x == 67.5 - 13.5 && text.origin_y == 290 && text.scale == 1, "centred text origin");
        Expect(stage.At(54, 289)[0] == 255, "glyph coverage is blended with the fill colour");
        canvas.SetTextAlign("left");
        canvas.Save();
        canvas.Translate(200, 100);
        canvas.Scale(.5, .5);
        canvas.SetFont("700 16px \"Noto Sans\"");
        canvas.FillText("Great job!", 13, 21);
        canvas.Restore();
        Expect(text.origin_x == 206.5 && text.origin_y == 110.5 && text.scale == .5, "scaled chip text origin");
        Expect(canvas.unsupported() == nullptr, "supported text");
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failures of %d checks\n", failures, checks);
        return 1;
    }
    std::printf("PASS tvideo raster canvas: %d checks\n", checks);
    return 0;
}
