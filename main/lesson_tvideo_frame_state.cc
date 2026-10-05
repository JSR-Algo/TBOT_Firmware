#include "lesson_tvideo_frame_state.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Values must match the JS evaluator bit for bit before rounding; a fused
// multiply-add would round differently from `a + b * c` in JS. Builds pass
// -ffp-contract=off for this file (GCC ignores the standard pragma).
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

namespace tbot {
namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI
constexpr double kFlightMs = 3200, kLandingMs = 500, kFarBeatMs = 800;
constexpr double kCelebrationJumpMs = 1500, kObjectRevealMs = 800, kPuffMs = 800;
constexpr double kCardRevealDelayMs = 350, kCardRevealMs = 500, kCardInitialTranslateY = 14;
constexpr double kCorrectChipMs = 600, kChipScale[3] = {0.6, 1.1, 1};
constexpr double kWordOutMs = 400, kWordInMs = 550;
constexpr int kConfettiColors = 6;

struct EffectInfo { const char* name; std::uint32_t duration_ms; };
constexpr EffectInfo kEffects[] = {
    {"opening", 9500}, {"greet", 1200}, {"teach", 2600}, {"listen", 1300}, {"thinking", 1300},
    {"correct", 600}, {"retry-level-1", 1200}, {"retry-level-2", 1400}, {"retry-level-3", 1600},
    {"celebrate", 3000}, {"word-transition", 1100},
};

double Clamp(double value, double minimum = 0, double maximum = 1) {
    return std::fmin(maximum, std::fmax(minimum, value));
}

double Lerp(double start, double end, double amount) { return start + (end - start) * amount; }

// Math.round: nearest integer, ties toward +infinity.
double JsRound(double value) {
    const double floor = std::floor(value);
    return value - floor >= 0.5 ? floor + 1 : floor;
}

TVideoPose PoseBetween(const TVideoPose& start, const TVideoPose& end, double amount) {
    return {RoundTVideo6(Lerp(start.x, end.x, amount)), RoundTVideo6(Lerp(start.y, end.y, amount)),
            RoundTVideo6(Lerp(start.scale, end.scale, amount))};
}

TVideoPose EvaluateWalk(double time_ms, const TVideoScenePath& scene) {
    const auto& frames = scene.walk;
    int next_index = -1;  // Array.prototype.findIndex
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (frames[index].time_ms >= time_ms) { next_index = static_cast<int>(index); break; }
    }
    if (next_index <= 0) return frames[0].pose;
    const auto& next = frames[next_index];
    const auto& previous = frames[next_index - 1];
    return PoseBetween(previous.pose, next.pose, (time_ms - previous.time_ms) / (next.time_ms - previous.time_ms));
}

bool IsCelebration(TVideoEffect effect) {
    return effect == TVideoEffect::kCorrect || effect == TVideoEffect::kCelebrate;
}

void NearRobot(const TVideoScenePath& scene, TVideoClipRole role, TVideoFrameState* out) {
    auto& robot = out->robot;
    robot.x = scene.teaching_x;
    robot.y = scene.teaching_y;
    robot.scale_x = 0.9;
    robot.scale_y = 0.9;
    robot.opacity = 1;
    robot.clip_role = role;
    robot.shadow_opacity = role == TVideoClipRole::kCelebration ? 0 : 1;
    robot.shadow_scale_x = 1;
    robot.puff_active = false;
    robot.puff_opacity = 0;
    robot.puff_scale = 1;
}

