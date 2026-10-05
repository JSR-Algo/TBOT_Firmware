#ifndef LESSON_TVIDEO_FRAME_STATE_H
#define LESSON_TVIDEO_FRAME_STATE_H

#include <cJSON.h>

#include <array>
#include <cstdint>
#include <vector>

// Canonical tvideoJourney.v1 timeline evaluator (port of the backend
// evaluateTVideoFrame). The firmware compositor and the admin preview must place
// the same layers at the same logical frame; tvideo-journey-frame-state.v1
// vectors produced by the backend pin every value after the same 6-decimal rounding.
namespace tbot {

enum class TVideoEffect : std::uint8_t {
    kOpening, kGreet, kTeach, kListen, kThinking, kCorrect,
    kRetryLevel1, kRetryLevel2, kRetryLevel3, kCelebrate, kWordTransition,
};
enum class TVideoClipRole : std::uint8_t { kFlight, kWalking, kGreetingTeaching, kCelebration };
enum class TVideoCardCue : std::uint8_t { kDefault, kListening, kThinking, kCorrect, kRetry, kWordTransition };

inline constexpr int kTVideoConfettiPieces = 64;

struct TVideoPose { double x = 0, y = 0, scale = 0; };
struct TVideoWalkKeyframe { double time_ms = 0; TVideoPose pose; };

struct TVideoScenePath {
    TVideoPose flight_start, flight_mid, flight_end, landing;
    std::vector<TVideoWalkKeyframe> walk;
    double teaching_x = 0, teaching_y = 0, object_x = 0, object_y = 0;
};

struct TVideoFrameInput {
    TVideoEffect effect = TVideoEffect::kOpening;
    double time_ms = 0;
    const TVideoScenePath* scene = nullptr;
    int progress_index = 0, progress_count = 0;
};

struct TVideoConfettiPiece {
    int index = 0, color_index = 0;
    double size_px = 0, delay_ms = 0, duration_ms = 0, x = 0, peak_y = 0, fall_y = 0, rotation = 0;
    double opacity = 0, translate_x = 0, translate_y = 0;
};

struct TVideoFrameState {
    double frame_index = 0, time_ms = 0;
    struct {
        double x = 0, y = 0, scale_x = 0, scale_y = 0, opacity = 0;
        TVideoClipRole clip_role = TVideoClipRole::kFlight;
        double shadow_opacity = 0, shadow_scale_x = 0;
        bool puff_active = false;
        double puff_opacity = 0, puff_scale = 0;
    } robot;
    struct {
        double x = 0, y = 0, translate_x = 0, scale = 0, opacity = 0, bob_offset_y = 0;
        bool word_pill_visible = false;
    } object;
    struct {
        bool visible = false;
        double opacity = 0, translate_y = 0;
        int active_dots = 0, total_dots = 0;
        double listening_glow = 0;
        bool correct_chip_visible = false;
        double correct_chip_opacity = 0, correct_chip_scale = 0;
        TVideoCardCue cue = TVideoCardCue::kDefault;
        double gentle_pulse = 0;
        int retry_level = 0;
        double retry_offset_x = 0;
    } card;
    bool has_confetti = false;
    std::array<TVideoConfettiPiece, kTVideoConfettiPieces> confetti{};
};

// Pixel placement on the 480x320 stage exactly as the canonical renderer paints a
// frame (backend layoutTVideoFrame, proven against render-entry.mjs). Text widths
// are font dependent and excluded; the device lays out text with its own fonts.
struct TVideoFrameLayout {
    double background_media_time_ms = 0;
    struct { double x = 0, y = 0, size = 0, opacity = 0, center_x = 0, center_y = 0, media_time_ms = 0; } object;
    struct { bool visible = false; double center_x = 0, top = 0, height = 0, opacity = 0; } word_pill;
    struct {
        double anchor_x = 0, anchor_y = 0, scale_x = 0, scale_y = 0, base_size = 0, opacity = 0;
        TVideoClipRole clip_role = TVideoClipRole::kFlight;
        double media_time_ms = 0;
    } robot;
    bool has_shadow = false;
    struct { double center_x = 0, center_y = 0, radius_x = 0, radius_y = 0, opacity = 0; } shadow;
    struct Circle { double center_x = 0, center_y = 0, radius = 0, opacity = 0; int color_index = 0; bool active = false; };
    int puff_count = 0;
    std::array<Circle, 5> puff{};
    std::vector<Circle> progress_dots;
    bool has_card = false;
    struct {
        double offset_x = 0, offset_y = 0, opacity = 0, x = 0, y = 0, width = 0, height = 0, radius = 0;
        TVideoCardCue cue = TVideoCardCue::kDefault;
        double ring_line_width = 0;
    } card;
    bool has_correct_chip = false;
    struct { double x = 0, y = 0, scale = 0, opacity = 0, height = 0; } correct_chip;
    struct Piece { double center_x = 0, center_y = 0, rotation_rad = 0, size = 0, opacity = 0; int color_index = 0; };
    int confetti_count = 0;
    std::array<Piece, kTVideoConfettiPieces> confetti{};
};

void LayoutTVideoFrame(const TVideoFrameState& state, TVideoFrameLayout* out);

bool ParseTVideoEffect(const char* value, TVideoEffect* out);
std::uint32_t TVideoEffectDurationMs(TVideoEffect effect);
// Reads scenePath from a tvideoJourney.v1 document already admitted by
// ParseOriginalSourceScene. Returns nullptr on success, otherwise a reason.
const char* ParseTVideoScenePath(const cJSON* journey, TVideoScenePath* out);
// JS Number(value.toFixed(6)): nearest 1e-6, exact ties away from zero.
double RoundTVideo6(double value);
const char* EvaluateTVideoFrame(const TVideoFrameInput& input, TVideoFrameState* out);

}  // namespace tbot

#endif
