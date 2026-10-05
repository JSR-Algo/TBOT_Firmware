#ifndef LESSON_TVIDEO_RASTER_CANVAS_H
#define LESSON_TVIDEO_RASTER_CANVAS_H

#include "lesson_original_source_compositor.h"
#include "lesson_tvideo_painter.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Software TVideoCanvas onto the opaque 480x320 RGB888 stage. Paths are flattened
// in user space, transformed, and filled with 16 sub-scanlines per row and exact
// horizontal coverage (nonzero winding, antialiased like the canonical canvas).
// Strokes are rings between the two offset outlines (miter joins, butt caps).
// Clipping is limited to axis-aligned rectangles, the only clip the canonical
// renderer uses. Media are decoded original frames placed by DrawComposeSource;
// text is delegated to a TVideoTextRenderer so the device uses its own fonts.
namespace tbot {

// Coverage callback: device pixel (x, y), coverage in [0, 1].
using TVideoCoveragePlot = std::function<void(int x, int y, float coverage)>;

class TVideoTextRenderer {
public:
    virtual ~TVideoTextRenderer() = default;
    // Advance width in CSS pixels of `text` at `font_px` (bold).
    virtual double Measure(const std::string& text, int font_px) = 0;
    // Plots glyph coverage for `text` whose left edge is at device x and baseline
    // at device y, uniformly scaled by `scale` from `font_px`.
    virtual void Render(const std::string& text, int font_px, double scale, double x, double baseline,
                        const TVideoCoveragePlot& plot) = 0;
};

class TVideoRasterCanvas final : public TVideoCanvas {
public:
    // `rgb` is the 480x320x3 stage; `text` may be null (text is then measured as
    // zero width and not drawn).
    TVideoRasterCanvas(std::uint8_t* rgb, TVideoTextRenderer* text);

    // Media for DrawMedia; a missing (null or empty) source draws nothing.
    void SetMedia(TVideoMedia media, const ComposeSource* source);
    // Set when a call needed an unsupported feature (rotated media, non-rectangular
    // clip); the frame should then be treated as failed.
    const char* unsupported() const { return unsupported_; }

    void SetFillStyle(const char* style) override;
    void SetStrokeStyle(const char* style) override;
    void SetGlobalAlpha(double alpha) override;
    void SetLineWidth(double width) override;
    void SetFont(const char* font) override;
    void SetTextAlign(const char* align) override;
    void SetTextBaseline(const char* baseline) override;
    void Save() override;
    void Restore() override;
    void Translate(double x, double y) override;
    void Scale(double x, double y) override;
    void Rotate(double radians) override;
    void DrawMedia(TVideoMedia media, double x, double y, double width, double height) override;
    void BeginPath() override;
    void ClosePath() override;
    void Clip() override;
    void Rect(double x, double y, double width, double height) override;
    void MoveTo(double x, double y) override;
    void LineTo(double x, double y) override;
    void FillRect(double x, double y, double width, double height) override;
    void FillText(const std::string& text, double x, double y) override;
    void Arc(double x, double y, double radius, double start, double end) override;
    void Ellipse(double x, double y, double radius_x, double radius_y, double rotation, double start,
                 double end) override;
    void RoundRect(double x, double y, double width, double height, double radius) override;
    void Fill() override;
    void Stroke() override;
    double MeasureText(const std::string& text) override;

    struct Point { double x, y; };
    struct Color { float r = 0, g = 0, b = 0, a = 1; };
    struct Matrix { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };
    struct ClipRect { double x0, y0, x1, y1; };

private:
    struct SubPath {
        std::vector<Point> points;  // user space
        bool closed = false;
    };
    struct State {
        Matrix matrix;
        Color fill{0, 0, 0, 1}, stroke{0, 0, 0, 1};
        double alpha = 1, line_width = 1;
        int font_px = 10;
        bool align_center = false;
        ClipRect clip{0, 0, 480, 320};
    };

    Point Transform(Point point) const;
    SubPath& Current();
    void AppendArc(double x, double y, double radius_x, double radius_y, double start, double end);
    void FillPolygons(const std::vector<std::vector<Point>>& polygons, const Color& color);
    void Blend(int x, int y, float coverage, const Color& color);

    std::uint8_t* rgb_;
    TVideoTextRenderer* text_;
    std::array<const ComposeSource*, 6> media_{};
    State state_;
    std::vector<State> stack_;
    std::vector<SubPath> path_;
    const char* unsupported_ = nullptr;
};

// Parses "#rrggbb" and "rgba(r,g,b,a)"; false for anything else.
bool ParseTVideoColor(const char* style, TVideoRasterCanvas::Color* out);

}  // namespace tbot

#endif