void EvaluateRobot(TVideoEffect effect, double time_ms, const TVideoScenePath& scene, TVideoFrameState* out) {
    auto& robot = out->robot;
    if (effect != TVideoEffect::kOpening) {
        NearRobot(scene, IsCelebration(effect) ? TVideoClipRole::kCelebration : TVideoClipRole::kGreetingTeaching, out);
        if (effect == TVideoEffect::kCelebrate) {
            const double cycle_ms = std::fmod(time_ms, kCelebrationJumpMs);
            if (cycle_ms >= 600 && cycle_ms <= 700) {
                robot.y = RoundTVideo6(scene.teaching_y - 0.28);
                robot.scale_x = 0.9;
                robot.scale_y = 0.99;
            } else if (cycle_ms >= 200 && cycle_ms < 600) {
                robot.y = RoundTVideo6(scene.teaching_y - 0.1);
                robot.scale_x = 0.94;
                robot.scale_y = 0.84;
            } else if (cycle_ms > 700 && cycle_ms < 1100) {
                robot.scale_x = 0.96;
                robot.scale_y = 0.83;
            }
        }
        return;
    }
    if (time_ms < kFlightMs) {
        const double progress = time_ms / kFlightMs;
        const TVideoPose pose = progress <= 0.5
            ? PoseBetween(scene.flight_start, scene.flight_mid, progress * 2)
            : PoseBetween(scene.flight_mid, scene.flight_end, (progress - 0.5) * 2);
        robot.x = pose.x;
        robot.y = pose.y;
        robot.scale_x = pose.scale;
        robot.scale_y = pose.scale;
        robot.opacity = time_ms == 0 ? 0 : 1;
        robot.clip_role = TVideoClipRole::kFlight;
        robot.shadow_opacity = 0;
        robot.shadow_scale_x = 0.5;
        robot.puff_active = false;
        robot.puff_opacity = 0;
        robot.puff_scale = 0.4;
        return;
    }
    const TVideoPose& landing = scene.landing;
    if (time_ms < kFlightMs + kLandingMs + kFarBeatMs) {
        const double landing_time = time_ms - kFlightMs;
        const bool squash = landing_time <= kLandingMs && landing_time >= 100 && landing_time <= 300;
        const double puff_progress = Clamp(landing_time / kPuffMs);
        robot.x = landing.x;
        robot.y = landing.y;
        robot.scale_x = RoundTVideo6(landing.scale * (squash ? 1.04 : 1));
        robot.scale_y = RoundTVideo6(landing.scale * (squash ? 0.94 : 1));
        robot.opacity = 1;
        robot.clip_role = TVideoClipRole::kFlight;
        robot.shadow_opacity = 1;
        robot.shadow_scale_x = RoundTVideo6(1 - 0.1 * std::sin(kPi * Clamp(landing_time / 3200)));
        robot.puff_active = landing_time < 800;
        robot.puff_opacity = RoundTVideo6(std::sin(kPi * puff_progress));
        robot.puff_scale = RoundTVideo6(0.4 + 0.6 * puff_progress);
        return;
    }
    const TVideoPose pose = EvaluateWalk(time_ms - kFlightMs - kLandingMs - kFarBeatMs, scene);
    robot.x = pose.x;
    robot.y = pose.y;
    robot.scale_x = pose.scale;
    robot.scale_y = pose.scale;
    robot.opacity = 1;
    robot.clip_role = TVideoClipRole::kWalking;
    robot.shadow_opacity = 1;
    robot.shadow_scale_x = 1;
    robot.puff_active = false;
    robot.puff_opacity = 0;
    robot.puff_scale = 1;
}

void EvaluateObject(TVideoEffect effect, double time_ms, const TVideoScenePath& scene, TVideoFrameState* out) {
    auto& object = out->object;
    double opacity = effect == TVideoEffect::kOpening || IsCelebration(effect) ? 0 : 1;
    double translate_x = 0, scale = 1;
    if (effect == TVideoEffect::kGreet) opacity = RoundTVideo6(Clamp(time_ms / kObjectRevealMs));
    if (effect == TVideoEffect::kWordTransition) {
        if (time_ms <= kWordOutMs) {
            const double progress = time_ms / kWordOutMs;
            opacity = RoundTVideo6(1 - progress);
            translate_x = RoundTVideo6(-28 * progress);
            scale = RoundTVideo6(1 - 0.12 * progress);
        } else {
            const double progress = Clamp((time_ms - kWordOutMs) / kWordInMs);
            opacity = RoundTVideo6(progress);
            translate_x = RoundTVideo6(30 * (1 - progress));
            scale = RoundTVideo6(0.88 + 0.12 * progress);
        }
    }
    const double bob_progress = std::fmod(time_ms, 2600) / 2600;
    const double bob = bob_progress <= 0.5 ? Lerp(4, -6, bob_progress * 2) : Lerp(-6, 4, (bob_progress - 0.5) * 2);
    object.x = scene.object_x;
    object.y = scene.object_y;
    object.translate_x = translate_x;
    object.scale = scale;
    object.opacity = opacity;
    object.bob_offset_y = RoundTVideo6(bob);
    object.word_pill_visible = opacity > 0;
}

int RetryLevel(TVideoEffect effect) {
    if (effect == TVideoEffect::kRetryLevel1) return 1;
    if (effect == TVideoEffect::kRetryLevel2) return 2;
    if (effect == TVideoEffect::kRetryLevel3) return 3;
    return 0;
}

