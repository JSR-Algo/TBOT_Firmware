#include "checked_cjson.h"
#include "lesson_tvideo_frame_state.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace tbot;

namespace {
int failures = 0;
int checks = 0;
const cJSON* Get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

void ExpectNumber(const cJSON* expected, double actual, const std::string& where) {
    ++checks;
    if (!cJSON_IsNumber(expected) || expected->valuedouble != actual) {
        if (failures++ < 20) {
            std::fprintf(stderr, "FAIL %s expected=%.17g actual=%.17g\n", where.c_str(),
                         cJSON_IsNumber(expected) ? expected->valuedouble : NAN, actual);
        }
    }
}

void ExpectBool(const cJSON* expected, bool actual, const std::string& where) {
    ++checks;
    if (!cJSON_IsBool(expected) || cJSON_IsTrue(expected) != actual) {
        if (failures++ < 20) std::fprintf(stderr, "FAIL %s bool\n", where.c_str());
    }
}

void ExpectString(const cJSON* expected, const char* actual, const std::string& where) {
    ++checks;
    if (!cJSON_IsString(expected) || std::strcmp(expected->valuestring, actual) != 0) {
        if (failures++ < 20) std::fprintf(stderr, "FAIL %s string actual=%s\n", where.c_str(), actual);
    }
}

const char* ClipRoleName(TVideoClipRole role) {
    static const char* const kNames[] = {"flight", "walking", "greeting-teaching", "celebration"};
    return kNames[static_cast<int>(role)];
}

const char* CueName(TVideoCardCue cue) {
    static const char* const kNames[] = {"default", "listening", "thinking", "correct", "retry", "word-transition"};
    return kNames[static_cast<int>(cue)];
}

const char* const kColors[] = {"#ffd166", "#ff8a6b", "#79d8bd", "#b39ddb", "#5bb8e6", "#ffffff"};

void CompareState(const cJSON* expected, const TVideoFrameState& actual, const std::string& at) {
    ExpectNumber(Get(expected, "frameIndex"), actual.frame_index, at + ".frameIndex");
    ExpectNumber(Get(expected, "timeMs"), actual.time_ms, at + ".timeMs");
    const cJSON* robot = Get(expected, "robot");
    ExpectNumber(Get(robot, "x"), actual.robot.x, at + ".robot.x");
    ExpectNumber(Get(robot, "y"), actual.robot.y, at + ".robot.y");
    ExpectNumber(Get(robot, "scaleX"), actual.robot.scale_x, at + ".robot.scaleX");
    ExpectNumber(Get(robot, "scaleY"), actual.robot.scale_y, at + ".robot.scaleY");
    ExpectNumber(Get(robot, "opacity"), actual.robot.opacity, at + ".robot.opacity");
    ExpectString(Get(robot, "clipRole"), ClipRoleName(actual.robot.clip_role), at + ".robot.clipRole");
    ExpectNumber(Get(Get(robot, "shadow"), "opacity"), actual.robot.shadow_opacity, at + ".shadow.opacity");
    ExpectNumber(Get(Get(robot, "shadow"), "scaleX"), actual.robot.shadow_scale_x, at + ".shadow.scaleX");
    ExpectBool(Get(Get(robot, "puff"), "active"), actual.robot.puff_active, at + ".puff.active");
    ExpectNumber(Get(Get(robot, "puff"), "opacity"), actual.robot.puff_opacity, at + ".puff.opacity");
    ExpectNumber(Get(Get(robot, "puff"), "scale"), actual.robot.puff_scale, at + ".puff.scale");
    const cJSON* object = Get(expected, "object");
    ExpectNumber(Get(object, "x"), actual.object.x, at + ".object.x");
    ExpectNumber(Get(object, "y"), actual.object.y, at + ".object.y");
    ExpectNumber(Get(object, "translateX"), actual.object.translate_x, at + ".object.translateX");
    ExpectNumber(Get(object, "scale"), actual.object.scale, at + ".object.scale");
    ExpectNumber(Get(object, "opacity"), actual.object.opacity, at + ".object.opacity");
    ExpectNumber(Get(object, "bobOffsetY"), actual.object.bob_offset_y, at + ".object.bobOffsetY");
    ExpectBool(Get(Get(object, "wordPill"), "visible"), actual.object.word_pill_visible, at + ".wordPill");
    const cJSON* card = Get(expected, "card");
    ExpectBool(Get(card, "visible"), actual.card.visible, at + ".card.visible");
    ExpectNumber(Get(card, "opacity"), actual.card.opacity, at + ".card.opacity");
    ExpectNumber(Get(card, "translateY"), actual.card.translate_y, at + ".card.translateY");
    ExpectNumber(Get(Get(card, "progress"), "activeDots"), actual.card.active_dots, at + ".card.activeDots");
    ExpectNumber(Get(Get(card, "progress"), "totalDots"), actual.card.total_dots, at + ".card.totalDots");
    ExpectNumber(Get(card, "listeningGlow"), actual.card.listening_glow, at + ".card.listeningGlow");
    const cJSON* chip = Get(card, "correctChip");
    ExpectBool(Get(chip, "visible"), actual.card.correct_chip_visible, at + ".chip.visible");
    ExpectNumber(Get(chip, "opacity"), actual.card.correct_chip_opacity, at + ".chip.opacity");
    ExpectNumber(Get(chip, "scale"), actual.card.correct_chip_scale, at + ".chip.scale");
    ExpectString(Get(card, "cue"), CueName(actual.card.cue), at + ".card.cue");
    ExpectNumber(Get(card, "gentlePulse"), actual.card.gentle_pulse, at + ".card.gentlePulse");
    ExpectNumber(Get(card, "retryLevel"), actual.card.retry_level, at + ".card.retryLevel");
    ExpectNumber(Get(card, "retryOffsetX"), actual.card.retry_offset_x, at + ".card.retryOffsetX");
    const cJSON* confetti = Get(expected, "confetti");
    ++checks;
    if (cJSON_GetArraySize(confetti) != (actual.has_confetti ? kTVideoConfettiPieces : 0)) {
        if (failures++ < 20) std::fprintf(stderr, "FAIL %s confetti count\n", at.c_str());
        return;
    }
    int index = 0;
    for (const cJSON* piece = confetti->child; piece != nullptr; piece = piece->next, ++index) {
        const auto& p = actual.confetti[index];
        const std::string where = at + ".confetti[" + std::to_string(index) + "]";
        ExpectNumber(Get(piece, "index"), p.index, where + ".index");
        ExpectString(Get(piece, "color"), kColors[p.color_index], where + ".color");
        ExpectNumber(Get(piece, "sizePx"), p.size_px, where + ".sizePx");
        ExpectNumber(Get(piece, "delayMs"), p.delay_ms, where + ".delayMs");
        ExpectNumber(Get(piece, "durationMs"), p.duration_ms, where + ".durationMs");
        ExpectNumber(Get(piece, "x"), p.x, where + ".x");
        ExpectNumber(Get(piece, "peakY"), p.peak_y, where + ".peakY");
        ExpectNumber(Get(piece, "fallY"), p.fall_y, where + ".fallY");
        ExpectNumber(Get(piece, "rotation"), p.rotation, where + ".rotation");
        ExpectNumber(Get(piece, "opacity"), p.opacity, where + ".opacity");
        ExpectNumber(Get(piece, "translateX"), p.translate_x, where + ".translateX");
        ExpectNumber(Get(piece, "translateY"), p.translate_y, where + ".translateY");
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    CheckedCJsonPtr root(cJSON_Parse(text.str().c_str()));
    if (!root) return 3;
    for (const cJSON* item = Get(root.get(), "roundingCases")->child; item != nullptr; item = item->next) {
        ExpectNumber(Get(item, "output"), RoundTVideo6(Get(item, "input")->valuedouble),
                     "round(" + std::to_string(Get(item, "input")->valuedouble) + ")");
    }
    TVideoScenePath scene;
    if (const char* error = ParseTVideoScenePath(Get(root.get(), "journey"), &scene)) {
        std::fprintf(stderr, "FAIL scenePath: %s\n", error);
        return 1;
    }
    int frames = 0;
    for (const cJSON* cue = Get(root.get(), "cues")->child; cue != nullptr; cue = cue->next) {
        TVideoFrameInput input;
        input.scene = &scene;
        if (!ParseTVideoEffect(Get(cue, "effect")->valuestring, &input.effect)) return 4;
        ExpectNumber(Get(cue, "durationMs"), TVideoEffectDurationMs(input.effect), "duration");
        input.progress_index = Get(Get(cue, "progress"), "index")->valueint;
        input.progress_count = Get(Get(cue, "progress"), "count")->valueint;
        for (const cJSON* frame = Get(cue, "frames")->child; frame != nullptr; frame = frame->next, ++frames) {
            input.time_ms = Get(frame, "timeMs")->valuedouble;
            TVideoFrameState state;
            if (EvaluateTVideoFrame(input, &state) != nullptr) return 5;
            CompareState(Get(frame, "state"), state,
                         std::string(Get(cue, "cueId")->valuestring) + "@" + std::to_string(input.time_ms));
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("PASS tvideo frame state: %d frames, %d exact checks against backend vectors\n", frames, checks);
    return 0;
}
