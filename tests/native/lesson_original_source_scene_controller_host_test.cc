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
        const cJSON* copy = Get(cue, "copy");
        Expect(plan[index].copy.label == Get(copy, "label")->valuestring &&
               plan[index].copy.prompt == Get(copy, "prompt")->valuestring &&
               plan[index].copy.correct == Get(copy, "correct")->valuestring &&
               plan[index].copy.retry == Get(copy, "retry")->valuestring, "copy " + plan[index].cue_id);
        // The teaching object is the owning step's: a word transition shows the next word.
        const cJSON* owner = Get(Get(scene.get(), "journey"), "steps")->child;
        while (owner != nullptr && plan[index].step_key != Get(owner, "stepKey")->valuestring) owner = owner->next;
        Expect(owner != nullptr && plan[index].teaching_object_id ==
                   Get(Get(owner, "teachingObject"), "assetVersionId")->valuestring, "object " + plan[index].cue_id);
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
    // A refused prepare leaves the loaded scene and the playing cue untouched: scene B
    // (same bytes, renamed step) loads, but its plan has no "barn-teach", so the
    // prepare is refused and frames still come from scene A's "barn-teach".
    {
        std::string renamed = scene_json;
        for (std::size_t at = renamed.find("\"stepKey\":\"barn\""); at != std::string::npos;
             at = renamed.find("\"stepKey\":\"barn\"", at + 1)) {
            renamed.replace(at, 16, "\"stepKey\":\"bxrn\"");
        }
        const std::string sha_b(64, 'b');
        const auto two_scenes = [&](const std::string& key, const std::string& sha, std::uint32_t bytes,
                                    std::string* json) -> const char* {
            if (sha == sha_b) {
                *json = renamed;
                return nullptr;
            }
            return loader(key, sha, bytes, json);
        };
        const auto prepare_body = [&](const char* cue_id, int sequence, const std::string& sha) {
            return std::string("{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"") + cue_id +
                   "\",\"commandSequenceId\":" + std::to_string(sequence) + ",\"scene\":{\"cacheKey\":\"farm-original/v1-" +
                   std::string(64, 'a') + "\",\"sha256\":\"" + sha + "\",\"bytes\":" +
                   std::to_string(scene_json.size()) + "}}}";
        };
        const std::string sha_a = Get(farm, "canonicalSha256")->valuestring;
        OriginalSourceSceneController reference(loader), controller(two_scenes);
        for (auto* each : {&reference, &controller}) {
            CheckedCJsonPtr prepare(cJSON_Parse(prepare_body("barn-teach", 1, sha_a).c_str()));
            CheckedCJsonPtr start(cJSON_Parse(
                "{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"barn-teach\",\"commandSequenceId\":2}}"));
            each->Handle("lesson_prepare", prepare.get(), 0);
            each->Handle("lesson_start", start.get(), 0);
        }
        CheckedCJsonPtr scene_b(cJSON_Parse(prepare_body("bxrn-teach", 3, sha_b).c_str()));
        CheckedCJsonPtr missing(cJSON_Parse(prepare_body("barn-teach", 3, sha_b).c_str()));
        Expect(!controller.Handle("lesson_prepare", missing.get(), 100).accepted, "cue missing from scene B is refused");
        Expect(controller.active_cue() != nullptr && controller.active_cue()->cue_id == "barn-teach",
               "refused prepare keeps the playing cue");
        TVideoFrameState expected, actual;
        TVideoFrameLayout expected_layout, actual_layout;
        reference.FrameAt(1500, &expected, &expected_layout);
        Expect(controller.FrameAt(1500, &actual, &actual_layout) && actual.time_ms == expected.time_ms &&
                   actual_layout.robot.anchor_x == expected_layout.robot.anchor_x,
               "refused prepare keeps scene A frames");
        // The refused sequence was not consumed and scene B is still loadable.
        Expect(controller.Handle("lesson_prepare", scene_b.get(), 200).accepted &&
                   controller.active_cue()->cue_id == "bxrn-teach", "scene B prepare at the same sequence");

        // A failing prepare check refuses the prepare without consuming its sequence.
        OriginalSourceSceneController checked(loader);
        bool fail_check = true;
        std::string checked_cue;
        checked.SetPrepareCheck([&](const OriginalSourcePrepareContext& context) -> const char* {
            checked_cue = context.cue.cue_id;
            return fail_check ? "first frame unavailable" : nullptr;
        });
        CheckedCJsonPtr prepare(cJSON_Parse(prepare_body("barn-greet", 1, sha_a).c_str()));
        const auto refused = checked.Handle("lesson_prepare", prepare.get(), 0);
        Expect(!refused.accepted && refused.error == "media: first frame unavailable" && checked_cue == "barn-greet" &&
                   checked.active_cue() == nullptr, "prepare check refusal");
        fail_check = false;
        Expect(checked.Handle("lesson_prepare", prepare.get(), 0).accepted && checked.active_cue() != nullptr,
               "same prepare accepted once the first frame is ready");
        Expect(checked.scene_assets().background_id == "10000000-0000-4000-8000-000000000001" &&
                   checked.scene_assets().robot_clip_ids[0] == "20000000-0000-4000-8000-000000000001" &&
                   checked.scene_info().originals.size() == 7, "scene assets of the loaded scene");
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("PASS original-source scene controller: %d checks against backend vectors\n", checks);
    return 0;
}
