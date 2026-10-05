// The firmware painter reproduces every canonical renderer paint-op trace
// (tvideo-journey-paint-ops.v1, recorded by the backend from render-entry.mjs) exactly:
// call sequence, arguments and the canvas state at each call.
#include "checked_cjson.h"
#include "lesson_tvideo_painter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tbot;

namespace {
int failures = 0, checks = 0;
void Expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition && failures++ < 20) std::fprintf(stderr, "FAIL %s\n", message.c_str());
}
std::string Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
const cJSON* Get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

struct Arg {
    enum Kind { kNumber, kString, kMedia } kind;
    double number = 0;
    std::string text;
};
struct State {
    std::string fill = "#000000", stroke = "#000000";
    double alpha = 1, line_width = 1;
    std::string font = "10px sans-serif", align = "start", baseline = "alphabetic";
};
struct Op {
    std::string name;
    std::vector<Arg> args;
    State state;
};

const char* MediaName(TVideoMedia media) {
    switch (media) {
        case TVideoMedia::kBackground: return "background";
        case TVideoMedia::kTeachingObject: return "teachingObject";
        case TVideoMedia::kRobotFlight: return "robot-flight";
        case TVideoMedia::kRobotWalking: return "robot-walking";
        case TVideoMedia::kRobotGreetingTeaching: return "robot-greeting-teaching";
        case TVideoMedia::kRobotCelebration: return "robot-celebration";
    }
    return "";
}

// Mirrors the backend recorder: state changes only through property sets, every
// call is recorded with the state at that moment, and measureText uses the
// vectors' deterministic model.
class RecordingCanvas final : public TVideoCanvas {
public:
    std::vector<Op> ops;
    void SetFillStyle(const char* style) override { state_.fill = style; }
    void SetStrokeStyle(const char* style) override { state_.stroke = style; }
    void SetGlobalAlpha(double alpha) override { state_.alpha = alpha; }
    void SetLineWidth(double width) override { state_.line_width = width; }
    void SetFont(const char* font) override { state_.font = font; }
    void SetTextAlign(const char* align) override { state_.align = align; }
    void SetTextBaseline(const char* baseline) override { state_.baseline = baseline; }
    void Save() override { Record("save", {}); }
    void Restore() override { Record("restore", {}); }
    void Translate(double x, double y) override { Record("translate", {N(x), N(y)}); }
    void Scale(double x, double y) override { Record("scale", {N(x), N(y)}); }
    void Rotate(double radians) override { Record("rotate", {N(radians)}); }
    void DrawMedia(TVideoMedia media, double x, double y, double w, double h) override {
        Record("drawImage", {Arg{Arg::kMedia, 0, MediaName(media)}, N(x), N(y), N(w), N(h)});
    }
    void BeginPath() override { Record("beginPath", {}); }
    void ClosePath() override { Record("closePath", {}); }
    void Clip() override { Record("clip", {}); }
    void Rect(double x, double y, double w, double h) override { Record("rect", {N(x), N(y), N(w), N(h)}); }
    void MoveTo(double x, double y) override { Record("moveTo", {N(x), N(y)}); }
    void LineTo(double x, double y) override { Record("lineTo", {N(x), N(y)}); }
    void FillRect(double x, double y, double w, double h) override { Record("fillRect", {N(x), N(y), N(w), N(h)}); }
    void FillText(const std::string& text, double x, double y) override { Record("fillText", {S(text), N(x), N(y)}); }
    void Arc(double x, double y, double r, double a0, double a1) override {
        Record("arc", {N(x), N(y), N(r), N(a0), N(a1)});
    }
    void Ellipse(double x, double y, double rx, double ry, double rotation, double a0, double a1) override {
        Record("ellipse", {N(x), N(y), N(rx), N(ry), N(rotation), N(a0), N(a1)});
    }
    void RoundRect(double x, double y, double w, double h, double r) override {
        Record("roundRect", {N(x), N(y), N(w), N(h), N(r)});
    }
    void Fill() override { Record("fill", {}); }
    void Stroke() override { Record("stroke", {}); }
    double MeasureText(const std::string& text) override {
        Record("measureText", {S(text)});
        const char* px = std::strstr(state_.font.c_str(), "px");
        const char* digits = px;
        while (digits > state_.font.c_str() && digits[-1] >= '0' && digits[-1] <= '9') --digits;
        const double size = px != nullptr && digits < px ? std::atof(std::string(digits, px).c_str()) : 16;
        return static_cast<double>(text.size()) * size * .48;
    }

private:
    static Arg N(double value) { return Arg{Arg::kNumber, value, {}}; }
    static Arg S(const std::string& value) { return Arg{Arg::kString, 0, value}; }
    void Record(const char* name, std::vector<Arg> args) { ops.push_back({name, std::move(args), state_}); }
    State state_;
};

