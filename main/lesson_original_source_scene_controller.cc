#include "lesson_original_source_scene_controller.h"

#include "checked_cjson.h"

#include <algorithm>
#include <cstring>

namespace tbot {
namespace {

struct EffectSpec { const char* name; TVideoEffect effect; bool loop; };
constexpr EffectSpec kStepEffects[] = {
    {"teach", TVideoEffect::kTeach, false}, {"listen", TVideoEffect::kListen, true},
    {"thinking", TVideoEffect::kThinking, true}, {"correct", TVideoEffect::kCorrect, false},
    {"retry-level-1", TVideoEffect::kRetryLevel1, false}, {"retry-level-2", TVideoEffect::kRetryLevel2, false},
    {"retry-level-3", TVideoEffect::kRetryLevel3, false}, {"celebrate", TVideoEffect::kCelebrate, false},
};

const char* CommandName(OriginalSourceCommandName command) {
    static const char* const kNames[] = {"prepare", "start", "pause", "resume", "stop", "cancel"};
    return kNames[static_cast<int>(command)];
}

std::string AckJson(const OriginalSourceCommandInfo& command) {
    const char* event = command.command == OriginalSourceCommandName::kPrepare ? "frameZeroReady"
        : command.command == OriginalSourceCommandName::kStart ? "phaseReady" : "commandApplied";
    CheckedCJsonPtr ack(cJSON_CreateObject());
    if (!ack) return {};
    cJSON_AddStringToObject(ack.get(), "event", event);
    cJSON_AddStringToObject(ack.get(), "command", CommandName(command.command));
    cJSON_AddStringToObject(ack.get(), "cueId", command.cue_id.c_str());
    cJSON_AddNumberToObject(ack.get(), "commandSequenceId", static_cast<double>(command.command_sequence_id));
    cJSON_AddBoolToObject(ack.get(), "accepted", true);
    if (std::strcmp(event, "commandApplied") != 0) cJSON_AddBoolToObject(ack.get(), event, true);
    CheckedCJsonStringPtr text(cJSON_PrintUnformatted(ack.get()));
    return text ? std::string(text.get()) : std::string();
}

}  // namespace

const char* DeriveOriginalSourceCuePlan(const cJSON* journey, std::vector<OriginalSourceCue>* out) {
    const cJSON* steps = cJSON_GetObjectItemCaseSensitive(journey, "steps");
    if (!cJSON_IsArray(steps) || cJSON_GetArraySize(steps) == 0) return "journey steps";
    struct Step { std::string key; int index, count; TVideoCopy copy; std::string object_id; };
    std::vector<Step> parsed;
    for (const cJSON* step = steps->child; step != nullptr; step = step->next) {
        const cJSON* key = cJSON_GetObjectItemCaseSensitive(step, "stepKey");
        const cJSON* progress = cJSON_GetObjectItemCaseSensitive(step, "progress");
        const cJSON* index = cJSON_GetObjectItemCaseSensitive(progress, "index");
        const cJSON* count = cJSON_GetObjectItemCaseSensitive(progress, "count");
        const cJSON* word = cJSON_GetObjectItemCaseSensitive(step, "targetWord");
        const cJSON* prompt =
            cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(step, "teachingCopy"), "prompt");
        const cJSON* object = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(step, "teachingObject"), "assetVersionId");
        if (!cJSON_IsString(key) || !cJSON_IsNumber(index) || !cJSON_IsNumber(count) || !cJSON_IsString(word) ||
            !cJSON_IsString(prompt) || !cJSON_IsString(object)) {
            return "journey step";
        }
        parsed.push_back({key->valuestring, index->valueint, count->valueint,
                          TVideoCopy{word->valuestring, prompt->valuestring, kTVideoCorrectLabel, kTVideoRetryLabel},
                          object->valuestring});
    }
    std::vector<OriginalSourceCue> plan;
    const auto add = [&](const std::string& id, TVideoEffect effect, bool loop, const Step& owner) {
        plan.push_back({id, owner.key, effect, loop, owner.index, owner.count, owner.copy, owner.object_id});
    };
    add(parsed[0].key + "-opening", TVideoEffect::kOpening, false, parsed[0]);
    add(parsed[0].key + "-greet", TVideoEffect::kGreet, true, parsed[0]);
    for (std::size_t index = 0; index < parsed.size(); ++index) {
        for (const auto& spec : kStepEffects) add(parsed[index].key + "-" + spec.name, spec.effect, spec.loop, parsed[index]);
        if (index + 1 < parsed.size()) {
            add(parsed[index].key + "-to-" + parsed[index + 1].key + "-word-transition", TVideoEffect::kWordTransition,
                false, parsed[index + 1]);
        }
    }
    *out = std::move(plan);
    return nullptr;
}