void EvaluateCard(TVideoEffect effect, double time_ms, const TVideoFrameInput& input, TVideoFrameState* out) {
    auto& card = out->card;
    const int retry = RetryLevel(effect);
    const bool correct = IsCelebration(effect);
    card.listening_glow = effect == TVideoEffect::kListen ? RoundTVideo6(std::sin(kPi * time_ms / 1300)) : 0;
    card.gentle_pulse = effect == TVideoEffect::kThinking ? RoundTVideo6(std::sin(kPi * time_ms / 1300)) : 0;
    card.cue = effect == TVideoEffect::kListen ? TVideoCardCue::kListening
        : effect == TVideoEffect::kThinking ? TVideoCardCue::kThinking
        : retry > 0 ? TVideoCardCue::kRetry
        : effect == TVideoEffect::kWordTransition ? TVideoCardCue::kWordTransition
        : correct ? TVideoCardCue::kCorrect : TVideoCardCue::kDefault;
    const double reveal_progress = effect == TVideoEffect::kGreet
        ? Clamp((time_ms - kCardRevealDelayMs) / kCardRevealMs) : 1;
    const double chip_progress = Clamp(time_ms / kCorrectChipMs);
    const double chip_scale = chip_progress <= 0.6
        ? Lerp(kChipScale[0], kChipScale[1], chip_progress / 0.6)
        : Lerp(kChipScale[1], kChipScale[2], (chip_progress - 0.6) / 0.4);
    static constexpr double kAmplitude[4] = {0, 2, 4, 6};
    static constexpr double kPulses[4] = {0, 1, 2, 3};
    const double retry_duration = retry > 0 ? TVideoEffectDurationMs(effect) : 1;
    card.visible = effect != TVideoEffect::kOpening && !correct;
    card.opacity = RoundTVideo6(reveal_progress);
    card.translate_y = RoundTVideo6(kCardInitialTranslateY * (1 - reveal_progress));
    card.active_dots = input.progress_index;
    card.total_dots = input.progress_count;
    card.correct_chip_visible = correct;
    card.correct_chip_opacity = correct
        ? RoundTVideo6(chip_progress == 0 ? 0 : std::fmin(1, chip_progress / 0.6)) : 0;
    card.correct_chip_scale = correct ? RoundTVideo6(chip_scale) : 1;
    card.retry_level = retry;
    card.retry_offset_x = retry > 0
        ? RoundTVideo6(std::sin((kPi * 2 * kPulses[retry] * time_ms) / retry_duration) * kAmplitude[retry]) : 0;
}

void EvaluateConfetti(TVideoEffect effect, double time_ms, TVideoFrameState* out) {
    out->has_confetti = IsCelebration(effect);
    if (!out->has_confetti) return;
    for (int index = 0; index < kTVideoConfettiPieces; ++index) {
        const double angle = (static_cast<double>(index) / kTVideoConfettiPieces) * kPi * 2;
        const double spread = 130 + ((index * 53) % 240);
        const double x = JsRound(std::cos(angle) * spread) + (((index * 37) % 80) - 40);
        const double peak_y = -(150 + ((index * 29) % 170));
        const double fall_y = 190 + ((index * 41) % 170);
        const double delay_ms = (index * 17) % 260;
        const double duration_ms = 1700 + ((index * 7) % 9) * 100;
        const double progress = Clamp((time_ms - delay_ms) / duration_ms);
        const double translate_y = progress < 0.38
            ? Lerp(0, peak_y, progress / 0.38) : Lerp(peak_y, fall_y, (progress - 0.38) / 0.62);
        auto& piece = out->confetti[index];
        piece.index = index;
        piece.color_index = index % kConfettiColors;
        piece.size_px = 10 + ((index * 13) % 9);
        piece.delay_ms = delay_ms;
        piece.duration_ms = duration_ms;
        piece.x = x;
        piece.peak_y = peak_y;
        piece.fall_y = fall_y;
        piece.rotation = ((index * 67) % 720) - 360;
        piece.opacity = progress == 0 || progress == 1 ? 0 : progress < 0.08 ? RoundTVideo6(progress / 0.08) : 1;
        piece.translate_x = RoundTVideo6(
            x * (progress < 0.38 ? 0.62 * progress / 0.38 : 0.62 + 0.38 * ((progress - 0.38) / 0.62)));
        piece.translate_y = RoundTVideo6(translate_y);
    }
}

const cJSON* Get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

bool Number(const cJSON* value, double* out) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble)) return false;
    *out = value->valuedouble;
    return true;
}

bool Pose(const cJSON* value, TVideoPose* out, bool with_scale = true) {
    return cJSON_IsObject(value) && Number(Get(value, "x"), &out->x) && Number(Get(value, "y"), &out->y) &&
           (!with_scale || Number(Get(value, "scale"), &out->scale));
}

}  // namespace

