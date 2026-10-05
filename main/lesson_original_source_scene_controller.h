#ifndef LESSON_ORIGINAL_SOURCE_SCENE_CONTROLLER_H
#define LESSON_ORIGINAL_SOURCE_SCENE_CONTROLLER_H

#include "lesson_original_source_contract.h"
#include "lesson_tvideo_frame_state.h"
#include "lesson_tvideo_painter.h"

#include <cJSON.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Renderer-v6 device protocol: consumes original-source-scene.v1 control bodies,
// applies the shared ordering rules, loads the verified scene, keeps the cue clock
// and produces the ACK payloads the ESP lesson runtime accepts. Pure logic; the
// lesson handler supplies scene bytes (SD pack, read lease, sha256) and the clock.
namespace tbot {

struct OriginalSourceCue {
    std::string cue_id, step_key;
    TVideoEffect effect = TVideoEffect::kOpening;
    bool loop = false;
    int progress_index = 0, progress_count = 0;
    TVideoCopy copy;                 // card text of the owning step
    std::string teaching_object_id;  // owning step's teachingObject assetVersionId
};

// Cue plan of a tvideoJourney.v1 journey, in the backend's order.
const char* DeriveOriginalSourceCuePlan(const cJSON* journey, std::vector<OriginalSourceCue>* out);

// Returns nullptr and fills `json` with the exact scene bytes, or a reason.
using OriginalSourceSceneLoader =
    std::function<const char*(const std::string& cache_key, const std::string& sha256, std::uint32_t bytes,
                              std::string* json)>;

struct OriginalSourceControlResult {
    bool accepted = false;
    std::string error;          // stale / invalid-state / contract / scene reason when not accepted
    std::string ack_json;       // body.cinematicPhase ACK object, when accepted
    bool asset_pack_ready = false;  // initial prepare: body.assetPack {ready, cacheKey}
    std::string cache_key;
    bool terminal = false;      // stop / cancel ended the cue
};

class OriginalSourceSceneController {
public:
    explicit OriginalSourceSceneController(OriginalSourceSceneLoader loader) : loader_(std::move(loader)) {}

    OriginalSourceControlResult Handle(const char* frame_type, const cJSON* body, std::uint64_t now_ms);
    // Canonical frame for the active cue at `now_ms`; false when no cue is playing.
    bool FrameAt(std::uint64_t now_ms, TVideoFrameState* state, TVideoFrameLayout* layout) const;

    const OriginalSourceCue* active_cue() const { return cue_index_ < 0 ? nullptr : &cues_[cue_index_]; }
    OriginalSourceRendererPhase phase() const { return control_.phase; }

private:
    const char* LoadScene(const OriginalSourceCommandInfo& command);
    double CueTimeMs(std::uint64_t now_ms) const;

    OriginalSourceSceneLoader loader_;
    OriginalSourceControlState control_;
    std::string scene_sha256_, cache_key_;
    TVideoScenePath scene_path_;
    std::vector<OriginalSourceCue> cues_;
    int cue_index_ = -1;
    bool prepared_once_ = false;
    std::uint64_t started_at_ms_ = 0, paused_at_ms_ = 0;
    std::string last_ack_;
    bool last_asset_pack_ready_ = false;
};

}  // namespace tbot

#endif
