#include "checked_cjson.h"
#include "lesson_original_source_scene_controller.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;  // original-source-scene vectors, frame-state vectors
    CheckedCJsonPtr scenes(cJSON_Parse(Read(argv[1]).c_str()));
    CheckedCJsonPtr frames(cJSON_Parse(Read(argv[2]).c_str()));
    if (!scenes || !frames) return 3;
    const cJSON* farm = Get(Get(scenes.get(), "scenes"), "valid")->child;  // farm-real-originals
    const std::string scene_json = Get(farm, "canonicalJson")->valuestring;
    const auto loader = [&](const std::string&, const std::string& sha, std::uint32_t, std::string* json) -> const char* {
        if (sha != Get(farm, "canonicalSha256")->valuestring) return "unknown scene";
        *json = scene_json;
        return nullptr;
    };

    // Cue plan derived on the device equals the backend cue plan.
    CheckedCJsonPtr scene(cJSON_Parse(scene_json.c_str()));
    std::vector<OriginalSourceCue> plan;
    Expect(DeriveOriginalSourceCuePlan(Get(scene.get(), "journey"), &plan) == nullptr, "cue plan");
    const cJSON* cues = Get(frames.get(), "cues");
    Expect(static_cast<int>(plan.size()) == cJSON_GetArraySize(cues), "cue count");
    int index = 0;
    for (const cJSON* cue = cues->child; cue != nullptr && index < static_cast<int>(plan.size()); cue = cue->next, ++index) {
        TVideoEffect effect;
        ParseTVideoEffect(Get(cue, "effect")->valuestring, &effect);
        Expect(plan[index].cue_id == Get(cue, "cueId")->valuestring && plan[index].effect == effect &&
               plan[index].step_key == Get(cue, "stepKey")->valuestring &&
               plan[index].progress_index == Get(Get(cue, "progress"), "index")->valueint, "cue " + plan[index].cue_id);
    }

    // Shared ordering vectors: accepted iff applied/replayed, state follows the contract.
    // Their scene reference names a farm-original cache key with the real farm scene.
    for (const cJSON* ordering = Get(scenes.get(), "orderings")->child; ordering != nullptr; ordering = ordering->next) {
        OriginalSourceSceneController controller(loader);
        std::uint64_t now = 0;
        for (const cJSON* step = Get(ordering, "steps")->child; step != nullptr; step = step->next) {
            const std::string verdict = Get(step, "verdict")->valuestring;
            const auto result = controller.Handle(Get(step, "frameType")->valuestring, Get(step, "body"), now += 100);
            const std::string at = std::string(Get(ordering, "name")->valuestring) + " " + verdict;
            const bool scene_known = true;
            Expect(result.accepted == ((verdict == "applied" || verdict == "replayed") && scene_known), at);
            static const char* const kPhases[] = {"idle", "prepared", "running", "paused"};
            Expect(std::string(kPhases[static_cast<int>(controller.phase())]) == Get(step, "state")->valuestring, at + " state");
        }
    }

    // Playback clock: prepare at t, start at t, frame at t + timeMs equals the vectors.
    for (const cJSON* cue = cues->child; cue != nullptr; cue = cue->next) {
        OriginalSourceSceneController controller(loader);
        CheckedCJsonPtr prepare(cJSON_Parse(
            (std::string("{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"") + Get(cue, "cueId")->valuestring +
             "\",\"commandSequenceId\":1,\"scene\":{\"cacheKey\":\"farm-original/v1-" + std::string(64, 'a') +
             "\",\"sha256\":\"" + Get(farm, "canonicalSha256")->valuestring + "\",\"bytes\":" +
             std::to_string(scene_json.size()) + "}}}").c_str()));
        CheckedCJsonPtr start(cJSON_Parse((std::string("{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"") +
                                            Get(cue, "cueId")->valuestring + "\",\"commandSequenceId\":2}}").c_str()));
        Expect(controller.Handle("lesson_prepare", prepare.get(), 1000).accepted, "prepare cue");
        Expect(controller.Handle("lesson_start", start.get(), 1000).accepted, "start cue");
        for (const cJSON* frame = Get(cue, "frames")->child; frame != nullptr; frame = frame->next) {
            const double time = Get(frame, "timeMs")->valuedouble;
            // The device clock is never negative; a looping cue wraps at its duration,
            // so only once cues hold the clamped final frame.
            const double duration = Get(cue, "durationMs")->valuedouble;
            if (time < 0 || time > duration) continue;
            if (time == duration && (std::string(Get(cue, "effect")->valuestring) == "greet" ||
                                     std::string(Get(cue, "effect")->valuestring) == "listen" ||
                                     std::string(Get(cue, "effect")->valuestring) == "thinking")) continue;
            TVideoFrameState state;
            TVideoFrameLayout layout;
            Expect(controller.FrameAt(1000 + static_cast<std::uint64_t>(time), &state, &layout), "frame");
            Expect(state.time_ms == Get(Get(frame, "state"), "timeMs")->valuedouble &&
                   layout.robot.anchor_x == Get(Get(Get(frame, "layout"), "robot"), "anchorX")->valuedouble,
                   std::string(Get(cue, "cueId")->valuestring) + " frame " + std::to_string(time));
        }
    }
    // A looping cue wraps to its first frame at its duration; a once cue holds the end.
    for (const char* cue_id : {"barn-listen", "barn-teach"}) {
        OriginalSourceSceneController controller(loader);
        CheckedCJsonPtr prepare(cJSON_Parse((std::string("{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"") +
            cue_id + "\",\"commandSequenceId\":1,\"scene\":{\"cacheKey\":\"farm-original/v1-" + std::string(64, 'a') +
            "\",\"sha256\":\"" + Get(farm, "canonicalSha256")->valuestring + "\",\"bytes\":" +
            std::to_string(scene_json.size()) + "}}}").c_str()));
        CheckedCJsonPtr start(cJSON_Parse((std::string("{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"") +
                                            cue_id + "\",\"commandSequenceId\":2}}").c_str()));
        controller.Handle("lesson_prepare", prepare.get(), 0);
        controller.Handle("lesson_start", start.get(), 0);
        TVideoFrameState state;
        TVideoFrameLayout layout;
        const double duration = TVideoEffectDurationMs(controller.active_cue()->effect);
        controller.FrameAt(static_cast<std::uint64_t>(duration), &state, &layout);
        Expect(controller.active_cue()->loop ? state.time_ms == 0 : state.time_ms == duration,
               std::string(cue_id) + " wraps only when looping");
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("PASS original-source scene controller: %d checks against backend vectors\n", checks);
    return 0;
}
