#ifndef LESSON_ORIGINAL_SOURCE_CONTRACT_H
#define LESSON_ORIGINAL_SOURCE_CONTRACT_H

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <vector>

// Renderer v6 original-source scene and control contract (original-source-scene.v1).
// The backend produces the canonical scene and shared vectors; this parser accepts
// exactly what a device needs to fetch, decode and order playback. Authoring-only
// rules (pronunciation symbols, answer wording) remain backend responsibilities.
namespace tbot {

inline constexpr char kOriginalSourceSceneContract[] = "original-source-scene.v1";
inline constexpr char kLessonRendererV6[] = "teebot-lesson-renderer.v6";
inline constexpr std::uint32_t kOriginalSourceMaxBytes = 4u * 1024u * 1024u;
inline constexpr std::uint32_t kOriginalSourceMaxDimension = 512;
inline constexpr std::uint32_t kOriginalSourceMaxFrames = 10000;
inline constexpr std::uint32_t kOriginalSourceMaxTimeBase = 2147483647u;
inline constexpr std::size_t kOriginalSourceMaxOriginals = 63;
inline constexpr std::uint32_t kOriginalSourceSceneMaxBytes = 256u * 1024u;

enum class OriginalSourceCodecId : std::uint8_t { kH264, kVp9Alpha, kPng };

struct OriginalSourceOriginal {
    std::string asset_version_id;
    std::string sha256;
    std::uint32_t bytes = 0;
    OriginalSourceCodecId codec = OriginalSourceCodecId::kPng;
    std::string codec_profile;
    std::uint32_t width = 0, height = 0, frame_count = 0;
    std::uint32_t time_base_num = 0, time_base_den = 0;
};

struct OriginalSourceSceneInfo {
    std::vector<OriginalSourceOriginal> originals;
    std::vector<std::string> required_capabilities;
    std::size_t step_count = 0;
};

// Returns nullptr on success, otherwise a static reason string.
const char* ParseOriginalSourceScene(const cJSON* scene, OriginalSourceSceneInfo* out);
std::vector<std::string> MissingOriginalSourceCapabilities(
    const OriginalSourceSceneInfo& scene, const std::vector<std::string>& advertised);

enum class OriginalSourceCommandName : std::uint8_t { kPrepare, kStart, kPause, kResume, kStop, kCancel };

struct OriginalSourceCommandInfo {
    OriginalSourceCommandName command = OriginalSourceCommandName::kPrepare;
    std::string cue_id;
    std::uint64_t command_sequence_id = 0;
    std::string reason;
    std::string scene_cache_key, scene_sha256;
    std::uint32_t scene_bytes = 0;
};

const char* ParseOriginalSourceCommand(const char* frame_type, const cJSON* body,
                                       OriginalSourceCommandInfo* out);

enum class OriginalSourceRendererPhase : std::uint8_t { kIdle, kPrepared, kRunning, kPaused };
enum class OriginalSourceVerdict : std::uint8_t { kApplied, kReplayed, kStale, kInvalidState };

struct OriginalSourceControlState {
    OriginalSourceRendererPhase phase = OriginalSourceRendererPhase::kIdle;
    std::string cue_id;
    std::uint64_t last_sequence = 0;
    std::string last_fingerprint;
};

// Shared ordering rules (mirrors the v4 renderer): identical command at the last
// accepted sequence replays; other command at that sequence, a lower sequence or
// another cue is stale; rejections never change state; prepare replaces the cue.
OriginalSourceVerdict ApplyOriginalSourceCommand(OriginalSourceControlState* state,
                                                 const OriginalSourceCommandInfo& command);

}  // namespace tbot

#endif
