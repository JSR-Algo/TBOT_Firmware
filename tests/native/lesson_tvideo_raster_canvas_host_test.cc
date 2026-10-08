// Geometry of the renderer-v6 software canvas: antialiased coverage integrates to
// the exact shape areas, strokes are rings of the line width, and clipping, alpha,
// colour parsing, media placement and text placement follow canvas semantics.
#include "lesson_tvideo_raster_canvas.h"
#include "lesson_original_source_compositor.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
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


namespace {
// The fill accumulation before BE08 R21, verbatim apart from its inputs: per pixel, the
// ordered float sum over 16 sub-scanlines. The canvas must match it byte for byte.
struct RefPoint { double x, y; };
struct RefEdge { double x0, y0, x1, y1; int direction; };
struct RefClip { double x0, y0, x1, y1; };
double RefOverlap(double a0, double a1, double b0, double b1) { return std::max(0.0, std::min(a1, b1) - std::max(a0, b0)); }
void ReferenceFill(const std::vector<std::vector<RefPoint>>& polygons, const TVideoRasterCanvas::Color& color,
                   float layer_alpha_in, const RefClip& clip, std::uint8_t* rgb) {
    using Edge = RefEdge;
    constexpr int kSubScanlines = 16;
    const auto Overlap = RefOverlap;

    std::vector<Edge> edges;
    double min_x = 1e30, max_x = -1e30, min_y = 1e30, max_y = -1e30;
    for (const auto& polygon : polygons) {
        for (std::size_t index = 0; index < polygon.size(); ++index) {
            const RefPoint a = polygon[index], b = polygon[(index + 1) % polygon.size()];
            if (!std::isfinite(a.x) || !std::isfinite(a.y)) return;
            min_x = std::min(min_x, a.x);
            max_x = std::max(max_x, a.x);
            min_y = std::min(min_y, a.y);
            max_y = std::max(max_y, a.y);
            if (a.y == b.y) continue;
            edges.push_back(a.y < b.y ? RefEdge{a.x, a.y, b.x, b.y, 1} : RefEdge{b.x, b.y, a.x, a.y, -1});
        }
    }
    if (edges.empty()) return;
    
    const int x0 = std::max(0, static_cast<int>(std::floor(std::max(min_x, clip.x0))));
    const int x1 = std::min(kTVideoStageWidth, static_cast<int>(std::ceil(std::min(max_x, clip.x1))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::max(min_y, clip.y0))));
    const int y1 = std::min(kTVideoStageHeight, static_cast<int>(std::ceil(std::min(max_y, clip.y1))));
    if (x0 >= x1 || y0 >= y1) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    std::vector<float> coverage(static_cast<std::size_t>(x1 - x0));
    std::vector<const Edge*> active;
    std::vector<std::pair<double, int>> crossings;
    std::size_t next = 0;
    const float full_sub_coverage = static_cast<float>(1.0 / kSubScanlines);
    // Clip overlap per column, once per fill instead of per pixel (same values).
    std::vector<double> column_clip(static_cast<std::size_t>(x1 - x0));
    for (int px = x0; px < x1; ++px) column_clip[px - x0] = Overlap(px, px + 1, clip.x0, clip.x1);
    const float layer_alpha = layer_alpha_in;
    for (int row = y0; row < y1; ++row) {
        std::fill(coverage.begin(), coverage.end(), 0.0f);
        bool any = false;
        for (int sub = 0; sub < kSubScanlines; ++sub) {
            const double sample_y = row + (sub + 0.5) / kSubScanlines;
            while (next < edges.size() && edges[next].y0 <= sample_y) active.push_back(&edges[next++]);
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&](const Edge* edge) { return edge->y1 <= sample_y; }),
                         active.end());
            crossings.clear();
            for (const Edge* edge : active) {
                if (edge->y0 > sample_y) continue;
                const double t = (sample_y - edge->y0) / (edge->y1 - edge->y0);
                crossings.push_back({edge->x0 + t * (edge->x1 - edge->x0), edge->direction});
            }
            std::sort(crossings.begin(), crossings.end());
            int winding = 0;
            double span_start = 0;
            for (const auto& crossing : crossings) {
                const int before = winding;
                winding += crossing.second;
                if (before == 0 && winding != 0) {
                    span_start = crossing.first;
                } else if (before != 0 && winding == 0) {
                    const double a = std::max(span_start, static_cast<double>(x0));
                    const double b = std::min(crossing.first, static_cast<double>(x1));
                    // Integer bounds, once per span (the ESP32-S3 emulates double math):
                    // px < b <=> px < ceil(b); pixels in [ceil(a), floor(b)) overlap the
                    // span by exactly 1.0, as Overlap() would return.
                    const int end = std::min(x1, static_cast<int>(std::ceil(b)));
                    const int full_begin = static_cast<int>(std::ceil(a));
                    const int full_end = static_cast<int>(std::floor(b));
                    for (int px = static_cast<int>(std::floor(a)); px < end; ++px) {
                        if (px >= full_begin && px < full_end) {
                            coverage[px - x0] += full_sub_coverage;
                            any = true;
                            continue;
                        }
                        const double covered = Overlap(a, b, px, px + 1);
                        if (covered > 0) {
                            coverage[px - x0] += static_cast<float>(covered / kSubScanlines);
                            any = true;
                        }
                    }
                }
            }
        }
        if (!any) continue;
        const double row_clip = Overlap(row, row + 1, clip.y0, clip.y1);
        std::uint8_t* pixels = rgb + row * kTVideoStageWidth * 3;
        for (int px = x0; px < x1; ++px) {
            float covered = coverage[px - x0];
            if (covered <= 0) continue;
            // Same arithmetic as Blend(): a clip overlap of exactly 1.0 x 1.0 leaves
            // the coverage unchanged, so only clip-edge pixels multiply in double.
            const double clip_overlap = column_clip[px - x0];
            if (!(clip_overlap == 1.0 && row_clip == 1.0)) covered *= static_cast<float>(clip_overlap * row_clip);
            const float alpha = std::min(1.0f, covered) * color.a * layer_alpha;
            if (alpha <= 0) continue;
            std::uint8_t* pixel = pixels + px * 3;
            pixel[0] = static_cast<std::uint8_t>(RoundPixel(color.r * alpha + pixel[0] * (1 - alpha)));
            pixel[1] = static_cast<std::uint8_t>(RoundPixel(color.g * alpha + pixel[1] * (1 - alpha)));
            pixel[2] = static_cast<std::uint8_t>(RoundPixel(color.b * alpha + pixel[2] * (1 - alpha)));
        }
    }
}

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
    // Randomized fills (fractional, off-stage and self-intersecting polygons, translucent
    // colours, layer alpha, rectangular clips) match the pre-R21 accumulation byte for byte.
    {
        std::uint32_t seed = 0x2121;
        const auto next = [&]() { return seed = seed * 1664525u + 1013904223u; };
        const auto uniform = [&](double lo, double hi) { return lo + (hi - lo) * ((next() >> 8) / 16777216.0); };
        int identical = 0;
        for (int round = 0; round < 400; ++round) {
            std::vector<std::uint8_t> base(480 * 320 * 3);
            for (auto& byte : base) byte = static_cast<std::uint8_t>(next() >> 24);
            Stage stage;
            stage.rgb = base;
            std::vector<std::uint8_t> expected = base;
            TVideoRasterCanvas canvas(stage.rgb.data(), nullptr);
            char style[64];
            std::snprintf(style, sizeof style, "rgba(%u,%u,%u,%.3f)", next() >> 24, next() >> 24, next() >> 24,
                          uniform(0.05, 1));
            TVideoRasterCanvas::Color color{};
            ParseTVideoColor(style, &color);
            const double alpha = round % 3 == 0 ? 1.0 : uniform(0.1, 1);
            RefClip clip{0, 0, 480, 320};
            if (round % 4 == 1) {
                const double cx = uniform(-40, 400), cy = uniform(-40, 260), cw = uniform(10, 300), ch = uniform(10, 200);
                canvas.BeginPath();
                canvas.Rect(cx, cy, cw, ch);
                canvas.Clip();
                clip = {std::max(0.0, cx), std::max(0.0, cy), std::min(480.0, cx + cw), std::min(320.0, cy + ch)};
            }
            canvas.SetFillStyle(style);
            canvas.SetGlobalAlpha(alpha);
            std::vector<std::vector<RefPoint>> polygons(1 + next() % 3);
            canvas.BeginPath();
            for (auto& polygon : polygons) {
                const int count = 3 + static_cast<int>(next() % 7);
                const bool large = next() % 3 == 0;
                const double cx = uniform(-20, 500), cy = uniform(-20, 340), r = large ? uniform(50, 400) : uniform(0.3, 60);
                for (int index = 0; index < count; ++index) {
                    // Integer, half-pixel and arbitrary coordinates, as canvas ops produce.
                    double x = cx + uniform(-r, r), y = cy + uniform(-r, r);
                    if (next() % 4 == 0) x = std::round(x);
                    if (next() % 4 == 0) y = std::round(y * 2) / 2;
                    polygon.push_back({x, y});
                    if (index == 0) canvas.MoveTo(x, y); else canvas.LineTo(x, y);
                }
            }
            canvas.Fill();
            ReferenceFill(polygons, color, static_cast<float>(alpha), clip, expected.data());
            identical += stage.rgb == expected;
            Expect(stage.rgb == expected, "randomized fill " + std::to_string(round) + " matches the reference");
        }
        Expect(identical == 400, "400/400 randomized fills identical");
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failures of %d checks\n", failures, checks);
        return 1;
    }
    std::printf("PASS tvideo raster canvas: %d checks\n", checks);
    return 0;
}
