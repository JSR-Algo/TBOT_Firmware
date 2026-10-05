// Cross-check tool (not a unit test): rasterizes every paint-op trace frame with the
// firmware evaluator, layout, painter and software canvas, shapes only (no media, no
// glyphs, the vectors' measureText model), and compares each with a Chromium RGBA
// rendering of the same frame by the admin canonical painter.
#include "checked_cjson.h"
#include "lesson_tvideo_raster_canvas.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tbot;

namespace {
std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
const cJSON* Get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

class MeasureOnly final : public TVideoTextRenderer {
public:
    double Measure(const std::string& text, int font_px) override { return text.size() * font_px * .48; }
    void Render(const std::string&, int, double, double, double, const TVideoCoveragePlot&) override {}
};
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) return 2;  // paint-op vectors, frame-state vectors, chromium dir (<index>.rgba)
    CheckedCJsonPtr paint(cJSON_Parse(Read(argv[1]).c_str()));
    CheckedCJsonPtr frames(cJSON_Parse(Read(argv[2]).c_str()));
    if (!paint || !frames) return 3;
    TVideoScenePath scene;
    if (ParseTVideoScenePath(Get(frames.get(), "journey"), &scene) != nullptr) return 4;
    std::printf("[\n");
    int index = 0;
    double worst_mae = 0;
    int worst_max = 0;
    for (const cJSON* trace = Get(paint.get(), "traces")->child; trace != nullptr; trace = trace->next, ++index) {
        const std::string cue_id = Get(trace, "cueId")->valuestring;
        const cJSON* cue = Get(frames.get(), "cues")->child;
        while (cue != nullptr && cue_id != Get(cue, "cueId")->valuestring) cue = cue->next;
        TVideoFrameInput input;
        input.scene = &scene;
        ParseTVideoEffect(Get(cue, "effect")->valuestring, &input.effect);
        input.time_ms = Get(trace, "frameIndex")->valuedouble * 100;
        input.progress_index = Get(Get(cue, "progress"), "index")->valueint;
        input.progress_count = Get(Get(cue, "progress"), "count")->valueint;
        const cJSON* copy = Get(cue, "copy");
        TVideoFrameState state;
        TVideoFrameLayout layout;
        EvaluateTVideoFrame(input, &state);
        LayoutTVideoFrame(state, &layout);
        std::vector<std::uint8_t> rgb(480 * 320 * 3);
        MeasureOnly text;
        TVideoRasterCanvas canvas(rgb.data(), &text);
        const char* error = PaintTVideoFrame(&canvas, state, layout,
                                             TVideoCopy{Get(copy, "label")->valuestring, Get(copy, "prompt")->valuestring,
                                                        Get(copy, "correct")->valuestring, Get(copy, "retry")->valuestring});
        const std::string chromium = Read(std::string(argv[3]) + "/" + std::to_string(index) + ".rgba");
        if (error != nullptr || canvas.unsupported() != nullptr || chromium.size() != 480 * 320 * 4) return 5;
        double sum = 0;
        int max = 0, over8 = 0, over24 = 0;
        for (int pixel = 0; pixel < 480 * 320; ++pixel) {
            for (int channel = 0; channel < 3; ++channel) {
                const int diff = std::abs(rgb[pixel * 3 + channel] -
                                          static_cast<std::uint8_t>(chromium[pixel * 4 + channel]));
                sum += diff;
                max = std::max(max, diff);
                over8 += diff > 8;
                over24 += diff > 24;
            }
        }
        const double mae = sum / (480 * 320 * 3);
        worst_mae = std::max(worst_mae, mae);
        worst_max = std::max(worst_max, max);
        std::printf("  {\"trace\":%d,\"cueId\":\"%s\",\"frameIndex\":%d,\"mae\":%.4f,\"max\":%d,\"pctOver8\":%.4f,"
                    "\"pctOver24\":%.4f},\n",
                    index, cue_id.c_str(), Get(trace, "frameIndex")->valueint, mae, max,
                    100.0 * over8 / (480 * 320 * 3), 100.0 * over24 / (480 * 320 * 3));
    }
    std::printf("  {\"traces\":%d,\"worstMae\":%.4f,\"worstMax\":%d}\n]\n", index, worst_mae, worst_max);
    return 0;
}