bool ParseTVideoEffect(const char* value, TVideoEffect* out) {
    if (value == nullptr) return false;
    for (std::size_t index = 0; index < sizeof(kEffects) / sizeof(kEffects[0]); ++index) {
        if (std::strcmp(value, kEffects[index].name) == 0) {
            *out = static_cast<TVideoEffect>(index);
            return true;
        }
    }
    return false;
}

std::uint32_t TVideoEffectDurationMs(TVideoEffect effect) {
    return kEffects[static_cast<int>(effect)].duration_ms;
}

const char* ParseTVideoScenePath(const cJSON* journey, TVideoScenePath* out) {
    const cJSON* scene = Get(journey, "scenePath");
    const cJSON* flight = Get(scene, "flightIngress");
    const cJSON* walk = Get(Get(scene, "walk"), "keyframes");
    TVideoScenePath path;
    if (!Pose(Get(flight, "start"), &path.flight_start) || !Pose(Get(flight, "mid"), &path.flight_mid) ||
        !Pose(Get(flight, "end"), &path.flight_end) || !Pose(Get(scene, "landing"), &path.landing))
        return "flight or landing pose";
    if (!cJSON_IsArray(walk) || cJSON_GetArraySize(walk) < 2) return "walk keyframes";
    for (const cJSON* frame = walk->child; frame != nullptr; frame = frame->next) {
        TVideoWalkKeyframe keyframe;
        if (!Number(Get(frame, "timeMs"), &keyframe.time_ms) || !Pose(frame, &keyframe.pose)) return "walk keyframe";
        path.walk.push_back(keyframe);
    }
    TVideoPose teaching, object;
    if (!Pose(Get(scene, "teachingAnchor"), &teaching, false) || !Pose(Get(scene, "objectAnchor"), &object, false))
        return "anchors";
    path.teaching_x = teaching.x;
    path.teaching_y = teaching.y;
    path.object_x = object.x;
    path.object_y = object.y;
    *out = std::move(path);
    return nullptr;
}

double RoundTVideo6(double value) {
    if (!std::isfinite(value)) return value;
    const bool negative = std::signbit(value);
    const double magnitude = std::fabs(value);
    // Below half of 1e-6 every value rounds to zero (an exact 5e-7 tie is not a double).
    if (magnitude < 4e-7) return negative ? -0.0 : 0.0;
    if (magnitude >= 1e12) return value;  // never produced by the evaluator
    // Exact decimal expansion: |x| >= 4e-7 needs at most ~75 fractional digits.
    char digits[128];
    std::snprintf(digits, sizeof digits, "%.90f", magnitude);
    const char* point = std::strchr(digits, '.');
    long long scaled = std::strtoll(digits, nullptr, 10);
    for (int index = 1; index <= 6; ++index) scaled = scaled * 10 + (point[index] - '0');
    if (point[7] >= '5') ++scaled;  // ties go to the larger magnitude, as in toFixed
    const double rounded = static_cast<double>(scaled) / 1000000.0;
    return negative ? -rounded : rounded;
}

