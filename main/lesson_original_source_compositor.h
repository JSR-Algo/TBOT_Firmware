#ifndef LESSON_ORIGINAL_SOURCE_COMPOSITOR_H
#define LESSON_ORIGINAL_SOURCE_COMPOSITOR_H

#include "lesson_tvideo_frame_state.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Renderer-v6 media compositor: places decoded original frames on the 480x320
// stage exactly where the canonical layout says, as the canonical canvas renderer
// does (bilinear sampling, straight alpha, globalAlpha, source-over). Fixed shapes
// and text are drawn separately by the display layer.
namespace tbot {

inline constexpr int kTVideoStageWidth = 480;
inline constexpr int kTVideoStageHeight = 320;

// A decoded source frame. YUV 4:2:0 frames (optionally with a full-resolution
// alpha plane) use planes[0..2] (+[3]); RGBA frames use planes[0] only.
struct ComposeSource {
    const std::uint8_t* planes[4]{};
    int strides[4]{};
    int width = 0, height = 0;
    bool rgba = false;
    bool alpha = false;  // YUV: planes[3] carries alpha
};

// drawImage(source, x, y, w, h) with globalAlpha, source-over onto the opaque
// 480x320 RGB stage. Width and height must be positive.
void DrawComposeSource(const ComposeSource& source, double x, double y, double w, double h, double opacity,
                       std::uint8_t* rgb /* 480 * 320 * 3 */);

// Unspecified colour metadata (all current originals) is decoded as BT.601 limited
// range, matching what browsers apply to the same streams.
void ComposeTVideoMediaLayers(const TVideoFrameLayout& layout, const ComposeSource& background,
                              const ComposeSource& teaching_object, const ComposeSource& robot,
                              std::uint8_t* rgb /* 480 * 320 * 3 */);

// Index of the frame a browser shows at `seconds`: the last frame whose
// presentation time is not after it (frames sorted by pts).
std::size_t SelectFrameAtSeconds(const std::vector<std::int64_t>& pts, int time_base_num, int time_base_den,
                                 double seconds);

// Canonical seek target: cue-local media time clamped to [0, duration - 1 ms].
double CanonicalMediaSeconds(double media_time_ms, double duration_seconds);

void ConvertRgb888ToRgb565(const std::uint8_t* rgb, std::uint16_t* out, std::size_t pixels);

}  // namespace tbot

#endif