const char* OriginalSourceSceneController::LoadScene(const OriginalSourceCommandInfo& command) {
    if (command.scene_sha256 == scene_sha256_ && command.scene_cache_key == cache_key_ && !cues_.empty()) return nullptr;
    std::string json;
    if (!loader_) return "scene loader unavailable";
    if (const char* error = loader_(command.scene_cache_key, command.scene_sha256, command.scene_bytes, &json)) return error;
    if (json.size() != command.scene_bytes) return "scene length differs from the reference";
    CheckedCJsonPtr scene(cJSON_Parse(json.c_str()));
    if (!scene) return "scene is not JSON";
    OriginalSourceSceneInfo info;
    if (const char* error = ParseOriginalSourceScene(scene.get(), &info)) return error;
    const cJSON* journey = cJSON_GetObjectItemCaseSensitive(scene.get(), "journey");
    TVideoScenePath path;
    std::vector<OriginalSourceCue> cues;
    if (const char* error = ParseTVideoScenePath(journey, &path)) return error;
    if (const char* error = DeriveOriginalSourceCuePlan(journey, &cues)) return error;
    scene_path_ = std::move(path);
    cues_ = std::move(cues);
    scene_sha256_ = command.scene_sha256;
    cache_key_ = command.scene_cache_key;
    return nullptr;
}

OriginalSourceControlResult OriginalSourceSceneController::Handle(const char* frame_type, const cJSON* body,
                                                                  std::uint64_t now_ms) {
    OriginalSourceControlResult result;
    OriginalSourceCommandInfo command;
    if (const char* error = ParseOriginalSourceCommand(frame_type, body, &command)) {
        result.error = std::string("contract: ") + error;
        return result;
    }
    OriginalSourceControlState next = control_;
    const OriginalSourceVerdict verdict = ApplyOriginalSourceCommand(&next, command);
    if (verdict == OriginalSourceVerdict::kReplayed) {
        // A retried frame gets the identical ACK; state and clock are unchanged.
        result.accepted = true;
        result.ack_json = last_ack_;
        result.asset_pack_ready = last_asset_pack_ready_;
        result.cache_key = cache_key_;
        return result;
    }
    if (verdict != OriginalSourceVerdict::kApplied) {
        result.error = verdict == OriginalSourceVerdict::kStale ? "stale command" : "invalid state";
        return result;
    }
    int cue_index = cue_index_;
    if (command.command == OriginalSourceCommandName::kPrepare) {
        if (const char* error = LoadScene(command)) {
            result.error = std::string("scene: ") + error;
            return result;
        }
        cue_index = -1;
        for (std::size_t index = 0; index < cues_.size(); ++index) {
            if (cues_[index].cue_id == command.cue_id) cue_index = static_cast<int>(index);
        }
        if (cue_index < 0) {
            result.error = "scene: cue is not in the scene plan";
            return result;
        }
    }
    // Commit only after every check passed: rejections never consume a sequence.
    control_ = next;
    cue_index_ = cue_index;
    switch (command.command) {
        case OriginalSourceCommandName::kPrepare: started_at_ms_ = paused_at_ms_ = now_ms; break;
        case OriginalSourceCommandName::kStart: started_at_ms_ = now_ms; break;
        case OriginalSourceCommandName::kPause: paused_at_ms_ = now_ms; break;
        case OriginalSourceCommandName::kResume: started_at_ms_ += now_ms - paused_at_ms_; break;
        case OriginalSourceCommandName::kStop:
        case OriginalSourceCommandName::kCancel:
            result.terminal = true;
            cue_index_ = -1;
            break;
    }
    result.accepted = true;
    result.ack_json = AckJson(command);
    result.asset_pack_ready = command.command == OriginalSourceCommandName::kPrepare && !prepared_once_;
    if (command.command == OriginalSourceCommandName::kPrepare) prepared_once_ = true;
    result.cache_key = cache_key_;
    last_ack_ = result.ack_json;
    last_asset_pack_ready_ = result.asset_pack_ready;
    return result;
}

double OriginalSourceSceneController::CueTimeMs(std::uint64_t now_ms) const {
    const OriginalSourceCue& cue = cues_[cue_index_];
    const double duration = TVideoEffectDurationMs(cue.effect);
    std::uint64_t elapsed = 0;
    if (control_.phase == OriginalSourceRendererPhase::kRunning) elapsed = now_ms - started_at_ms_;
    else if (control_.phase == OriginalSourceRendererPhase::kPaused) elapsed = paused_at_ms_ - started_at_ms_;
    const double local = cue.loop ? static_cast<double>(elapsed % static_cast<std::uint64_t>(duration))
                                  : std::min(static_cast<double>(elapsed), duration);
    return local;
}

bool OriginalSourceSceneController::FrameAt(std::uint64_t now_ms, TVideoFrameState* state,
                                            TVideoFrameLayout* layout) const {
    if (cue_index_ < 0) return false;
    const OriginalSourceCue& cue = cues_[cue_index_];
    TVideoFrameInput input;
    input.effect = cue.effect;
    input.time_ms = CueTimeMs(now_ms);
    input.scene = &scene_path_;
    input.progress_index = cue.progress_index;
    input.progress_count = cue.progress_count;
    if (EvaluateTVideoFrame(input, state) != nullptr) return false;
    LayoutTVideoFrame(*state, layout);
    return true;
}

}  // namespace tbot