void LayoutTVideoFrame(const TVideoFrameState& state, TVideoFrameLayout* out) {
    // Same constants and evaluation order as render-entry.mjs / tvideo-journey.layout.ts.
    constexpr double kWidth = 480, kHeight = 320, kObjectSize = 95;
    constexpr double kObjectOffsetX = 67.5 - .3 * kWidth, kObjectOffsetY = 215.5 - .66 * kHeight;
    constexpr double kRobotBaseSize = 150 / .9;
    constexpr double kRobotOffsetX = 193 - .465 * kWidth, kRobotOffsetY = 310 - .685 * kHeight;
    static constexpr double kPuff[5][2] = {{-34, -26}, {28, -34}, {-16, -46}, {40, -14}, {6, -54}};
    const auto unit = [](double value) { return std::fmin(1, std::fmax(0, std::isfinite(value) ? value : 0)); };
    const auto or_zero = [](double value) { return value == 0 || std::isnan(value) ? 0.0 : value; };
    TVideoFrameLayout layout;
    layout.background_media_time_ms = state.time_ms;
    const double object_x = state.object.x * kWidth + state.object.translate_x + kObjectOffsetX;
    const double object_y = state.object.y * kHeight + state.object.bob_offset_y + kObjectOffsetY;
    const double size = kObjectSize * state.object.scale;
    layout.object = {object_x - size / 2, object_y - size / 2, size, state.object.opacity, object_x, object_y,
                     state.time_ms};
    layout.word_pill = {state.object.word_pill_visible, object_x, object_y + 49.5, 28, state.object.opacity};
    const double robot_x = state.robot.x * kWidth + kRobotOffsetX;
    const double robot_y = state.robot.y * kHeight + kRobotOffsetY;
    layout.robot.anchor_x = robot_x;
    layout.robot.anchor_y = robot_y;
    layout.robot.scale_x = state.robot.scale_x;
    layout.robot.scale_y = state.robot.scale_y;
    layout.robot.base_size = kRobotBaseSize;
    layout.robot.opacity = state.robot.opacity;
    layout.robot.clip_role = state.robot.clip_role;
    layout.robot.media_time_ms = state.time_ms;
    layout.has_shadow = state.robot.shadow_opacity > 0;
    if (layout.has_shadow) {
        layout.shadow = {robot_x, robot_y + 5, 35 * state.robot.shadow_scale_x, 7, state.robot.shadow_opacity};
    }
    if (state.robot.puff_active && state.robot.puff_opacity > 0) {
        layout.puff_count = 5;
        for (int index = 0; index < 5; ++index) {
            auto& particle = layout.puff[index];
            particle.center_x = robot_x + kPuff[index][0] * state.robot.puff_scale;
            particle.center_y = robot_y + kPuff[index][1] * state.robot.puff_scale;
            particle.radius = 4.5 * state.robot.puff_scale;
            particle.opacity = state.robot.puff_opacity;
            particle.color_index = index;
        }
    }
    for (int index = 0; index < state.card.total_dots; ++index) {
        TVideoFrameLayout::Circle dot;
        dot.center_x = 33 + index * 24;
        dot.center_y = 18.5;
        dot.radius = 7.5;
        dot.active = index < state.card.active_dots;
        layout.progress_dots.push_back(dot);
    }
    layout.has_card = state.card.visible;
    if (layout.has_card) {
        const auto& card = state.card;
        layout.card.offset_x = or_zero(card.retry_offset_x);
        layout.card.offset_y = or_zero(card.translate_y);
        layout.card.opacity = card.opacity;
        layout.card.x = 25;
        layout.card.y = 35;
        layout.card.width = 190;
        layout.card.height = 92;
        layout.card.radius = 18;
        layout.card.cue = card.cue;
        layout.card.ring_line_width = card.cue == TVideoCardCue::kListening ? 1.5 + 5 * unit(card.listening_glow)
            : card.cue == TVideoCardCue::kThinking ? 1.5 + unit(card.gentle_pulse) * 2
            : card.cue == TVideoCardCue::kRetry || card.cue == TVideoCardCue::kWordTransition ? 2 : 1.5;
    }
    layout.has_correct_chip = state.card.correct_chip_visible;
    if (layout.has_correct_chip) {
        layout.correct_chip = {25, 90, state.card.correct_chip_scale, state.card.correct_chip_opacity, 29};
    }
    if (state.has_confetti) {
        layout.confetti_count = kTVideoConfettiPieces;
        for (int index = 0; index < kTVideoConfettiPieces; ++index) {
            const auto& piece = state.confetti[index];
            layout.confetti[index] = {223 + piece.translate_x, 172 + piece.translate_y, piece.rotation * kPi / 180,
                                      piece.size_px, piece.opacity, piece.color_index};
        }
    }
    *out = std::move(layout);
}

const char* EvaluateTVideoFrame(const TVideoFrameInput& input, TVideoFrameState* out) {
    if (input.scene == nullptr || input.scene->walk.empty()) return "scene path missing";
    if (!std::isfinite(input.time_ms)) return "timeMs must be finite";
    const double duration = TVideoEffectDurationMs(input.effect);
    const double clamped = Clamp(input.time_ms, 0, duration);
    const double frame_index = std::floor(clamped / 100);
    const double time_ms = frame_index * 100;
    const double object_time_ms = input.effect == TVideoEffect::kWordTransition
        ? std::fmin(clamped, kWordOutMs + kWordInMs) : time_ms;
    TVideoFrameState state;
    state.frame_index = frame_index;
    state.time_ms = time_ms;
    EvaluateRobot(input.effect, time_ms, *input.scene, &state);
    EvaluateObject(input.effect, object_time_ms, *input.scene, &state);
    EvaluateCard(input.effect, time_ms, input, &state);
    EvaluateConfetti(input.effect, time_ms, &state);
    *out = state;
    return nullptr;
}

}  // namespace tbot
