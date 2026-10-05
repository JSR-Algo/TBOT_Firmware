#ifndef LESSON_TVIDEO_PAINTER_H
#define LESSON_TVIDEO_PAINTER_H

#include "lesson_tvideo_frame_state.h"

#include <string>

// Paints one canonical tvideoJourney.v1 frame onto a 2D canvas: drawing order,
// colours, paths and prompt fitting mirror the backend's pinned canonical renderer
// (render-entry.mjs). Every placement comes from LayoutTVideoFrame. The
// tvideo-journey-paint-ops.v1 vectors pin the exact call sequence.
namespace tbot {

enum class TVideoMedia : std::uint8_t {
    kBackground, kTeachingObject, kRobotFlight, kRobotWalking, kRobotGreetingTeaching, kRobotCelebration,
};

// The subset of CanvasRenderingContext2D the canonical renderer uses. Style
// strings are the canonical CSS values ("#rrggbb", "rgba(r,g,b,a)",
// '700 16px "Noto Sans"', "left", "center", "alphabetic").
class TVideoCanvas {
public:
    virtual ~TVideoCanvas() = default;
    virtual void SetFillStyle(const char* style) = 0;
    virtual void SetStrokeStyle(const char* style) = 0;
    virtual void SetGlobalAlpha(double alpha) = 0;
    virtual void SetLineWidth(double width) = 0;
    virtual void SetFont(const char* font) = 0;
    virtual void SetTextAlign(const char* align) = 0;
    virtual void SetTextBaseline(const char* baseline) = 0;
    virtual void Save() = 0;
    virtual void Restore() = 0;
    virtual void Translate(double x, double y) = 0;
    virtual void Scale(double x, double y) = 0;
    virtual void Rotate(double radians) = 0;
    virtual void DrawMedia(TVideoMedia media, double x, double y, double width, double height) = 0;
    virtual void BeginPath() = 0;
    virtual void ClosePath() = 0;
    virtual void Clip() = 0;
    virtual void Rect(double x, double y, double width, double height) = 0;
    virtual void MoveTo(double x, double y) = 0;
    virtual void LineTo(double x, double y) = 0;
    virtual void FillRect(double x, double y, double width, double height) = 0;
    virtual void FillText(const std::string& text, double x, double y) = 0;
    virtual void Arc(double x, double y, double radius, double start, double end) = 0;
    virtual void Ellipse(double x, double y, double radius_x, double radius_y, double rotation, double start,
                         double end) = 0;
    virtual void RoundRect(double x, double y, double width, double height, double radius) = 0;
    virtual void Fill() = 0;
    virtual void Stroke() = 0;
    virtual double MeasureText(const std::string& text) = 0;
};

// Card text of the active cue: the owning step's target word and prompt plus the
// fixed coaching labels.
struct TVideoCopy {
    std::string label, prompt, correct, retry;
};

inline constexpr char kTVideoCorrectLabel[] = "Great job!";
inline constexpr char kTVideoRetryLabel[] = "Try again.";

// Returns nullptr, or "prompt does not fit" when no approved font size fits the
// card (the canonical renderer refuses such a frame).
const char* PaintTVideoFrame(TVideoCanvas* canvas, const TVideoFrameState& state, const TVideoFrameLayout& layout,
                             const TVideoCopy& copy);

}  // namespace tbot

#endif
