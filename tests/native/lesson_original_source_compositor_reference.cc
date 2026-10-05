// Composes canonical frames from externally decoded original frames with the
// firmware evaluator, layout and compositor, writing raw RGB for comparison with a
// browser rendering of the same originals (cross-check tool, not a unit test).
#include "checked_cjson.h"
#include "lesson_original_source_compositor.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tbot;

static std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static ComposeSource Yuv(const std::string& bytes, int width, int height, bool alpha) {
    ComposeSource source;
    const auto* base = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const int chroma = ((width + 1) / 2) * ((height + 1) / 2);
    source.planes[0] = base;
    source.planes[1] = base + width * height;
    source.planes[2] = base + width * height + chroma;
    source.planes[3] = alpha ? base + width * height + 2 * chroma : nullptr;
    source.strides[0] = width;
    source.strides[1] = source.strides[2] = (width + 1) / 2;
    source.strides[3] = width;
    source.width = width;
    source.height = height;
    source.alpha = alpha;
    return source;
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;  // frame-state vectors, compose dir (picks.json + raw inputs), output dir
    CheckedCJsonPtr vectors(cJSON_Parse(Read(argv[1]).c_str()));
    CheckedCJsonPtr picks(cJSON_Parse(Read(std::string(argv[2]) + "/picks.json").c_str()));
    if (!vectors || !picks) return 3;
    TVideoScenePath scene;
    if (ParseTVideoScenePath(cJSON_GetObjectItem(vectors.get(), "journey"), &scene) != nullptr) return 4;
    const cJSON* cues = cJSON_GetObjectItem(vectors.get(), "cues");
    int index = 0;
    for (const cJSON* pick = picks->child; pick != nullptr; pick = pick->next, ++index) {
        const std::string cue_id = cJSON_GetObjectItem(pick, "cueId")->valuestring;
        const cJSON* cue = nullptr;
        for (const cJSON* entry = cues->child; entry != nullptr; entry = entry->next) {
            if (cue_id == cJSON_GetObjectItem(entry, "cueId")->valuestring) cue = entry;
        }
        if (cue == nullptr) return 5;
        TVideoFrameInput input;
        input.scene = &scene;
        ParseTVideoEffect(cJSON_GetObjectItem(cue, "effect")->valuestring, &input.effect);
        input.time_ms = cJSON_GetObjectItem(pick, "timeMs")->valuedouble;
        input.progress_index = cJSON_GetObjectItem(cJSON_GetObjectItem(cue, "progress"), "index")->valueint;
        input.progress_count = cJSON_GetObjectItem(cJSON_GetObjectItem(cue, "progress"), "count")->valueint;
        TVideoFrameState state;
        TVideoFrameLayout layout;
        if (EvaluateTVideoFrame(input, &state) != nullptr) return 6;
        LayoutTVideoFrame(state, &layout);
        const std::string dir = argv[2];
        const std::string background = Read(dir + "/" + std::to_string(index) + "-bg.raw");
        const std::string robot = Read(dir + "/" + std::to_string(index) + "-robot.raw");
        const std::string object = Read(dir + "/obj-" + cJSON_GetObjectItem(pick, "stepKey")->valuestring + ".raw");
        ComposeSource object_source;
        object_source.planes[0] = reinterpret_cast<const std::uint8_t*>(object.data());
        object_source.strides[0] = 512 * 4;
        object_source.width = object_source.height = 512;
        object_source.rgba = true;
        std::vector<std::uint8_t> rgb(kTVideoStageWidth * kTVideoStageHeight * 3);
        ComposeTVideoMediaLayers(layout, Yuv(background, 480, 320, false), object_source, Yuv(robot, 480, 480, true),
                                 rgb.data());
        std::ofstream out(std::string(argv[3]) + "/" + std::to_string(index) + "-cpp.rgb", std::ios::binary);
        out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        std::printf("composed %s@%g clip=%d\n", cue_id.c_str(), input.time_ms, static_cast<int>(layout.robot.clip_role));
    }
    return 0;
}