bool SameArg(const Arg& arg, const cJSON* expected) {
    if (arg.kind == Arg::kNumber) return cJSON_IsNumber(expected) && expected->valuedouble == arg.number;
    if (arg.kind == Arg::kString) return cJSON_IsString(expected) && arg.text == expected->valuestring;
    const cJSON* media = Get(expected, "media");
    return cJSON_IsString(media) && arg.text == media->valuestring;
}

bool SameState(const State& state, const cJSON* expected) {
    return state.fill == Get(expected, "fillStyle")->valuestring &&
           state.stroke == Get(expected, "strokeStyle")->valuestring &&
           state.alpha == Get(expected, "globalAlpha")->valuedouble &&
           state.line_width == Get(expected, "lineWidth")->valuedouble &&
           state.font == Get(expected, "font")->valuestring && state.align == Get(expected, "textAlign")->valuestring &&
           state.baseline == Get(expected, "textBaseline")->valuestring;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;  // paint-op vectors, frame-state vectors
    CheckedCJsonPtr paint(cJSON_Parse(Read(argv[1]).c_str()));
    CheckedCJsonPtr frames(cJSON_Parse(Read(argv[2]).c_str()));
    if (!paint || !frames) return 3;
    TVideoScenePath scene;
    if (ParseTVideoScenePath(Get(frames.get(), "journey"), &scene) != nullptr) return 4;
    int traces = 0, compared = 0;
    for (const cJSON* trace = Get(paint.get(), "traces")->child; trace != nullptr; trace = trace->next, ++traces) {
        const std::string cue_id = Get(trace, "cueId")->valuestring;
        const cJSON* cue = Get(frames.get(), "cues")->child;
        while (cue != nullptr && cue_id != Get(cue, "cueId")->valuestring) cue = cue->next;
        if (cue == nullptr) return 5;
        TVideoFrameInput input;
        input.scene = &scene;
        if (!ParseTVideoEffect(Get(cue, "effect")->valuestring, &input.effect)) return 6;
        input.time_ms = Get(trace, "frameIndex")->valuedouble * 100;
        input.progress_index = Get(Get(cue, "progress"), "index")->valueint;
        input.progress_count = Get(Get(cue, "progress"), "count")->valueint;
        const cJSON* copy_json = Get(cue, "copy");
        const TVideoCopy copy{Get(copy_json, "label")->valuestring, Get(copy_json, "prompt")->valuestring,
                              Get(copy_json, "correct")->valuestring, Get(copy_json, "retry")->valuestring};
        Expect(copy.correct == kTVideoCorrectLabel && copy.retry == kTVideoRetryLabel, "coaching labels " + cue_id);
        TVideoFrameState state;
        TVideoFrameLayout layout;
        if (EvaluateTVideoFrame(input, &state) != nullptr) return 7;
        LayoutTVideoFrame(state, &layout);
        RecordingCanvas canvas;
        const std::string at = cue_id + "#" + std::to_string(Get(trace, "frameIndex")->valueint);
        Expect(PaintTVideoFrame(&canvas, state, layout, copy) == nullptr, "paint " + at);
        const cJSON* expected = Get(trace, "ops");
        Expect(static_cast<int>(canvas.ops.size()) == cJSON_GetArraySize(expected), "op count " + at);
        int index = 0;
        for (const cJSON* op = expected->child; op != nullptr && index < static_cast<int>(canvas.ops.size());
             op = op->next, ++index) {
            const Op& actual = canvas.ops[index];
            bool same = actual.name == Get(op, "op")->valuestring &&
                        static_cast<int>(actual.args.size()) == cJSON_GetArraySize(Get(op, "args"));
            int arg_index = 0;
            for (const cJSON* arg = Get(op, "args")->child; same && arg != nullptr; arg = arg->next, ++arg_index) {
                same = SameArg(actual.args[arg_index], arg);
            }
            Expect(same && SameState(actual.state, Get(op, "state")),
                   at + " op " + std::to_string(index) + " " + actual.name + " vs " + Get(op, "op")->valuestring);
            ++compared;
        }
    }

    // A prompt that fits no approved font size is refused, as the canonical renderer refuses it.
    {
        TVideoFrameInput input;
        input.scene = &scene;
        input.effect = TVideoEffect::kTeach;
        input.time_ms = 1500;
        input.progress_index = 1;
        input.progress_count = 2;
        TVideoFrameState state;
        TVideoFrameLayout layout;
        EvaluateTVideoFrame(input, &state);
        LayoutTVideoFrame(state, &layout);
        RecordingCanvas canvas;
        Expect(layout.has_card, "teach frame has a card");
        Expect(PaintTVideoFrame(&canvas, state, layout,
                                TVideoCopy{"barn", "Supercalifragilisticexpialidocious", kTVideoCorrectLabel,
                                           kTVideoRetryLabel}) != nullptr,
               "overlong prompt word is refused");
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d failures of %d checks\n", failures, checks);
        return 1;
    }
    std::printf("PASS tvideo painter: %d traces, %d ops identical to the canonical renderer (%d checks)\n", traces,
                compared, checks);
    return 0;
}
