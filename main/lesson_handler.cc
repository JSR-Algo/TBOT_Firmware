// US-006 Slice-01 — additive lesson_* renderer (LANE-FIRMWARE / S10 / CP-6).
// See lesson_handler.h for the contract, the backward-compat anchor, and the
// D-PRELOAD-OWNER byte-source decision. THIN renderer: owns NO lesson business
// logic (the ESP Server is the interpreter/runtime and the arbiter of ordering,
// READY, and asset verification). This file only: parses the frozen envelope,
// gates on the contract identity + profile, renders ONE espTft lesson_step (s4
// "model") onto the real LVGL display via the degraded fallback ladder, and emits
// the canonical lesson_ack / lesson_progress / lesson_error frames.

#include "application.h"
#include "board.h"
#include "display.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "protocol.h"
#include "lesson_handler.h"
#include "lesson_tvideo_template.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_motion_presets.h"
#include "lesson_layer_state.h"
#include "lesson_cinematic_renderer.h"
#include "lesson_flattened_cinematic_renderer.h"
#include "system_info.h"
#include "lesson_tvideo_template.h"
// US-006 image render: the on-device LVGL decoder + draw path. LvglDisplay carries
// SetLessonBackground (the new persistent full-screen draw) and LvglAllocatedImage is
// the same decoded-bytes wrapper the proven mcp_server.cc preview path uses.
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "jpeg_to_image.h"

#include <ctime>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <memory>
#include <set>
#include <deque>
#include <map>
#include <atomic>

#include <cJSON.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#ifndef TBOT_HOST_NATIVE_COVERAGE
#include <src/draw/lv_image_decoder_private.h>
#include <src/draw/lv_draw_buf_private.h>
#include <src/osal/lv_os.h>
#endif

#define TAG "Lesson"

namespace {
std::atomic<int> g_cinematic_json_fail_after{-1};

#ifdef TBOT_HOST_NATIVE_COVERAGE
std::atomic<int> g_lesson_json_fail_after{-1};

bool LessonJsonOperationAllowed() {
    int remaining = g_lesson_json_fail_after.load(std::memory_order_relaxed);
    while (remaining >= 0) {
        if (remaining == 0) return false;
        if (g_lesson_json_fail_after.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed)) return true;
    }
    return true;
}

cJSON* LessonJsonCreateObject() {
    return LessonJsonOperationAllowed() ? cJSON_CreateObject() : nullptr;
}

cJSON* LessonJsonAddString(cJSON* object, const char* key, const char* value) {
    return LessonJsonOperationAllowed() ? cJSON_AddStringToObject(object, key, value) : nullptr;
}

cJSON* LessonJsonAddNumber(cJSON* object, const char* key, double value) {
    return LessonJsonOperationAllowed() ? cJSON_AddNumberToObject(object, key, value) : nullptr;
}

cJSON* LessonJsonAddBool(cJSON* object, const char* key, bool value) {
    return LessonJsonOperationAllowed() ? cJSON_AddBoolToObject(object, key, value) : nullptr;
}

cJSON* LessonJsonAddNull(cJSON* object, const char* key) {
    return LessonJsonOperationAllowed() ? cJSON_AddNullToObject(object, key) : nullptr;
}

cJSON_bool LessonJsonAttachItem(cJSON* object, const char* key, cJSON* item) {
    return LessonJsonOperationAllowed() ? cJSON_AddItemToObject(object, key, item) : false;
}

char* LessonJsonPrintUnformatted(const cJSON* item) {
    return LessonJsonOperationAllowed() ? cJSON_PrintUnformatted(item) : nullptr;
}
#else
#define LessonJsonCreateObject cJSON_CreateObject
#define LessonJsonAddString cJSON_AddStringToObject
#define LessonJsonAddNumber cJSON_AddNumberToObject
#define LessonJsonAddBool cJSON_AddBoolToObject
#define LessonJsonAddNull cJSON_AddNullToObject
#define LessonJsonAttachItem cJSON_AddItemToObject
#define LessonJsonPrintUnformatted cJSON_PrintUnformatted
#endif

bool CinematicJsonOperationAllowed() {
    int remaining = g_cinematic_json_fail_after.load(std::memory_order_relaxed);
    while (remaining >= 0) {
        if (remaining == 0) return false;
        if (g_cinematic_json_fail_after.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed)) return true;
    }
    return true;
}

cJSON* CinematicJsonCreateObject() {
    return CinematicJsonOperationAllowed() ? cJSON_CreateObject() : nullptr;
}

bool CinematicJsonAddString(cJSON* object, const char* key, const char* value) {
    return CinematicJsonOperationAllowed() &&
           cJSON_AddStringToObject(object, key, value) != nullptr;
}

bool CinematicJsonAddNumber(cJSON* object, const char* key, double value) {
    return CinematicJsonOperationAllowed() &&
           cJSON_AddNumberToObject(object, key, value) != nullptr;
}

bool CinematicJsonAddBool(cJSON* object, const char* key, bool value) {
    return CinematicJsonOperationAllowed() &&
           cJSON_AddBoolToObject(object, key, value) != nullptr;
}

LessonAssetSessionResult BeginCinematicLessonSession(
    const std::string& assignment_id,
    const std::string& session_id) {
    return LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        assignment_id, session_id);
}
}  // namespace

namespace tbot {
void SetLessonCinematicJsonFailAfterForTest(int successful_operations_before_failure) {
    g_cinematic_json_fail_after.store(successful_operations_before_failure,
                                      std::memory_order_relaxed);
}
#ifdef TBOT_HOST_NATIVE_COVERAGE
void SetLessonJsonFailAfterForTest(int successful_operations_before_failure) {
    g_lesson_json_fail_after.store(successful_operations_before_failure,
                                   std::memory_order_relaxed);
}
#endif
}  // namespace tbot

void AddLessonRendererFeatures(cJSON* features) {
    if (features == nullptr) return;
    cJSON_DeleteItemFromObject(features, "renderer");
    cJSON* renderers = cJSON_CreateArray();
    cJSON_AddItemToArray(renderers, cJSON_CreateString(kLessonRendererV1));
    cJSON_AddItemToArray(renderers, cJSON_CreateString(kLessonRendererV2));
    if (tbot::LessonCinematicRendererCapabilityReady()) {
        cJSON_AddItemToArray(renderers, cJSON_CreateString(tbot::kLessonRendererV3));
    }
    if (tbot::LessonFlattenedCinematicRendererCapabilityReady()) {
        cJSON_AddItemToArray(renderers, cJSON_CreateString(tbot::kLessonRendererV4));
    }
    cJSON_AddItemToObject(features, "renderer", renderers);

    cJSON* renderer_v2 = cJSON_CreateObject();
    cJSON_AddBoolToObject(renderer_v2, "openingEntrance", true);
    cJSON_AddBoolToObject(renderer_v2, "visualStateEvents", true);
    cJSON_AddStringToObject(renderer_v2, "physicalMotionOwner", "server");
    cJSON_AddBoolToObject(renderer_v2, "singleSpriteEntrance", true);
    cJSON_AddItemToObject(features, "lessonRendererV2", renderer_v2);
    if (tbot::LessonCinematicRendererCapabilityReady()) {
        cJSON* renderer_v3 = cJSON_CreateObject();
        cJSON_AddBoolToObject(renderer_v3, "directMp4Cinematic", true);
        cJSON_AddBoolToObject(renderer_v3, "sdAssetPack", true);
        cJSON_AddItemToObject(features, "lessonRendererV3", renderer_v3);
    }
    if (tbot::LessonFlattenedCinematicRendererCapabilityReady()) {
        cJSON* renderer_v4 = cJSON_CreateObject();
        cJSON_AddBoolToObject(renderer_v4, "flattenedMjpegCinematic", true);
        cJSON_AddBoolToObject(renderer_v4, "sdAssetPack", true);
        cJSON_AddItemToObject(features, "lessonRendererV4", renderer_v4);
    }
}

namespace {

// ---- null-safe accessors (every envelope + body.scene.* field is guarded) ----
const char* Str(const cJSON* o, const char* k) {
    if (o == nullptr) return nullptr;
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}
bool IsAsciiWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\v' || value == '\f';
}
bool Blank(const char* value) {
    if (value == nullptr) return true;
    while (*value != '\0') {
        if (!IsAsciiWhitespace(*value)) return false;
        ++value;
    }
    return true;
}
bool IsValidLessonIdentity(const char* value) {
    if (value == nullptr) return false;
    const size_t length = strlen(value);
    return length > 0 && length <= kLessonAssetIdentityMaxBytes;
}
void TrimAsciiWhitespace(std::string& value) {
    size_t first = 0;
    while (first < value.size() && IsAsciiWhitespace(value[first])) ++first;
    size_t last = value.size();
    while (last > first && IsAsciiWhitespace(value[last - 1])) --last;
    if (first != 0 || last != value.size()) {
        value = value.substr(first, last - first);
    }
}

std::string EncodeLessonAssetPathSegment(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char ch : value) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                          ch == '.' || ch == '~';
        if (safe) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4) & 0x0f]);
            encoded.push_back(kHex[ch & 0x0f]);
        }
    }
    return encoded;
}

size_t Utf8CharLen(unsigned char ch) {
    if ((ch & 0x80) == 0) return 1;
    if ((ch & 0xe0) == 0xc0) return 2;
    if ((ch & 0xf0) == 0xe0) return 3;
    if ((ch & 0xf8) == 0xf0) return 4;
    return 0;
}

size_t Utf8PrefixLen(const std::string& value, size_t max_bytes, size_t* last_space) {
    size_t pos = 0;
    size_t last_good = 0;
    if (last_space != nullptr) *last_space = std::string::npos;
    while (pos < value.size() && pos < max_bytes) {
        size_t len = Utf8CharLen(static_cast<unsigned char>(value[pos]));
        if (len == 0 || pos + len > max_bytes) break;
        bool valid = true;
        for (size_t i = 1; i < len; ++i) {
            if (pos + i >= value.size() ||
                (static_cast<unsigned char>(value[pos + i]) & 0xc0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        if (last_space != nullptr && IsAsciiWhitespace(value[pos])) *last_space = pos;
        pos += len;
        last_good = pos;
    }
    return last_good;
}

void TruncateUtf8(std::string& value, size_t max_bytes) {
    size_t last_space = std::string::npos;
    const size_t last_good = Utf8PrefixLen(value, max_bytes, &last_space);
    if (last_good < value.size()) {
        const char* suffix = "...";
        const size_t suffix_len = 3;
        size_t cut = last_good;
        if (max_bytes > suffix_len && last_space != std::string::npos &&
            last_space >= max_bytes / 2 && last_space + suffix_len <= max_bytes) {
            cut = last_space;
        } else if (max_bytes > suffix_len) {
            cut = Utf8PrefixLen(value, max_bytes - suffix_len, nullptr);
        }
        value.resize(cut);
        TrimAsciiWhitespace(value);
        if (max_bytes > suffix_len && !value.empty()) value += suffix;
    }
}

void NormalizeCaptionLine(std::string& value) {
    TrimAsciiWhitespace(value);
    size_t write = 0;
    bool in_space = false;
    for (char ch : value) {
        if (IsAsciiWhitespace(ch)) {
            if (!in_space && write > 0) value[write++] = ' ';
            in_space = true;
        } else {
            value[write++] = ch;
            in_space = false;
        }
    }
    value.resize(write);
    TruncateUtf8(value, 96);  // fit the 480px lesson caption line
}

std::string NormalizeAsciiToken(const char* value) {
    if (value == nullptr) return "";
    std::string out(value);
    TrimAsciiWhitespace(out);
    for (char& ch : out) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return out;
}

bool IsValidUtf8String(const std::string& value) {
    return Utf8PrefixLen(value, value.size(), nullptr) == value.size();
}

std::string CaptionCandidate(const char* value) {
    if (Blank(value)) return "";
    std::string out(value);
    if (!IsValidUtf8String(out)) return "";
    NormalizeCaptionLine(out);
    return out;
}

bool LooksLikeChildQuestionCaption(const std::string& value) {
    if (value.find('?') != std::string::npos) return true;
    std::string lower(value);
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (lower.rfind("please ", 0) == 0) lower.erase(0, 7);
    return lower.rfind("say ", 0) == 0 ||
           lower.rfind("try saying ", 0) == 0 ||
           lower.rfind("try to say ", 0) == 0 ||
           lower.rfind("can you ", 0) == 0 ||
           lower.find("you can say ") != std::string::npos ||
           lower.rfind("tell me ", 0) == 0 ||
           lower.rfind("repeat ", 0) == 0;
}

bool Num(const cJSON* o, const char* k, double& out) {
    if (o == nullptr) return false;
    const cJSON* v = cJSON_GetObjectItem(o, k);
    if (!cJSON_IsNumber(v)) return false;
    out = v->valuedouble;
    return true;
}
constexpr double kMaxJsonSafeInteger = 9007199254740991.0;

bool PositiveIntegerAtMost(double value, double maximum) {
    return std::isfinite(value) && value > 0 && value <= kMaxJsonSafeInteger &&
           value <= maximum && std::trunc(value) == value;
}
const cJSON* Obj(const cJSON* o, const char* k) {
    if (o == nullptr) return nullptr;
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsObject(v) ? v : nullptr;
}

bool ExactObjectKeys(const cJSON* object, const std::set<std::string_view>& expected) {
    if (!cJSON_IsObject(object)) return false;
    size_t count = 0;
    for (const cJSON* item = object->child; item != nullptr; item = item->next) {
        if (item->string == nullptr || expected.find(item->string) == expected.end()) return false;
        ++count;
    }
    return count == expected.size();
}

bool ExactPositiveInteger(const cJSON* object, const char* key) {
    double value = 0;
    return Num(object, key, value) && std::isfinite(value) && value > 0 &&
           value <= 4294967295.0 && std::floor(value) == value;
}

bool ExactUint64(const cJSON* object, const char* key, std::uint64_t* out) {
    double value = 0;
    if (!Num(object, key, value) || !std::isfinite(value) || value <= 0 ||
        std::floor(value) != value || value > 9007199254740991.0) {
        return false;
    }
    if (out != nullptr) *out = static_cast<std::uint64_t>(value);
    return true;
}

bool ExactString(const cJSON* object, const char* key, const char* expected) {
    const char* value = Str(object, key);
    return value != nullptr && std::strcmp(value, expected) == 0;
}

bool ValidOpeningEntrance(const cJSON* entrance) {
    static const std::set<std::string_view> kKeys = {
        "preset", "policy", "layoutPreset", "backgroundAssetKey", "robotAssetKey", "fallback",
    };
    const char* layout = Str(entrance, "layoutPreset");
    return ExactObjectKeys(entrance, kKeys) &&
           ExactString(entrance, "preset", "flyLandWalkGreet") &&
           ExactString(entrance, "policy", "oncePerLessonSession") &&
           layout != nullptr &&
           (std::strcmp(layout, "centerRoad") == 0 ||
            std::strcmp(layout, "leftApproach") == 0 ||
            std::strcmp(layout, "rightApproach") == 0) &&
           !Blank(Str(entrance, "backgroundAssetKey")) &&
           !Blank(Str(entrance, "robotAssetKey")) &&
           ExactString(entrance, "fallback", "staticGreet");
}

bool ValidVisualStateContract(const cJSON* root, std::uint64_t* visual_generation) {
    static const std::set<std::string_view> kKeys = {
        "state", "overlayKey", "motionPreset", "visualGeneration",
    };
    static const std::set<std::string_view> kStates = {
        "teach", "listen", "thinking", "correct", "nearMiss", "incorrect",
        "retry", "celebrate", "completion",
    };
    const cJSON* body = Obj(root, "body");
    const char* state = Str(body, "state");
    return ExactObjectKeys(body, kKeys) && state != nullptr &&
           kStates.find(state) != kStates.end() && !Blank(Str(body, "overlayKey")) &&
           !Blank(Str(body, "motionPreset")) &&
           ExactUint64(body, "visualGeneration", visual_generation) &&
           IsValidLessonIdentity(Str(root, "stepId"));
}

struct StaticVisualStatePresentation {
    const char* emotion;
    const char* status;
};

StaticVisualStatePresentation VisualStatePresentation(const char* state) {
    if (std::strcmp(state, "teach") == 0) return {"happy", "Đang học..."};
    if (std::strcmp(state, "listen") == 0) return {"thinking", "Con nói nhé..."};
    if (std::strcmp(state, "thinking") == 0) return {"thinking", "Đang suy nghĩ..."};
    if (std::strcmp(state, "correct") == 0) return {"happy", "Chính xác!"};
    if (std::strcmp(state, "nearMiss") == 0) return {"neutral", "Gần đúng rồi!"};
    if (std::strcmp(state, "incorrect") == 0) return {"sad", "Chưa đúng"};
    if (std::strcmp(state, "retry") == 0) return {"thinking", "Thử lại nhé!"};
    if (std::strcmp(state, "celebrate") == 0) return {"happy", "Tuyệt vời!"};
    return {"happy", "Hoàn thành bài học"};
}

LessonVisualStateKind VisualStateKind(const char* state) {
    if (std::strcmp(state, "listen") == 0) return LessonVisualStateKind::kListen;
    if (std::strcmp(state, "thinking") == 0) return LessonVisualStateKind::kThinking;
    if (std::strcmp(state, "correct") == 0) return LessonVisualStateKind::kCorrect;
    if (std::strcmp(state, "nearMiss") == 0) return LessonVisualStateKind::kNearMiss;
    if (std::strcmp(state, "incorrect") == 0) return LessonVisualStateKind::kIncorrect;
    if (std::strcmp(state, "retry") == 0) return LessonVisualStateKind::kRetry;
    if (std::strcmp(state, "celebrate") == 0) return LessonVisualStateKind::kCelebrate;
    if (std::strcmp(state, "completion") == 0) return LessonVisualStateKind::kCompletion;
    return LessonVisualStateKind::kTeach;
}

LessonVisualCompletionResult QueueCompletionResult(LessonVisualApplyResult result) {
    switch (result) {
    case LessonVisualApplyResult::kApplied: return LessonVisualCompletionResult::kApplied;
    case LessonVisualApplyResult::kDegraded: return LessonVisualCompletionResult::kDegraded;
    case LessonVisualApplyResult::kRejected: return LessonVisualCompletionResult::kRejected;
    case LessonVisualApplyResult::kPhaseTimeout: return LessonVisualCompletionResult::kPhaseTimeout;
    }
    return LessonVisualCompletionResult::kRejected;
}

const char* StableVisualDegradedReason(const char* value) {
    if (value == nullptr) return nullptr;
    for (const char* allowed : {
             "missingOverlay", "animationStartFailed", "phaseTimeout", "reducedMotion",
             "unsupportedContract", "assetIdentityMismatch", "insufficientHeap", "superseded"}) {
        if (std::strcmp(value, allowed) == 0) return allowed;
    }
    return nullptr;
}

bool IsSha256(const char* value) {
    if (value == nullptr || std::strlen(value) != 64) return false;
    for (const char* ch = value; *ch != '\0'; ++ch) {
        if (!((*ch >= '0' && *ch <= '9') || (*ch >= 'a' && *ch <= 'f') ||
              (*ch >= 'A' && *ch <= 'F'))) return false;
    }
    return true;
}

bool IsStaticImageMediaType(const char* media_type) {
    if (media_type == nullptr) return false;
    return std::strcmp(media_type, "image/png") == 0 ||
           std::strcmp(media_type, "image/jpeg") == 0;
}

bool ValidPinnedAsset(const cJSON* asset, bool require_png) {
    static const std::set<std::string_view> kKeys = {
        "versionId", "sha256", "bytes", "mediaType",
    };
    const char* media_type = Str(asset, "mediaType");
    const bool media_valid = require_png
        ? (media_type != nullptr && std::strcmp(media_type, "image/png") == 0)
        : IsStaticImageMediaType(media_type);
    return ExactObjectKeys(asset, kKeys) && !Blank(Str(asset, "versionId")) &&
           IsSha256(Str(asset, "sha256")) && ExactPositiveInteger(asset, "bytes") && media_valid;
}  // GCOVR_EXCL_LINE: closing brace has no executable statement.

bool MatchesPinnedAsset(const cJSON* scene_asset, const cJSON* pinned_asset) {
    const char* scene_key = Str(scene_asset, "key");
    const char* scene_sha = Str(scene_asset, "sha256");
    return scene_key != nullptr && scene_sha != nullptr &&
           std::strcmp(scene_key, Str(pinned_asset, "versionId")) == 0 &&
           std::strcmp(scene_sha, Str(pinned_asset, "sha256")) == 0;
}

bool HasForbiddenMotionExtension(const char* source) {
    if (source == nullptr) return false;
    std::string lower(source);
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    const size_t suffix_end = lower.find_first_of("?#");
    if (suffix_end != std::string::npos) lower.resize(suffix_end);
    for (const char* suffix : {".gif", ".webm", ".mov", ".mp4"}) {
        const size_t length = std::strlen(suffix);
        if (lower.size() >= length && lower.compare(lower.size() - length, length, suffix) == 0) return true;
    }
    return false;
}

bool HasForbiddenMotionMedia(const cJSON* asset) {
    if (asset == nullptr) return false;
    const char* media_type = Str(asset, "mediaType");
    if (media_type == nullptr) media_type = Str(asset, "mimeType");
    if (media_type == nullptr) media_type = Str(asset, "contentType");
    std::string normalized_media = media_type != nullptr ? media_type : "";
    TrimAsciiWhitespace(normalized_media);
    for (char& ch : normalized_media) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return HasForbiddenMotionExtension(Str(asset, "src")) ||
           normalized_media.rfind("video/", 0) == 0 || normalized_media == "image/gif";
}

bool ValidTvideoProjection(const cJSON* projection) {
    static const std::set<std::string_view> kProjectionKeys = {
        "templateId", "templateVersion", "layoutPreset", "geometryVersion", "phases",
        "revealPhase", "fallbackPolicy", "background", "arrivedPose",
    };
    static const std::set<std::string_view> kProjectionKeysWithAtlas = {
        "templateId", "templateVersion", "layoutPreset", "geometryVersion", "phases",
        "revealPhase", "fallbackPolicy", "background", "arrivedPose", "atlas",
    };
    const cJSON* atlas = Obj(projection, "atlas");
    if (!ExactObjectKeys(projection, atlas != nullptr ? kProjectionKeysWithAtlas : kProjectionKeys)) return false;
    double template_version = 0;
    double geometry_version = 0;
    uint8_t template_version_u8 = 0;
    uint8_t geometry_version_u8 = 0;
    const char* reveal_phase = Str(projection, "revealPhase");
    const char* fallback_policy = Str(projection, "fallbackPolicy");
    if (!Num(projection, "templateVersion", template_version) ||
        !Num(projection, "geometryVersion", geometry_version) ||
        !lesson_tvideo::ExactVersion(template_version, &template_version_u8) ||
        !lesson_tvideo::ExactVersion(geometry_version, &geometry_version_u8) ||
        !lesson_tvideo::IsSupported(Str(projection, "templateId"), template_version_u8,
                                    Str(projection, "layoutPreset"), geometry_version_u8) ||
        reveal_phase == nullptr || std::strcmp(reveal_phase, "revealTeachingContent") != 0 ||
        fallback_policy == nullptr || std::strcmp(fallback_policy, "snapToArriveNearAndReveal") != 0 ||
        !ValidPinnedAsset(Obj(projection, "background"), false) ||
        !ValidPinnedAsset(Obj(projection, "arrivedPose"), true) ||
        (atlas != nullptr && !ValidPinnedAsset(atlas, true))) return false;
    const cJSON* phases = cJSON_GetObjectItem(projection, "phases");
    if (!cJSON_IsArray(phases) || cJSON_GetArraySize(phases) != lesson_tvideo::PhaseCount()) return false;
    static const std::set<std::string_view> kPhaseKeys = {"name", "durationMs"};
    for (uint8_t index = 0; index < lesson_tvideo::PhaseCount(); ++index) {
        const cJSON* phase = cJSON_GetArrayItem(phases, index);
        double duration = 0;
        if (!ExactObjectKeys(phase, kPhaseKeys) ||
            Str(phase, "name") == nullptr ||
            std::strcmp(Str(phase, "name"), lesson_tvideo::PhaseName(index)) != 0 ||
            !Num(phase, "durationMs", duration) ||
            duration != lesson_tvideo::PhaseDurationMs(index)) return false;
    }
    return true;
}

// Copy an identity field from the inbound frame to an outbound frame verbatim,
// preserving its JSON type (string stays string; number stays number — D-LV keeps
// lessonVersion a NUMBER on the wire).
void CopyStr(cJSON* dst, const cJSON* src, const char* k) {
    const char* v = Str(src, k);
    if (v != nullptr) LessonJsonAddString(dst, k, v);
}
void CopyNum(cJSON* dst, const cJSON* src, const char* k) {
    double v = 0.0;
    if (Num(src, k, v)) LessonJsonAddNumber(dst, k, v);
}

int64_t NowMs() { return static_cast<int64_t>(time(nullptr)) * 1000LL; }

// ---- single active lesson session (slice-01: one assignment at a time) -------
// HandleLessonMessage runs on the dedicated lesson_worker task, so lesson frames
// are processed sequentially and this state needs no lock. Draws are marshalled to
// the app task via Application::Schedule(). The F->S sequence is the only
// firmware-owned wire state (shared by acks + progress, monotonic per
// (assignmentId,sessionId), starting at 1 — fixture sequenceStreams F->S).
struct LessonSession {
    struct AckReplay {
        int64_t sequence = 0;
        std::string body_json;
    };
    static constexpr size_t kAckReplayWindow = 16;
    std::string assignment_id;
    std::string session_id;
    std::uint64_t lesson_asset_generation = 0;
    double      assignment_version = -1.0;  // staleness guard (plan §7.2)
    int64_t     last_in_sequence   = 0;     // highest processed S->F sequence (0 = none)
    int64_t     fs_sequence        = 0;     // firmware F->S counter, pre-inc on emit
    bool        prepared           = false;
    bool        running            = false;
    bool        paused             = false;
    bool        first_lesson_step_seen = false;
    bool        motion_presets_enabled = false;
    bool        tvideo_entrance_consumed = false;
    bool        renderer_v2 = false;
    std::string cinematic_renderer_id;
    bool        opening_entrance_consumed = false;
    bool        entrance_active = false;
    std::uint64_t visual_generation = 0;
    std::uint64_t pending_visual_nonce = 0;
    std::uint64_t current_transport_epoch = 0;
    int64_t     pending_server_sequence = 0;
    std::string pending_protocol_version;
    std::string pending_lesson_id;
    double      pending_lesson_version = 0;
    bool        pending_has_lesson_version = false;
    std::string pending_step_id;
    bool        pending_ack = false;
    std::map<std::string, std::string> verified_asset_paths;
    // FW-LESSON-02: the (rendered, degraded) of the ack we emitted for the last
    // processed inbound sequence, so a duplicate re-ack can REPLAY the prior ack body
    // idempotently (protocol §6 / lesson-robot-protocol.md:436-438) instead of
    // hardcoding rendered=false/degraded=false. Slice-01 has a single active step, so
    // caching the last ack's flags keyed on its sequence is sufficient.
    int64_t     last_ack_sequence  = 0;     // S->F sequence this cached ack acked (0 = none)
    bool        last_ack_rendered  = false;
    bool        last_ack_degraded  = false;
    std::string last_ack_asset_pack_json;
    std::string last_ack_degraded_reason;
    int64_t     prepare_ack_sequence = 0;   // prepare assetPack replay survives later lifecycle acks
    std::string prepare_ack_asset_pack_json;
    std::string last_ack_telemetry_json;
    std::string last_ack_body_json;
    std::deque<AckReplay> ack_history;
};
LessonSession g_session;
LessonLayerState g_layer_state;
std::atomic<std::uint64_t> g_visual_callback_token{0};
std::atomic<std::uint64_t> g_visual_completion_nonce{0};

std::uint64_t NextVisualCompletionNonce() {
    std::uint64_t nonce =
        g_visual_completion_nonce.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (nonce == 0) {
        nonce = g_visual_completion_nonce.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    return nonce;
}

#ifdef TBOT_HOST_NATIVE_COVERAGE
}  // namespace
namespace tbot {
void SetLessonVisualCompletionNonceForTest(std::uint64_t value) {
    g_visual_completion_nonce.store(value, std::memory_order_relaxed);
}
}  // namespace tbot
namespace {
#endif

void ClearTerminalLessonCursor() {
    // A completed/failed lesson is terminal. The server may reuse the same assignmentId
    // / sessionId for the next sample lesson and restart S->F sequence at 1; clear the
    // inbound cursor so fresh prepare is not treated as an old duplicate.
    g_session.last_in_sequence = 0;
    g_session.last_ack_sequence = 0;
    g_session.last_ack_rendered = false;
    g_session.last_ack_degraded = false;
    g_session.last_ack_asset_pack_json.clear();
    g_session.last_ack_degraded_reason.clear();
    g_session.prepare_ack_sequence = 0;
    g_session.prepare_ack_asset_pack_json.clear();
    g_session.last_ack_telemetry_json.clear();
    g_session.last_ack_body_json.clear();
    g_session.ack_history.clear();
    g_session.assignment_version = -1.0;
}

// Build an outbound F->S frame. Envelope identity (protocolVersion/assignmentId/
// sessionId/lessonId/lessonVersion/stepId) is echoed verbatim from the inbound
// frame; the firmware owns only type/sequence/timestamp/body. `body` is consumed.
std::string BuildFrame(const cJSON* in, const char* type, int64_t fs_seq, cJSON* body) {
    cJSON* root = LessonJsonCreateObject();
    if (root == nullptr || body == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(body);
        return "";
    }
    LessonJsonAddString(root, "type", type);
    CopyStr(root, in, "protocolVersion");
    CopyStr(root, in, "assignmentId");
    CopyStr(root, in, "sessionId");
    CopyStr(root, in, "lessonId");
    CopyNum(root, in, "lessonVersion");                 // NUMBER on the wire (D-LV)
    const char* step_id = Str(in, "stepId");            // echoed: "s4" on steps, null on lifecycle
    if (step_id != nullptr) LessonJsonAddString(root, "stepId", step_id);
    else                    LessonJsonAddNull(root, "stepId");
    LessonJsonAddNumber(root, "sequence", static_cast<double>(fs_seq));
    LessonJsonAddNumber(root, "timestamp", static_cast<double>(NowMs()));
    if (!LessonJsonAttachItem(root, "body", body)) {
        cJSON_Delete(body);
        cJSON_Delete(root);
        return "";
    }
    if (cJSON_GetObjectItem(root, "type") == nullptr ||
        cJSON_GetObjectItem(root, "protocolVersion") == nullptr ||
        cJSON_GetObjectItem(root, "assignmentId") == nullptr ||
        cJSON_GetObjectItem(root, "sessionId") == nullptr ||
        cJSON_GetObjectItem(root, "stepId") == nullptr ||
        cJSON_GetObjectItem(root, "sequence") == nullptr ||
        cJSON_GetObjectItem(root, "timestamp") == nullptr ||
        cJSON_GetObjectItem(root, "body") == nullptr) {
        cJSON_Delete(root);
        return "";
    }
    char* s = LessonJsonPrintUnformatted(root);
    std::string out = (s != nullptr) ? std::string(s) : std::string();
    if (s != nullptr) cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

const char* CanonicalRobotState(const char* value) {
    if (value == nullptr) return nullptr;
    for (const char* allowed : {"talking", "modeling", "listening", "thinking", "celebrating"}) {
        if (strcmp(value, allowed) == 0) return allowed;
    }
    return nullptr;
}

std::string SafeEvidenceToken(const char* value, bool allow_slash) {
    if (Blank(value)) return "-";
    std::string token(value);
    if (token.size() > 160) return "invalid";
    for (unsigned char ch : token) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                          ch == '.' || ch == ':' || ch == '~' || (allow_slash && ch == '/');
        if (!safe) return "invalid";
    }
    return token;
}

void LogLessonAckEvidence(const cJSON* in, const cJSON* ack_body) {
    double acked_d = -1.0;
    double render_elapsed_d = -1.0;
    Num(ack_body, "acks", acked_d);
    Num(ack_body, "renderElapsedMs", render_elapsed_d);
    const bool rendered = cJSON_IsTrue(cJSON_GetObjectItem(ack_body, "rendered"));
    const bool degraded = cJSON_IsTrue(cJSON_GetObjectItem(ack_body, "degraded"));
    const char* robot_state = CanonicalRobotState(Str(ack_body, "robotState"));
    const cJSON* asset_pack = Obj(ack_body, "assetPack");
    const bool asset_pack_ready = cJSON_IsTrue(cJSON_GetObjectItem(asset_pack, "ready"));
    const char* cache_key = Str(asset_pack, "cacheKey");

    std::string evidence =
        "assignmentId=" + SafeEvidenceToken(Str(in, "assignmentId"), false) +
        " sessionId=" + SafeEvidenceToken(Str(in, "sessionId"), false) +
        " stepId=" + SafeEvidenceToken(Str(in, "stepId"), false) +
        " body.acks=" + std::to_string(static_cast<long>(acked_d)) +
        " rendered=" + (rendered ? "true" : "false") +
        " degraded=" + (degraded ? "true" : "false");
    if (robot_state != nullptr) {
        evidence += " robotState=";
        evidence += robot_state;
    }
    evidence += " renderElapsedMs=" + std::to_string(static_cast<long>(render_elapsed_d));
    if (asset_pack != nullptr) {
        evidence += " assetPack.ready=";
        evidence += asset_pack_ready ? "true" : "false";
        evidence += " cacheKey=" + SafeEvidenceToken(cache_key, true);
    }
    ESP_LOGI(TAG, "lesson_ack TX %s", evidence.c_str());
}

cJSON* MakeErrorBody(const char* code, const char* message, bool retryable, const char* reason) {
    cJSON* b = LessonJsonCreateObject();
    cJSON* ctx = LessonJsonCreateObject();
    if (b == nullptr || ctx == nullptr || LessonJsonAddString(b, "code", code) == nullptr ||
        LessonJsonAddString(b, "message", message) == nullptr ||
        LessonJsonAddBool(b, "retryable", retryable) == nullptr ||
        LessonJsonAddString(ctx, "reason", reason) == nullptr ||
        !LessonJsonAttachItem(b, "context", ctx)) {
        cJSON_Delete(ctx);
        cJSON_Delete(b);
        return nullptr;
    }
    return b;
}

// FW-01 / FW-LESSON-01 — per-step completion class. The frozen contract splits the 9
// authorable step types into two classes ON THE WIRE (fixture multiStep._meta
// .completionClasses; multiStepThread steps 7-10): a PASSIVE narration step gets an
// ack ONLY and auto-advances on that ack, while an INTERACTIVE step gets render ack
// plus a listening window. Child response completion is owned by the transcript/tap
// response path, so the renderer must never fabricate lesson_progress from draw
// success. The old unconditional step_completed emission contaminated the ESP latch
// (runtime.py:80-88, latch-contamination guard runtime.py:284-300) and could skip
// steps (test_lesson_runtime.py:979).
//
// Classifier mirrors the ESP _is_passive_step (runtime.py:90-111) EXACTLY so both
// sides agree: explicit body.completionClass ('passive'|'interactive') is authoritative
// when present; otherwise fall back to PASSIVE_STEP_TYPES membership on body.stepType.
// This is NOT a contract change — it removes a wire emission the fixture says should
// never have been sent — so it needs NO renderer-version bump (the protocolVersion
// identity teebot-lesson-renderer.v1 and the served-manifest negotiation are unchanged).
bool IsPassiveStepType(const char* step_type) {
    if (step_type == nullptr) return false;  // unknown type -> treat as interactive
    if (strcmp(step_type, "greeting") == 0 ||
        strcmp(step_type, "review")   == 0 ||
        strcmp(step_type, "focus")    == 0 ||
        strcmp(step_type, "feedback") == 0 ||
        strcmp(step_type, "celebrate") == 0) {
        return true;
    }
    const std::string type = NormalizeAsciiToken(step_type);
    return type == "GREETING" ||
           type == "REVIEW" ||
           type == "FOCUS" ||
           type == "FEEDBACK" ||
           type == "CELEBRATE";
}
bool IsPassiveStep(const char* completion_class, const char* step_type) {
    const std::string completion = NormalizeAsciiToken(completion_class);
    if (!completion.empty()) {
        if (completion == "PASSIVE")     return true;
        if (completion == "INTERACTIVE") return false;
        // unknown completionClass -> fall through to the type-set fallback below.
    }
    return IsPassiveStepType(step_type);
}

// (Removed AssetAvailable(): a presence-only flashed-asset probe that previously fed the
// poster_drew rung. It only checked GetAssetData() presence and discarded the bytes, never
// decoding nor calling SetLessonBackground(), so a flashed-only poster was acked as drawn
// without any real draw. The renderer now draws exclusively through FetchLessonImage()
// (HTTP fetch or verified SD pack path), so the presence-only probe is dead and removed.)

bool IsJpegImage(const void* data, size_t size) {
    if (data == nullptr || size < 3) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    return bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
}

#ifndef TBOT_HOST_NATIVE_COVERAGE
bool IsPngImage(const void* data, size_t size) {
    if (data == nullptr || size < 8) return false;
    static constexpr uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    return memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0;
}
#endif

constexpr size_t kMaxLessonImageBytes = 512 * 1024;
constexpr size_t kMaxLessonDecodedImageBytes = 480 * 320 * 4;
constexpr size_t kMaxLessonAssetPackAssets = 64;
constexpr size_t kMaxLessonAssetPackDeclaredBytes = 4 * 1024 * 1024;
constexpr int kLessonImageHttpTimeoutMs = 1200;
constexpr int64_t kLessonRenderElapsedMaxMs = 60000;

bool LessonDecodedImageFitsBudget(size_t decoded_len, size_t width, size_t height, size_t stride) {
    if (decoded_len == 0 || width == 0 || height == 0 || stride == 0) return false;
    if (width > SIZE_MAX / 2) return false;
    if (stride < width * 2) return false;
    if (height > SIZE_MAX / stride) return false;
    const size_t footprint = stride * height;
    if (footprint == 0 || decoded_len < footprint) return false;
    return footprint <= kMaxLessonDecodedImageBytes && decoded_len <= kMaxLessonDecodedImageBytes;
}

#ifndef TBOT_HOST_NATIVE_COVERAGE
thread_local bool g_lesson_png_allocator_active = false;

extern "C" void* lodepng_malloc(size_t size) {
    return g_lesson_png_allocator_active
        ? heap_caps_malloc(size, LessonAllocationCaps(size))
        : lv_malloc(size);
}

extern "C" void* lodepng_realloc(void* pointer, size_t size) {
    return g_lesson_png_allocator_active
        ? heap_caps_realloc(pointer, size, LessonAllocationCaps(size))
        : lv_realloc(pointer, size);
}

extern "C" void lodepng_free(void* pointer) {
    if (g_lesson_png_allocator_active) heap_caps_free(pointer);
    else lv_free(pointer);
}

void* LessonPngBufferMalloc(size_t size, lv_color_format_t) {
    size += LV_DRAW_BUF_ALIGN - 1;
    return heap_caps_malloc(size, LessonAllocationCaps(size));
}

void LessonPngBufferFree(void* buffer) {
    heap_caps_free(buffer);
}

class LessonPngAllocatorScope {
public:
    LessonPngAllocatorScope() {
        lv_lock();
        handlers_ = lv_draw_buf_get_image_handlers();
        previous_malloc_ = handlers_->buf_malloc_cb;
        previous_free_ = handlers_->buf_free_cb;
        handlers_->buf_malloc_cb = LessonPngBufferMalloc;
        handlers_->buf_free_cb = LessonPngBufferFree;
        g_lesson_png_allocator_active = true;
    }
    ~LessonPngAllocatorScope() {
        g_lesson_png_allocator_active = false;
        handlers_->buf_malloc_cb = previous_malloc_;
        handlers_->buf_free_cb = previous_free_;
        lv_unlock();
    }
private:
    lv_draw_buf_handlers_t* handlers_ = nullptr;
    lv_draw_buf_malloc_cb_t previous_malloc_ = nullptr;
    lv_draw_buf_free_cb_t previous_free_ = nullptr;
};

std::unique_ptr<LvglImage> DecodeLessonPngBytes(char* compressed, size_t compressed_size,
                                                const char* log_prefix) {
    LessonPngAllocatorScope allocator_scope;
    lv_image_dsc_t source{};
    source.header.magic = LV_IMAGE_HEADER_MAGIC;
    source.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    source.data = reinterpret_cast<const uint8_t*>(compressed);
    source.data_size = compressed_size;

    lv_image_decoder_args_t args{};
    args.no_cache = true;
    lv_image_decoder_dsc_t decoder{};
    const lv_result_t result = lv_image_decoder_open(&decoder, &source, &args);
    if (result != LV_RESULT_OK || decoder.decoded == nullptr) {
        ESP_LOGW(TAG, "%s: PNG decode failed", log_prefix);
        heap_caps_free(compressed);
        return nullptr;
    }

    const lv_draw_buf_t* decoded = decoder.decoded;
    const size_t decoded_size = decoded->data_size;
    if (!LessonDecodedImageFitsBudget(decoded_size, decoded->header.w, decoded->header.h,
                                      decoded->header.stride)) {
        lv_image_decoder_close(&decoder);
        heap_caps_free(compressed);
        return nullptr;
    }
    uint8_t* psram = nullptr;
    if (decoded->data == decoded->unaligned_data) {
        psram = decoded->data;
        auto* mutable_decoded = const_cast<lv_draw_buf_t*>(decoded);
        mutable_decoded->data = nullptr;
        mutable_decoded->unaligned_data = nullptr;
        mutable_decoded->data_size = 0;
    } else {
        psram = static_cast<uint8_t*>(
            heap_caps_malloc(decoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (psram == nullptr) {
            lv_image_decoder_close(&decoder);
            heap_caps_free(compressed);
            return nullptr;
        }
        memcpy(psram, decoded->data, decoded_size);
    }
    const int width = static_cast<int>(decoded->header.w);
    const int height = static_cast<int>(decoded->header.h);
    const int stride = static_cast<int>(decoded->header.stride);
    const int color_format = decoded->header.cf;
    lv_image_decoder_close(&decoder);  // no_cache frees LVGL's temporary decoded buffer
    heap_caps_free(compressed);         // compressed PNG is transient only
    return std::make_unique<LvglAllocatedImage>(psram, decoded_size, width, height, stride,
                                                color_format);
}
#endif

std::unique_ptr<LvglImage> DecodeLessonImageBytes(char* data, size_t content_length,
                                                  const char* log_prefix) {
    if (data == nullptr || content_length == 0) return nullptr;
#ifndef TBOT_HOST_NATIVE_COVERAGE
    if (IsPngImage(data, content_length)) {
        return DecodeLessonPngBytes(data, content_length, log_prefix);
    }
#endif
    if (IsJpegImage(data, content_length)) {
        uint8_t* decoded_data = nullptr;
        size_t decoded_len = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;

        esp_err_t ret = jpeg_to_image_with_caps(
            reinterpret_cast<const uint8_t*>(data), content_length, &decoded_data, &decoded_len,
            &width, &height, &stride, LessonAllocationCaps(content_length));
        heap_caps_free(data);
        data = nullptr;

        if (ret != ESP_OK || decoded_data == nullptr ||
            !LessonDecodedImageFitsBudget(decoded_len, width, height, stride)) {
            ESP_LOGW(TAG, "%s: JPEG decode failed: %d", log_prefix, (int)ret);
            if (decoded_data != nullptr) heap_caps_free(decoded_data);
            return nullptr;
        }

        try {
            return std::make_unique<LvglAllocatedImage>(decoded_data, decoded_len,
                                                        static_cast<int>(width),
                                                        static_cast<int>(height),
                                                        static_cast<int>(stride),
                                                        LV_COLOR_FORMAT_RGB565);
        } catch (...) {
            ESP_LOGW(TAG, "%s: decoded JPEG rejected; skipping", log_prefix);
            heap_caps_free(decoded_data);
            return nullptr;
        }
    }  // GCOVR_EXCL_LINE: every JPEG branch path returns before this structural brace.

    // LvglAllocatedImage takes ownership of `data` (frees it in its dtor when the
    // cached unique_ptr is replaced/reset). Its ctor THROWS std::runtime_error if
    // LVGL can't decode the bytes (truncated/unsupported image, HTML error page
    // served as 200, etc.) and does NOT free data on throw — catch it so an
    // undecodable poster is a non-fatal skip instead of an exception escaping the
    // lesson_worker task (deep-audit #1 CRITICAL). catch(...) avoids any <exception>
    // include dependency.
    try {
        return std::make_unique<LvglAllocatedImage>(data, content_length);
    } catch (...) {
        ESP_LOGW(TAG, "%s: undecodable image; skipping", log_prefix);
        heap_caps_free(data);
        return nullptr;
    }
}

constexpr const char* kLessonAssetPackRoot = "/sdcard/tbot/lesson-assets/";

std::string LessonPackFileSystemPath(const std::string& canonical_path) {
#ifdef TBOT_HOST_NATIVE_COVERAGE
    const char* host_root_env = std::getenv("TBOT_HOST_LESSON_ASSET_ROOT");
    if (!Blank(host_root_env) && canonical_path.rfind(kLessonAssetPackRoot, 0) == 0) {
        std::string host_root(host_root_env);
        if (!host_root.empty() && host_root.back() != '/') host_root.push_back('/');
        return host_root + canonical_path.substr(strlen(kLessonAssetPackRoot));
    }
#endif
    return canonical_path;
}

std::string ValidateLessonPackPath(std::string path) {
    if (path.empty()) return "";
    if (path.find("..") != std::string::npos || path.find("\\") != std::string::npos) {
        ESP_LOGW(TAG, "lesson image file: rejected unsafe local path");
        return "";
    }
    if (path.rfind(kLessonAssetPackRoot, 0) != 0) {
        ESP_LOGW(TAG, "lesson image file: rejected non-pack local path");
        return "";
    }
    return path;
}

std::string LessonLocalPath(const char* url) {
    if (url == nullptr) return "";
    std::string local_url(url);
    TrimAsciiWhitespace(local_url);
    if (local_url.empty()) return "";
    const char* canonical_url = local_url.c_str();
    if (strncmp(canonical_url, "sd://", 5) == 0) {
        const char* tail = canonical_url + 5;
        if (strncmp(tail, "sdcard/", 7) == 0) return ValidateLessonPackPath(std::string("/") + tail);
        if (tail[0] == '/') return ValidateLessonPackPath(tail);
        return ValidateLessonPackPath(std::string("/sdcard/") + tail);
    }
    if (strncmp(canonical_url, "file://", 7) == 0) {
        return ValidateLessonPackPath(std::string(canonical_url + 7));
    }
    return "";
}

std::unique_ptr<LvglImage> FetchLessonLocalImage(const char* url) {
    std::string path = LessonLocalPath(url);
    if (path.empty()) return nullptr;
    std::string fs_path = LessonPackFileSystemPath(path);

    FILE* fp = fopen(fs_path.c_str(), "rb");
    if (fp == nullptr) {
        ESP_LOGW(TAG, "lesson image file: open failed %s", path.c_str());
        return nullptr;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return nullptr;
    }
    long file_size = ftell(fp);
    if (file_size <= 0 || static_cast<size_t>(file_size) > kMaxLessonImageBytes) {
        ESP_LOGW(TAG, "lesson image file: invalid size %ld", file_size);
        fclose(fp);
        return nullptr;
    }
    rewind(fp);

    size_t content_length = static_cast<size_t>(file_size);
    char* data = static_cast<char*>(heap_caps_malloc(content_length, LessonAllocationCaps(content_length)));
    if (data == nullptr) {
        ESP_LOGW(TAG, "lesson image file: alloc %u failed", (unsigned)content_length);
        fclose(fp);
        return nullptr;
    }
    size_t read = fread(data, 1, content_length, fp);
    fclose(fp);
    if (read != content_length) {
        ESP_LOGW(TAG, "lesson image file: short read %u/%u", (unsigned)read, (unsigned)content_length);
        heap_caps_free(data);
        return nullptr;
    }
    return DecodeLessonImageBytes(data, content_length, "lesson image file");
}

bool LessonLocalFileReady(const char* url, size_t expected_size = 0) {
    std::string path = LessonLocalPath(url);
    if (path.empty()) return false;
    std::string fs_path = LessonPackFileSystemPath(path);
    FILE* fp = fopen(fs_path.c_str(), "rb");
    if (fp == nullptr) return false;
    bool ready = false;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long file_size = ftell(fp);
        ready = file_size > 0 && static_cast<size_t>(file_size) <= kMaxLessonImageBytes;
        if (ready && expected_size > 0) {
            ready = static_cast<size_t>(file_size) == expected_size;
        }
    }
    fclose(fp);
    return ready;
}

cJSON* BuildAssetPackAck(const cJSON* body) {
    const cJSON* pack = Obj(body, "assetPack");
    if (pack == nullptr) return nullptr;

    const char* cache_key = Str(pack, "cacheKey");
    std::string cache_key_value = cache_key == nullptr ? "" : cache_key;
    TrimAsciiWhitespace(cache_key_value);
    const cJSON* manifest_ref = Obj(body, "manifestRef");
    const char* manifest_checksum = Str(manifest_ref, "manifestChecksum");
    std::string manifest_checksum_value = manifest_checksum == nullptr ? "" : manifest_checksum;
    TrimAsciiWhitespace(manifest_checksum_value);
    const bool manifest_checksum_required = !manifest_checksum_value.empty();
    const bool cache_key_has_manifest_checksum =
        manifest_checksum_required && !cache_key_value.empty() &&
        strstr(cache_key_value.c_str(), manifest_checksum_value.c_str()) != nullptr;
    const cJSON* assets = cJSON_GetObjectItem(pack, "assets");
    const char* local_root = Str(pack, "localRoot");
    std::string local_root_value = local_root == nullptr ? "" : local_root;
    TrimAsciiWhitespace(local_root_value);
    while (!local_root_value.empty() && local_root_value.back() == '/') {
        local_root_value.pop_back();
    }
    const cJSON* critical_assets = cJSON_GetObjectItem(body, "criticalAssets");
    const int asset_count = cJSON_IsArray(assets) ? cJSON_GetArraySize(assets) : 0;
    bool ready = manifest_checksum_required && cache_key_has_manifest_checksum &&
                 cJSON_IsArray(assets) && cJSON_GetArraySize(assets) > 0 &&
                 static_cast<size_t>(asset_count) <= kMaxLessonAssetPackAssets;
    std::set<std::string> ready_asset_keys;
    size_t total_declared_size = 0;
    if (ready) {
        const cJSON* asset = nullptr;
        cJSON_ArrayForEach(asset, assets) {
            const char* asset_key = Str(asset, "key");
            std::string asset_key_value = asset_key == nullptr ? "" : asset_key;
            TrimAsciiWhitespace(asset_key_value);
            const char* state = Str(asset, "state");
            const std::string normalized_state = NormalizeAsciiToken(state);
            const cJSON* checksum_ok = cJSON_GetObjectItem(asset, "checksumOk");
            const bool asset_verified =
                state != nullptr && normalized_state == "READY" && cJSON_IsTrue(checksum_ok);
            const char* local_path = Str(asset, "localPath");
            std::string derived_local_path;
            if (Blank(local_path) && !local_root_value.empty() && !asset_key_value.empty()) {
                derived_local_path = local_root_value + "/" +
                                     EncodeLessonAssetPathSegment(asset_key_value);
                local_path = derived_local_path.c_str();
            }
            double size_value = 0.0;
            bool has_declared_size = Num(asset, "size", size_value) && size_value > 0.0 &&
                                     size_value <= static_cast<double>(SIZE_MAX) &&
                                     static_cast<double>(static_cast<size_t>(size_value)) == size_value;
            size_t expected_size = 0;
            if (has_declared_size) {
                expected_size = static_cast<size_t>(size_value);
            }
            if (asset_key_value.empty() || !asset_verified ||
                !has_declared_size ||
                expected_size > kMaxLessonAssetPackDeclaredBytes - total_declared_size ||
                !LessonLocalFileReady(local_path, expected_size) ||
                !ready_asset_keys.insert(asset_key_value).second || !manifest_checksum_required ||
                !cache_key_has_manifest_checksum) {
                ready = false;
                break;
            }
            total_declared_size += expected_size;
        }
    }
    if (ready) {
        if (critical_assets != nullptr && !cJSON_IsArray(critical_assets)) {
            ready = false;
        } else if (cJSON_IsArray(critical_assets) && cJSON_GetArraySize(critical_assets) > 0) {
            const cJSON* critical_asset = nullptr;
            cJSON_ArrayForEach(critical_asset, critical_assets) {
                const char* critical_key = Str(critical_asset, "key");
                std::string critical_key_value = critical_key == nullptr ? "" : critical_key;
                TrimAsciiWhitespace(critical_key_value);
                if (critical_key_value.empty() ||
                    ready_asset_keys.find(critical_key_value) == ready_asset_keys.end()) {
                    ready = false;
                    break;
                }
            }
        }
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ready", ready);
    if (cache_key != nullptr) cJSON_AddStringToObject(out, "cacheKey", cache_key_value.c_str());
    return out;
}

std::map<std::string, std::string> VerifiedAssetPaths(const cJSON* body) {
    std::map<std::string, std::string> paths;
    const cJSON* pack = Obj(body, "assetPack");
    if (pack == nullptr) return paths;
    const cJSON* assets = cJSON_GetObjectItem(pack, "assets");
    const char* local_root = Str(pack, "localRoot");
    std::string root = local_root != nullptr ? local_root : "";
    TrimAsciiWhitespace(root);
    while (!root.empty() && root.back() == '/') root.pop_back();
    if (!cJSON_IsArray(assets)) return paths;
    const cJSON* asset = nullptr;
    cJSON_ArrayForEach(asset, assets) {
        const char* key = Str(asset, "key");
        const char* local_path = Str(asset, "localPath");
        std::string derived;
        if (key == nullptr) continue;
        if (Blank(local_path) && !root.empty()) {
            derived = root + "/" + EncodeLessonAssetPathSegment(key);
            local_path = derived.c_str();
        }
        if (!Blank(local_path)) paths.emplace(key, local_path);
    }
    return paths;
}

// US-006 image render — HTTP GET a lesson image URL and DECODE it into an LvglImage.
// This is the firmware half the recon flagged as missing: the backend already emits
// resolved scene.*.src URLs and the ESP server downloads/sha256-verifies the bytes,
// but the firmware previously only probed the flashed asset image. We now fetch and
// decode the authored URL over the existing network stack. PNG and other LVGL-native
// formats still go through LvglAllocatedImage's decoder probe; JPEG is decoded here
// with the firmware JPEG helper because LVGL is not built with a JPEG decoder.
//
// FAILURE IS NEVER FATAL: any error (no URL, no network, bad status, alloc fail, short
// read) returns nullptr and the caller falls back to the caption-only ladder. We run
// on the dedicated lesson_worker task (HandleLessonMessage), so this blocking download
// does NOT stall the WS receive callback or LVGL/app task; the decoded draw is then
// marshalled via Schedule().
std::unique_ptr<LvglImage> FetchLessonImage(const char* url) {
    if (url == nullptr || url[0] == '\0') return nullptr;
    std::string fetch_url(url);
    TrimAsciiWhitespace(fetch_url);
    if (fetch_url.empty()) return nullptr;
    const char* canonical_url = fetch_url.c_str();

    if (strncmp(canonical_url, "sd://", 5) == 0 || strncmp(canonical_url, "file://", 7) == 0) {
        return FetchLessonLocalImage(canonical_url);
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "lesson image fetch: no network");
        return nullptr;
    }
    auto http = network->CreateHttp(3);
    if (!http) return nullptr;
    http->SetTimeout(kLessonImageHttpTimeoutMs);

    if (!http->Open("GET", canonical_url)) {
        ESP_LOGW(TAG, "lesson image fetch: open failed");
        return nullptr;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "lesson image fetch: status %d", http->GetStatusCode());
        http->Close();
        return nullptr;
    }

    size_t content_length = http->GetBodyLength();
    char* data = nullptr;
    size_t total_read = 0;
    if (content_length > 0) {
        // Known Content-Length: one pre-sized allocation.
        if (content_length > kMaxLessonImageBytes) {
            ESP_LOGW(TAG, "lesson image fetch: body exceeds %u cap; dropping",
                     (unsigned)kMaxLessonImageBytes);
            http->Close();
            return nullptr;
        }
        data = static_cast<char*>(heap_caps_malloc(content_length, LessonAllocationCaps(content_length)));
        if (data == nullptr) {
            ESP_LOGW(TAG, "lesson image fetch: alloc %u failed", (unsigned)content_length);
            http->Close();
            return nullptr;
        }
        while (total_read < content_length) {
            int ret = http->Read(data + total_read, content_length - total_read);
            if (ret < 0) {
                ESP_LOGW(TAG, "lesson image fetch: read error");
                heap_caps_free(data);
                http->Close();
                return nullptr;
            }
            if (ret == 0) break;  // server closed early
            total_read += ret;
        }
        http->Close();
        if (total_read < content_length) {
            ESP_LOGW(TAG, "lesson image fetch: short read %u/%u",
                     (unsigned)total_read, (unsigned)content_length);
            heap_caps_free(data);
            return nullptr;
        }
    } else {
        // No Content-Length (Transfer-Encoding: chunked — common for CDN-transcoded
        // images): grow-as-you-read until EOF, capped (deep-audit #5: GetBodyLength()
        // == 0 was misread as "empty body" so chunked posters never rendered).
        size_t cap = 16 * 1024;
        data = static_cast<char*>(heap_caps_malloc(cap, LessonAllocationCaps(cap)));
        if (data == nullptr) {
            ESP_LOGW(TAG, "lesson image fetch: alloc %u failed", (unsigned)cap);
            http->Close();
            return nullptr;
        }
        for (;;) {
            if (total_read == cap) {
                if (cap >= kMaxLessonImageBytes) {
                    ESP_LOGW(TAG, "lesson image fetch: body exceeds %u cap; dropping",
                             (unsigned)kMaxLessonImageBytes);
                    heap_caps_free(data);
                    http->Close();
                    return nullptr;
                }
                size_t new_cap = cap * 2;
                if (new_cap > kMaxLessonImageBytes) new_cap = kMaxLessonImageBytes;
                char* grown = static_cast<char*>(heap_caps_malloc(new_cap, LessonAllocationCaps(new_cap)));
                if (grown == nullptr) {
                    ESP_LOGW(TAG, "lesson image fetch: realloc %u failed", (unsigned)new_cap);
                    heap_caps_free(data);
                    http->Close();
                    return nullptr;
                }
                memcpy(grown, data, total_read);
                heap_caps_free(data);
                data = grown;
                cap = new_cap;
            }
            int ret = http->Read(data + total_read, cap - total_read);
            if (ret < 0) {
                ESP_LOGW(TAG, "lesson image fetch: read error");
                heap_caps_free(data);
                http->Close();
                return nullptr;
            }
            if (ret == 0) break;  // EOF
            total_read += ret;
        }
        http->Close();
        if (total_read == 0) {
            ESP_LOGW(TAG, "lesson image fetch: empty body");
            heap_caps_free(data);
            return nullptr;
        }
        content_length = total_read;
    }

    return DecodeLessonImageBytes(data, content_length, "lesson image fetch");
}

}  // namespace

void SetLessonTransportEpoch(std::uint64_t transport_epoch) {
    g_session.current_transport_epoch = transport_epoch;
}

void InvalidateLessonVisualCompletionState(std::uint64_t transport_epoch) {
    g_visual_callback_token.fetch_add(1, std::memory_order_acq_rel);
    g_session.current_transport_epoch = transport_epoch;
    ++g_session.visual_generation;
    g_session.entrance_active = false;
    g_session.pending_server_sequence = 0;
    g_session.pending_visual_nonce = 0;
    g_session.pending_protocol_version.clear();
    g_session.pending_lesson_id.clear();
    g_session.pending_lesson_version = 0;
    g_session.pending_has_lesson_version = false;
    g_session.pending_step_id.clear();
    g_session.pending_ack = false;
}

bool AcceptLessonVisualCompletion(const LessonQueueItem& item, std::string* ack_frame) {
    if (ack_frame == nullptr) return false;
    ack_frame->clear();
    if ((item.kind != LessonQueueItemKind::kVisualCompleted &&
         item.kind != LessonQueueItemKind::kVisualTimedOut) ||
        !g_session.pending_ack ||
        item.transport_epoch != g_session.current_transport_epoch ||
        item.visual_generation != g_session.visual_generation ||
        item.visual_nonce == 0 ||
        item.visual_nonce != g_session.pending_visual_nonce ||
        item.server_sequence != g_session.pending_server_sequence ||
        g_session.assignment_id != item.assignment_id ||
        g_session.session_id != item.session_id ||
        g_session.pending_step_id != item.step_id) {
        return false;
    }

    bool accepted = false;
    bool degraded = false;
    const char* degraded_reason = nullptr;
    switch (item.kind == LessonQueueItemKind::kVisualTimedOut
                ? LessonVisualCompletionResult::kPhaseTimeout
                : item.completion_result) {
    case LessonVisualCompletionResult::kApplied:
        accepted = true;
        break;
    case LessonVisualCompletionResult::kDegraded:
        accepted = true;
        degraded = true;
        degraded_reason = StableVisualDegradedReason(item.degraded_reason);
        if (degraded_reason == nullptr) degraded_reason = "missingOverlay";
        break;
    case LessonVisualCompletionResult::kPhaseTimeout:
        accepted = true;
        degraded = true;
        degraded_reason = "phaseTimeout";
        break;
    case LessonVisualCompletionResult::kRejected:
        degraded_reason = StableVisualDegradedReason(item.degraded_reason);
        if (degraded_reason == nullptr) degraded_reason = "unsupportedContract";
        break;
    }

    cJSON* root = LessonJsonCreateObject();
    cJSON* body = LessonJsonCreateObject();
    if (root == nullptr || body == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(body);
        return false;
    }
    bool built = LessonJsonAddString(root, "type", "lesson_ack") != nullptr &&
                 LessonJsonAddString(root, "protocolVersion",
                                     g_session.pending_protocol_version.c_str()) != nullptr &&
                 LessonJsonAddString(root, "assignmentId", g_session.assignment_id.c_str()) != nullptr &&
                 LessonJsonAddString(root, "sessionId", g_session.session_id.c_str()) != nullptr;
    if (!g_session.pending_lesson_id.empty()) {
        built = built && LessonJsonAddString(
            root, "lessonId", g_session.pending_lesson_id.c_str()) != nullptr;
    }
    if (g_session.pending_has_lesson_version) {
        built = built && LessonJsonAddNumber(
            root, "lessonVersion", g_session.pending_lesson_version) != nullptr;
    }
    const int64_t ack_sequence = g_session.fs_sequence + 1;
    built = built &&
            (g_session.pending_step_id.empty()
                ? LessonJsonAddNull(root, "stepId") != nullptr
                : LessonJsonAddString(root, "stepId", g_session.pending_step_id.c_str()) != nullptr) &&
            LessonJsonAddNumber(root, "sequence", static_cast<double>(ack_sequence)) != nullptr &&
            LessonJsonAddNumber(root, "timestamp", static_cast<double>(NowMs())) != nullptr &&
            LessonJsonAddNumber(body, "acks", static_cast<double>(item.server_sequence)) != nullptr &&
            LessonJsonAddBool(body, "accepted", accepted) != nullptr &&
            LessonJsonAddBool(body, "degraded", degraded) != nullptr &&
            (degraded_reason == nullptr
                ? LessonJsonAddNull(body, "degradedReason") != nullptr
                : LessonJsonAddString(body, "degradedReason", degraded_reason) != nullptr) &&
            LessonJsonAddNumber(body, "visualGeneration",
                                static_cast<double>(item.visual_generation)) != nullptr;
    if (!built) {
        cJSON_Delete(root);
        cJSON_Delete(body);
        return false;
    }

    char* body_json = LessonJsonPrintUnformatted(body);
    if (!LessonJsonAttachItem(root, "body", body)) {
        if (body_json != nullptr) cJSON_free(body_json);
        cJSON_Delete(body);
        cJSON_Delete(root);
        return false;
    }
    char* serialized = LessonJsonPrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == nullptr) {
        if (body_json != nullptr) cJSON_free(body_json);
        return false;
    }
    g_session.fs_sequence = ack_sequence;
    *ack_frame = serialized;
    cJSON_free(serialized);
    if (body_json != nullptr) {
        g_session.last_ack_sequence = item.server_sequence;
        g_session.last_ack_body_json = body_json;
        g_session.ack_history.push_back({item.server_sequence, body_json});
        while (g_session.ack_history.size() > LessonSession::kAckReplayWindow) {
            g_session.ack_history.pop_front();
        }
        cJSON_free(body_json);
    }
    g_session.entrance_active = false;
    g_session.pending_ack = false;
    g_session.pending_server_sequence = 0;
    g_session.pending_visual_nonce = 0;
    g_session.pending_protocol_version.clear();
    g_session.pending_lesson_id.clear();
    g_session.pending_lesson_version = 0;
    g_session.pending_has_lesson_version = false;
    g_session.pending_step_id.clear();
    return true;
}

bool DispatchLessonVisualCompletion(const LessonQueueItem& item, Protocol* protocol) {
    if (protocol == nullptr) return false;
    std::string ack_frame;
    if (!AcceptLessonVisualCompletion(item, &ack_frame)) return false;
    return protocol->SendLessonFrame(ack_frame);
}

// Sub-dispatch the slice subset (lesson_prepare/start/step/stop) and render ONE
// espTft lesson_step. Additive: never reached for the 8 legacy types, the voice
// path, or the MCP arm tools.
bool Application::AbandonLessonStorageSession() {
    if (g_session.lesson_asset_generation == 0) return false;
    const std::string assignment_id = g_session.assignment_id;
    const std::string session_id = g_session.session_id;
    const std::uint64_t generation = g_session.lesson_asset_generation;
    if (!LessonAssetStorageCoordinator::GetInstance().EndLessonSession(
            assignment_id, session_id, generation)) {
        return false;
    }
    if (g_session.assignment_id == assignment_id &&
        g_session.session_id == session_id &&
        g_session.lesson_asset_generation == generation) {
        g_session.lesson_asset_generation = 0;
        g_session.running = false;
        g_session.paused = false;
        g_session.prepared = false;
        ClearTerminalLessonCursor();
        CancelLessonInteractiveListening();
        SetLessonRuntimeActive(false);
    }
    return true;
}

void Application::HandleLessonMessage(const cJSON* root) {
    const char* type = Str(root, "type");
    if (type == nullptr) return;  // defensive (transports pre-guard; see DIV note)

    // --- envelope identity (all null-guarded) ---
    const char* assignment_id    = Str(root, "assignmentId");
    const char* session_id       = Str(root, "sessionId");
    const char* protocol_version = Str(root, "protocolVersion");
    double sequence_d = 0.0;
    const bool has_seq = Num(root, "sequence", sequence_d);
    const cJSON* body = Obj(root, "body");
    const std::uint64_t frame_transport_epoch = g_session.current_transport_epoch;

    if (assignment_id == nullptr || session_id == nullptr || !has_seq) {
        ESP_LOGW(TAG, "lesson_* dropped: missing assignmentId/sessionId/sequence");
        return;
    }
    if (!std::isfinite(sequence_d) || sequence_d < 0.0 ||
        sequence_d > static_cast<double>(INT32_MAX) || std::trunc(sequence_d) != sequence_d) {
        ESP_LOGW(TAG, "lesson_* dropped: sequence must be an integer between 0 and INT32_MAX");
        return;
    }
    const int64_t sequence = static_cast<int64_t>(sequence_d);

    // emit helper: one outbound frame per call, advancing the F->S counter.
    // FW-LESSON-03: BuildFrame is the sole owner/consumer of frame_body (it frees it
    // via cJSON_Delete(root)). Build the frame string FIRST so frame_body is always
    // consumed, then guard only the SEND on protocol_ — otherwise a null protocol_
    // would leak frame_body (a latent per-frame heap leak if protocol_ is ever torn
    // down with a frame in flight).
    auto emit = [this](const cJSON* in, const char* frame_type, cJSON* frame_body) {
        if (frame_body == nullptr) return;
        const int64_t seq = g_session.fs_sequence + 1;
        std::string frame = BuildFrame(in, frame_type, seq, frame_body);
        if (protocol_ && !frame.empty()) {
            g_session.fs_sequence = seq;
            protocol_->SendLessonFrame(frame);
        }
    };
    auto emit_isolated_prepare_error = [this](const cJSON* in, cJSON* frame_body) {
        if (frame_body == nullptr) return;
        std::string frame = BuildFrame(in, "lesson_error", 1, frame_body);
        if (protocol_ && !frame.empty()) protocol_->SendLessonFrame(frame);
    };
    // Canonical lesson_ack (plan §5.3 / P0): body.acks echoes the ACKED sequence;
    // the ack's own envelope.sequence is the firmware F->S counter; there is NO
    // ackFor field. stepId is echoed (null on lifecycle, "s4" on a step).
    auto emit_ack = [&emit](const cJSON* in, int64_t acked, bool rendered, bool degraded,
                            cJSON* asset_pack_ack = nullptr, bool cache = true,
                            int64_t render_elapsed_ms = -1,
                            const char* degraded_reason = nullptr,
                            cJSON* telemetry = nullptr) {
        cJSON* b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "acks", static_cast<double>(acked));
        cJSON_AddBoolToObject(b, "rendered", rendered);
        cJSON_AddBoolToObject(b, "degraded", degraded);
        if (degraded_reason != nullptr && degraded_reason[0] != '\0') {
            cJSON_AddStringToObject(b, "degradedReason", degraded_reason);
        }
        const cJSON* inbound_scene = Obj(Obj(in, "body"), "scene");
        const char* robot_state = CanonicalRobotState(
            Str(Obj(inbound_scene, "robotOverlay"), "robotState"));
        if (robot_state != nullptr) {
            cJSON_AddStringToObject(b, "robotState", robot_state);
        }
        if (render_elapsed_ms >= 0) {
            cJSON_AddNumberToObject(b, "renderElapsedMs", static_cast<double>(render_elapsed_ms));
        }
        // FW-LESSON-02: remember this ack's body so a later duplicate of `acked` replays
        // the EXACT same (rendered, degraded). A re-ack (cache=false) never overwrites
        // the cached original.
        if (cache) {
            g_session.last_ack_sequence = acked;
            g_session.last_ack_rendered = rendered;
            g_session.last_ack_degraded = degraded;
            g_session.last_ack_degraded_reason = degraded_reason != nullptr ? degraded_reason : "";
            g_session.last_ack_asset_pack_json.clear();
            g_session.last_ack_telemetry_json.clear();
            if (asset_pack_ack != nullptr) {
                char* printed = cJSON_PrintUnformatted(asset_pack_ack);
                if (printed != nullptr) {
                    g_session.last_ack_asset_pack_json = printed;
                    const char* ack_type = Str(in, "type");
                    if (ack_type != nullptr && strcmp(ack_type, "lesson_prepare") == 0) {
                        g_session.prepare_ack_sequence = acked;
                        g_session.prepare_ack_asset_pack_json = printed;
                    }
                    cJSON_free(printed);
                }
            }
            if (telemetry != nullptr) {
                char* printed = cJSON_PrintUnformatted(telemetry);
                if (printed != nullptr) {
                    g_session.last_ack_telemetry_json = printed;
                    cJSON_free(printed);
                }
            }
        }
        if (asset_pack_ack != nullptr) cJSON_AddItemToObject(b, "assetPack", asset_pack_ack);
        if (telemetry != nullptr) cJSON_AddItemToObject(b, "telemetry", telemetry);
        LogLessonAckEvidence(in, b);
        if (cache) {
            char* printed = cJSON_PrintUnformatted(b);
            if (printed != nullptr) {
                g_session.last_ack_body_json = printed;
                g_session.ack_history.push_back({acked, printed});
                while (g_session.ack_history.size() > LessonSession::kAckReplayWindow) {
                    g_session.ack_history.pop_front();
                }
                cJSON_free(printed);
            }
        }
        emit(in, "lesson_ack", b);
    };
    auto emit_isolated_prepare_ack = [this](const cJSON* in, int64_t acked,
                                            cJSON* asset_pack_ack) {
        cJSON* body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "acks", static_cast<double>(acked));
        cJSON_AddBoolToObject(body, "rendered", false);
        cJSON_AddBoolToObject(body, "degraded", false);
        if (asset_pack_ack != nullptr) {
            cJSON_AddItemToObject(body, "assetPack", asset_pack_ack);
        }
        LogLessonAckEvidence(in, body);
        std::string frame = BuildFrame(in, "lesson_ack", 1, body);
        if (protocol_) protocol_->SendLessonFrame(frame);
    };
    auto show_lesson_failure_display = [this]() {
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            Schedule([display, lvgl_display]() {
                if (lvgl_display) lvgl_display->CancelLessonRobotEntrance();
                if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                if (lvgl_display) lvgl_display->SetLessonObject(nullptr);
                if (lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
                if (lvgl_display) lvgl_display->SetLessonTeachingWord("");
                if (lvgl_display) lvgl_display->SetLessonMode(false);
                display->SetLessonCaption("");
                display->SetStatus(Lang::Strings::ERROR);
                display->SetEmotion("sad");
                display->ClearChatMessages();
                display->SetChatMessage("assistant", "Bài học chưa tải được.");
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
            });
        }
    };
    auto abort_speaking_if_needed = []() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
    };
    auto end_lesson_asset_session = []() {
        if (g_session.lesson_asset_generation == 0) return false;
        const bool ended = LessonAssetStorageCoordinator::GetInstance().EndLessonSession(
            g_session.assignment_id,
            g_session.session_id,
            g_session.lesson_asset_generation);
        if (ended) g_session.lesson_asset_generation = 0;
        return ended;
    };
    auto end_lesson_after_failure = [this, &show_lesson_failure_display,
                                     &abort_speaking_if_needed,
                                     &end_lesson_asset_session](bool release_asset_session = true) {
        InvalidateLessonVisualCompletionState(g_session.current_transport_epoch);
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        Application::GetInstance().SetLessonRuntimeActive(false);
        g_session.running = false;
        g_session.paused = false;
        g_session.prepared = false;
        ClearTerminalLessonCursor();
        g_layer_state.ClearAll();
        if (release_asset_session) end_lesson_asset_session();
        show_lesson_failure_display();
    };
    auto clear_stale_lesson_for_fresh_prepare = [this](bool show_waiting_state) {
        InvalidateLessonVisualCompletionState(g_session.current_transport_epoch);
        Application::GetInstance().CancelLessonInteractiveListening();
        Application::GetInstance().SetLessonRuntimeActive(false);
        ClearTerminalLessonCursor();
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            Schedule([display, lvgl_display, show_waiting_state]() {
                if (lvgl_display) lvgl_display->CancelLessonRobotEntrance();
                if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                if (lvgl_display) lvgl_display->SetLessonObject(nullptr);
                if (lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
                if (lvgl_display) lvgl_display->SetLessonTeachingWord("");
                if (lvgl_display) lvgl_display->SetLessonMode(false);
                display->SetLessonCaption("");
                display->ClearChatMessages();
                if (show_waiting_state) {
                    display->SetStatus(Lang::Strings::PLEASE_WAIT);
                    display->SetEmotion("thinking");
                }
            });
        }
    };

    const bool cinematic_v3 = protocol_version != nullptr &&
        strcmp(protocol_version, tbot::kLessonRendererV3) == 0;
    const bool cinematic_v4 = protocol_version != nullptr &&
        strcmp(protocol_version, tbot::kLessonRendererV4) == 0;
    if (cinematic_v3 || cinematic_v4) {
        const bool prepare_frame = strcmp(type, "lesson_prepare") == 0;
        const bool start_frame = strcmp(type, "lesson_start") == 0;
        const bool stop_frame = strcmp(type, "lesson_stop") == 0;
        const bool control_frame = strcmp(type, "lesson_cinematic_control") == 0;
        if (!prepare_frame && !start_frame && !stop_frame && !control_frame) {
            emit(root, "lesson_error", MakeErrorBody(
                "CINEMATIC_COMMAND_UNSUPPORTED", "unsupported cinematic command frame",
                false, "command"));
            return;
        }
        const cJSON* command_body = (prepare_frame || start_frame || stop_frame)
            ? Obj(body, "cinematicPhase") : body;
        const char* command = Str(command_body, "command");
        const char* phase_id = Str(command_body, "phaseId");
        double command_sequence_number = 0;
        const bool command_sequence_ok = Num(command_body, "commandSequenceId",
                                              command_sequence_number) &&
            PositiveIntegerAtMost(command_sequence_number, kMaxJsonSafeInteger);
        const bool command_matches_frame = command != nullptr &&
            ((prepare_frame && strcmp(command, "prepare") == 0) ||
             (start_frame && strcmp(command, "start") == 0) ||
             (stop_frame && strcmp(command, "stop") == 0) ||
             (control_frame && (strcmp(command, "pause") == 0 ||
                                strcmp(command, "resume") == 0 ||
                                strcmp(command, "cancel") == 0)));
        const bool known_phase = phase_id != nullptr &&
            (strcmp(phase_id, "opening") == 0 || strcmp(phase_id, "greet") == 0 ||
             strcmp(phase_id, "teach") == 0 || strcmp(phase_id, "listen") == 0 ||
             strcmp(phase_id, "thinking") == 0 || strcmp(phase_id, "correct") == 0 ||
             strcmp(phase_id, "retry") == 0 || strcmp(phase_id, "celebrate") == 0);
        bool cinematic_command_shape_ok = true;
        if (!prepare_frame) {
            static const std::set<std::string_view> kBasicControlKeys = {
                "command", "phaseId", "commandSequenceId"};
            static const std::set<std::string_view> kResumeKeys = {
                "command", "phaseId", "commandSequenceId", "clockRebaseSequenceId"};
            static const std::set<std::string_view> kCancelKeys = {
                "command", "phaseId", "commandSequenceId", "reason"};
            const auto& expected = cinematic_v4 && command != nullptr &&
                    strcmp(command, "resume") == 0
                ? kResumeKeys
                : cinematic_v4 && command != nullptr && strcmp(command, "cancel") == 0
                    ? kCancelKeys : kBasicControlKeys;
            cinematic_command_shape_ok = ExactObjectKeys(command_body, expected);
        }
        bool control_values_ok = true;
        if (cinematic_v4 && !prepare_frame && command != nullptr &&
            strcmp(command, "resume") == 0) {
            double rebase_sequence = 0;
            control_values_ok = Num(command_body, "clockRebaseSequenceId", rebase_sequence) &&
                PositiveIntegerAtMost(rebase_sequence, kMaxJsonSafeInteger) &&
                rebase_sequence == command_sequence_number;
        } else if (cinematic_v4 && !prepare_frame && command != nullptr &&
                   strcmp(command, "cancel") == 0) {
            const char* reason = Str(command_body, "reason");
            control_values_ok = reason != nullptr && !Blank(reason) && std::strlen(reason) <= 64;
        }
        const std::uint64_t command_sequence_id = command_sequence_ok
            ? static_cast<std::uint64_t>(command_sequence_number) : 0;

        auto cinematic_error_name = [](tbot::LessonCinematicError error) {
            switch (error) {
            case tbot::LessonCinematicError::kUnsupportedContract: return "CINEMATIC_CAPABILITY_UNSUPPORTED";
            case tbot::LessonCinematicError::kInvalidPath:
            case tbot::LessonCinematicError::kFileOpen: return "CINEMATIC_SD_PATH_MISSING";
            case tbot::LessonCinematicError::kParserFailed: return "CINEMATIC_PARSER_FAILED";
            case tbot::LessonCinematicError::kFileRead: return "CINEMATIC_FILE_READ_FAILED";
            case tbot::LessonCinematicError::kSessionMismatch: return "CINEMATIC_SESSION_MISMATCH";
            case tbot::LessonCinematicError::kInsufficientPsram: return "CINEMATIC_INSUFFICIENT_PSRAM";
            case tbot::LessonCinematicError::kDecodeFailed: return "CINEMATIC_DECODE_FAILED";
            case tbot::LessonCinematicError::kDecodeTimeout: return "CINEMATIC_DECODE_TIMEOUT";
            case tbot::LessonCinematicError::kPresentFailed: return "CINEMATIC_PRESENT_FAILED";
            case tbot::LessonCinematicError::kStaleCommand: return "CINEMATIC_STALE_COMMAND";
            case tbot::LessonCinematicError::kSessionReleaseFailed:
                return "CINEMATIC_SESSION_RELEASE_FAILED";
            case tbot::LessonCinematicError::kInvalidPhase:
            case tbot::LessonCinematicError::kMetadataMismatch:
            case tbot::LessonCinematicError::kInvalidState:
            case tbot::LessonCinematicError::kNone: return "CINEMATIC_METADATA_MISMATCH";
            }
            return "CINEMATIC_METADATA_MISMATCH";
        };
        auto emit_cinematic_ack = [&](const tbot::LessonCinematicResponse& response) {
            if (!response.accepted) {
                emit(root, "lesson_error", MakeErrorBody(
                    cinematic_error_name(response.error), "cinematic command rejected",
                    false, "cinematicPhase"));
                return;
            }
            cJSON* ack_body = CinematicJsonCreateObject();
            cJSON* cinematic = CinematicJsonCreateObject();
            const char* event = response.type == tbot::LessonCinematicResponseType::kFrameZeroReady
                ? "frameZeroReady"
                : response.type == tbot::LessonCinematicResponseType::kPhaseReady
                    ? "phaseReady" : "commandApplied";
            bool built = ack_body != nullptr && cinematic != nullptr &&
                CinematicJsonAddNumber(ack_body, "acks", static_cast<double>(sequence)) &&
                CinematicJsonAddString(cinematic, "event", event) &&
                CinematicJsonAddString(cinematic, "command", command != nullptr ? command : "") &&
                CinematicJsonAddString(cinematic, "phaseId", phase_id != nullptr ? phase_id : "") &&
                CinematicJsonAddNumber(cinematic, "commandSequenceId",
                                       static_cast<double>(command_sequence_id)) &&
                CinematicJsonAddBool(cinematic, "accepted", true);
            if (response.accepted && prepare_frame) {
                built = built && CinematicJsonAddBool(cinematic, "frameZeroReady", true);
            } else if (response.accepted && start_frame) {
                built = built && CinematicJsonAddBool(cinematic, "phaseReady", true);
            }
            if (!built || !CinematicJsonOperationAllowed() ||
                !cJSON_AddItemToObject(ack_body, "cinematicPhase", cinematic)) {
                cJSON_Delete(cinematic);
                cJSON_Delete(ack_body);
                return;
            }
            emit(root, "lesson_ack", ack_body);
        };

        if (!prepare_frame &&
            (!g_session.prepared || g_session.lesson_asset_generation == 0 ||
             g_session.assignment_id != assignment_id || g_session.session_id != session_id ||
             g_session.cinematic_renderer_id != protocol_version)) {
            emit_isolated_prepare_error(root, MakeErrorBody(
                "CINEMATIC_SESSION_MISMATCH", "cinematic command session is not active",
                false, "cinematicPhase"));
            return;
        }

        tbot::LessonCinematicRenderer* renderer_v3 = tbot::ActiveLessonCinematicRenderer();
        tbot::LessonFlattenedCinematicRenderer* renderer_v4 =
            tbot::ActiveLessonFlattenedCinematicRenderer();
        const bool renderer_available = cinematic_v3 ? renderer_v3 != nullptr
                                                     : renderer_v4 != nullptr;
        if (!command_matches_frame || !known_phase || !command_sequence_ok ||
            !cinematic_command_shape_ok || !control_values_ok || !renderer_available) {
            emit_cinematic_ack({tbot::LessonCinematicResponseType::kFailure, false,
                                command_sequence_id, phase_id != nullptr ? phase_id : "",
                                !renderer_available
                                    ? tbot::LessonCinematicError::kUnsupportedContract
                                    : tbot::LessonCinematicError::kMetadataMismatch});
            return;
        }

        const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        auto commit_cinematic_prepare_session = [&](std::uint64_t generation) {
            const bool fresh_session = !g_session.prepared ||
                g_session.assignment_id != assignment_id || g_session.session_id != session_id;
            if (fresh_session) {
                InvalidateLessonVisualCompletionState(frame_transport_epoch);
                Application::GetInstance().CancelLessonInteractiveListening();
                Application::GetInstance().SetLessonRuntimeActive(false);
                g_layer_state.ClearAll();
                g_session = LessonSession{};
                g_session.assignment_id = assignment_id;
                g_session.session_id = session_id;
                g_session.current_transport_epoch = frame_transport_epoch;
            }
            g_session.lesson_asset_generation = generation;
            g_session.last_in_sequence = sequence;
            g_session.cinematic_renderer_id = protocol_version;
            g_session.prepared = true;
            g_session.running = false;
            g_session.paused = false;
        };
        auto complete_cinematic_prepare = [&](tbot::LessonCinematicResponse& prepare_response,
                                              std::uint64_t generation,
                                              bool route_v4) {
            if (!prepare_response.accepted) return;
            const bool renderer_handoff = g_session.prepared &&
                !g_session.cinematic_renderer_id.empty() &&
                g_session.cinematic_renderer_id != protocol_version;
            if (renderer_handoff) {
                if (g_session.cinematic_renderer_id == tbot::kLessonRendererV3 &&
                    renderer_v3 != nullptr) {
                    renderer_v3->DiscardSession();
                } else if (g_session.cinematic_renderer_id == tbot::kLessonRendererV4 &&
                           renderer_v4 != nullptr) {
                    renderer_v4->DiscardSession();
                }
            }
            tbot::SetLessonCinematicTimerRouteV4(route_v4);
            commit_cinematic_prepare_session(generation);
        };
        tbot::LessonCinematicResponse response;
        if (prepare_frame) {
            if (cinematic_v4) {
                static const std::set<std::string_view> kPrepareKeys = {
                    "command", "commandSequenceId", "templateId", "templateVersion",
                    "phaseId", "durationMs", "fps", "frameCount", "asset"};
                static const std::set<std::string_view> kAssetKeys = {
                    "derivativeId", "phaseId", "sdPath", "sha256", "bytes",
                    "mediaType", "width", "height"};
                const cJSON* asset = Obj(command_body, "asset");
                double template_version = 0, duration_ms = 0, fps = 0, frame_count = 0;
                double bytes = 0, width = 0, height = 0;
                std::string normalized_path = LessonLocalPath(Str(asset, "sdPath"));
                const bool valid = ExactObjectKeys(command_body, kPrepareKeys) &&
                    ExactObjectKeys(asset, kAssetKeys) &&
                    Num(command_body, "templateVersion", template_version) &&
                    Num(command_body, "durationMs", duration_ms) && Num(command_body, "fps", fps) &&
                    Num(command_body, "frameCount", frame_count) && Num(asset, "bytes", bytes) &&
                    Num(asset, "width", width) && Num(asset, "height", height) &&
                    template_version == 1 && fps == 10 && width == 480 && height == 320 &&
                    PositiveIntegerAtMost(duration_ms, UINT32_MAX) &&
                    PositiveIntegerAtMost(frame_count, UINT32_MAX) &&
                    PositiveIntegerAtMost(bytes, static_cast<double>(UINT64_MAX)) &&
                    !normalized_path.empty();
                if (!valid) {
                    response = {tbot::LessonCinematicResponseType::kFailure, false,
                                command_sequence_id, phase_id,
                                tbot::LessonCinematicError::kMetadataMismatch};
                } else {
                    tbot::LessonFlattenedCinematicPhaseConfig config{};
                    config.renderer_id = protocol_version;
                    config.template_id = Str(command_body, "templateId");
                    config.template_version = static_cast<std::uint16_t>(template_version);
                    config.phase_id = phase_id;
                    config.command_sequence_id = command_sequence_id;
                    config.duration_ms = static_cast<std::uint32_t>(duration_ms);
                    config.fps = static_cast<std::uint16_t>(fps);
                    config.frame_count = static_cast<std::uint32_t>(frame_count);
                    config.asset.derivative_id = Str(asset, "derivativeId");
                    config.asset.phase_id = Str(asset, "phaseId");
                    config.asset.sd_path = normalized_path.c_str();
                    config.asset.sha256 = Str(asset, "sha256");
                    config.asset.bytes = static_cast<std::uint64_t>(bytes);
                    config.asset.media_type = Str(asset, "mediaType");
                    config.asset.width = static_cast<std::uint16_t>(width);
                    config.asset.height = static_cast<std::uint16_t>(height);
                    const auto reservation = BeginCinematicLessonSession(
                        assignment_id, session_id);
                    if (!reservation.acquired) {
                        response = {tbot::LessonCinematicResponseType::kFailure, false,
                                    command_sequence_id, phase_id,
                                    tbot::LessonCinematicError::kFileOpen};
                    } else {
                        tbot::ConfigureProductionLessonFlattenedCinematicSession(
                            assignment_id, session_id, reservation.generation);
                        response = renderer_v4->Prepare(config, now_ms);
                        if (response.accepted) {
                            complete_cinematic_prepare(response, reservation.generation, true);
                        } else if (!reservation.idempotent) {
                            LessonAssetStorageCoordinator::GetInstance().EndLessonSession(
                                assignment_id, session_id, reservation.generation);
                        }
                    }
                }
            } else {
            tbot::LessonCinematicPhaseConfig config{};
            config.renderer_id = protocol_version;
            config.template_id = Str(command_body, "templateId");
            config.phase_id = phase_id;
            config.command_sequence_id = command_sequence_id;
            std::array<std::string, 3> normalized_paths;
            const cJSON* layers = cJSON_GetObjectItem(command_body, "layers");
            bool valid_layers = cJSON_IsArray(layers) && cJSON_GetArraySize(layers) == 3;
            static constexpr const char* kExpectedLayers[3] = {
                "background", "teachingObject", "robotOverlay"};
            static constexpr const char* kExpectedSlots[3] = {
                "backgroundScene", "teachingObject", "robotOverlay"};
            for (int index = 0; valid_layers && index < 3; ++index) {
                const cJSON* layer = cJSON_GetArrayItem(layers, index);
                const cJSON* rect = Obj(layer, "rect");
                double x = 0, y = 0, width = 0, height = 0;
                valid_layers = Str(layer, "layer") != nullptr && Str(layer, "slot") != nullptr &&
                    strcmp(Str(layer, "layer"), kExpectedLayers[index]) == 0 &&
                    strcmp(Str(layer, "slot"), kExpectedSlots[index]) == 0 &&
                    Num(rect, "x", x) && Num(rect, "y", y) &&
                    Num(rect, "width", width) && Num(rect, "height", height) &&
                    std::trunc(x) == x && std::trunc(y) == y && std::trunc(width) == width &&
                    std::trunc(height) == height && width > 0 && height > 0 &&
                    width <= UINT16_MAX && height <= UINT16_MAX;
                normalized_paths[index] = LessonLocalPath(Str(layer, "sdPath"));
                valid_layers = valid_layers && !normalized_paths[index].empty();
                config.layers[index].sd_path = normalized_paths[index].c_str();
                config.layers[index].rect = {static_cast<std::int32_t>(x),
                                             static_cast<std::int32_t>(y),
                                             static_cast<std::uint16_t>(width),
                                             static_cast<std::uint16_t>(height)};
                if (index > 0) {
                    const cJSON* chroma = Obj(layer, "chromaKey");
                    const char* color = Str(chroma, "keyColor");
                    double tolerance = 0, feather = 0;
                    valid_layers = valid_layers && color != nullptr && strlen(color) == 7 &&
                        color[0] == '#' && Num(chroma, "tolerance", tolerance) &&
                        Num(chroma, "featherPx", feather) && tolerance >= 0 && tolerance <= 255 &&
                        feather >= 0 && feather <= 255;
                    unsigned rgb = 0;
                    if (valid_layers && sscanf(color + 1, "%06x", &rgb) == 1) {
                        config.layers[index].chroma = {{
                            static_cast<std::uint8_t>((rgb >> 16) & 0xff),
                            static_cast<std::uint8_t>((rgb >> 8) & 0xff),
                            static_cast<std::uint8_t>(rgb & 0xff)},
                            static_cast<std::uint8_t>(tolerance),
                            static_cast<std::uint8_t>(feather)};
                    } else {
                        valid_layers = false;
                    }
                }
            }
            if (!valid_layers) {
                response = {tbot::LessonCinematicResponseType::kFailure, false,
                            command_sequence_id, phase_id,
                            tbot::LessonCinematicError::kMetadataMismatch};
            } else {
                const auto reservation = BeginCinematicLessonSession(
                    assignment_id, session_id);
                if (!reservation.acquired) {
                    response = {tbot::LessonCinematicResponseType::kFailure, false,
                                command_sequence_id, phase_id,
                                tbot::LessonCinematicError::kFileOpen};
                } else {
                    tbot::ConfigureProductionLessonCinematicSession(
                        assignment_id, session_id, reservation.generation);
                    response = renderer_v3->Prepare(config, now_ms);
                    if (response.accepted) {
                        complete_cinematic_prepare(response, reservation.generation, false);
                    } else if (!reservation.idempotent) {
                        LessonAssetStorageCoordinator::GetInstance().EndLessonSession(
                            assignment_id, session_id, reservation.generation);
                    }
                }
            }
            }
        } else if (start_frame) {
            response = cinematic_v4 ? renderer_v4->Start(command_sequence_id, phase_id, now_ms)
                                    : renderer_v3->Start(command_sequence_id, phase_id, now_ms);
            if (response.accepted) {
                g_session.running = true;
                g_session.paused = false;
                SetLessonRuntimeActive(true);
            }
        } else if (control_frame && strcmp(command, "pause") == 0) {
            response = cinematic_v4 ? renderer_v4->Pause(command_sequence_id, phase_id, now_ms)
                                    : renderer_v3->Pause(command_sequence_id, phase_id, now_ms);
            if (response.accepted) g_session.paused = true;
        } else if (control_frame && strcmp(command, "resume") == 0) {
            response = cinematic_v4 ? renderer_v4->Resume(command_sequence_id, phase_id, now_ms)
                                    : renderer_v3->Resume(command_sequence_id, phase_id, now_ms);
            if (response.accepted) g_session.paused = false;
        } else if (control_frame) {
            response = cinematic_v4 ? renderer_v4->Cancel(command_sequence_id, phase_id)
                                    : renderer_v3->Cancel(command_sequence_id, phase_id);
        } else {
            response = cinematic_v4 ? renderer_v4->Stop(command_sequence_id, phase_id)
                                    : renderer_v3->Stop(command_sequence_id, phase_id);
        }
        const bool terminal_command = stop_frame ||
            (control_frame && strcmp(command, "cancel") == 0);
        if (response.accepted && terminal_command) {
            SetLessonRuntimeActive(false);
            g_session.running = false;
            g_session.paused = false;
            if (cinematic_v4) renderer_v4->DiscardSession();
            else renderer_v3->DiscardSession();
            const bool session_released =
                (g_session.lesson_asset_generation == 0 ||
                LessonAssetStorageCoordinator::GetInstance().EndLessonSession(
                    g_session.assignment_id, g_session.session_id,
                    g_session.lesson_asset_generation));
            if (session_released) {
                g_session = LessonSession{};
            } else {
                response = {tbot::LessonCinematicResponseType::kFailure, false,
                            command_sequence_id, phase_id,
                            tbot::LessonCinematicError::kSessionReleaseFailed};
            }
        }
        emit_cinematic_ack(response);
        return;
    }

    const bool is_prepare = strcmp(type, "lesson_prepare") == 0;
    const bool preload_reset_only =
        is_prepare && cJSON_IsTrue(cJSON_GetObjectItem(body, "preloadResetOnly"));
    double prepare_assignment_version = 0.0;
    const bool prepare_has_assignment_version =
        is_prepare && Num(body, "assignmentVersion", prepare_assignment_version);
    const bool prepare_has_newer_assignment_version =
        prepare_has_assignment_version &&
        prepare_assignment_version > g_session.assignment_version;
    const bool same_session =
        g_session.assignment_id == assignment_id &&
        g_session.session_id == session_id;
    const bool restart_prepare =
        is_prepare &&
        same_session &&
        sequence == 1 &&
        g_session.running &&
        g_session.last_in_sequence > 2;
    const bool duplicate_prepare =
        is_prepare &&
        same_session &&
        sequence <= g_session.last_in_sequence &&
        !restart_prepare &&
        !prepare_has_newer_assignment_version;

    const bool renderer_v1 = protocol_version != nullptr &&
                             strcmp(protocol_version, kLessonProtocolVersion) == 0;
    const bool renderer_v2 = protocol_version != nullptr &&
                             strcmp(protocol_version, kLessonRendererV2) == 0;
    const bool version_ok = renderer_v1 || renderer_v2;
    const char* profile = Str(body, "profile");
    const bool profile_ok = (profile == nullptr) ||
                            strcmp(profile, kLessonProfileEspTft) == 0;
    const bool valid_prepare_identity =
        IsValidLessonIdentity(assignment_id) && IsValidLessonIdentity(session_id);
    const bool valid_prepare_body = body != nullptr;
    const cJSON* prepare_asset_pack = Obj(body, "assetPack");
    const cJSON* prepare_manifest_ref = Obj(body, "manifestRef");
    const char* prepare_manifest_checksum =
        Str(prepare_manifest_ref, "manifestChecksum");
    const bool prepare_isolated_stream =
        g_session.lesson_asset_generation == 0 || !same_session ||
        prepare_has_newer_assignment_version || restart_prepare;

    auto emit_prepare_error = [&](cJSON* error_body) {
        if (prepare_isolated_stream) {
            emit_isolated_prepare_error(root, error_body);
        } else {
            emit(root, "lesson_error", error_body);
        }
    };

    if (is_prepare && !valid_prepare_identity) {
        emit_prepare_error(MakeErrorBody(
            "LESSON_IDENTITY_INVALID", "lesson identity is invalid", false,
            "invalid_identity"));
        return;
    }
    if (is_prepare && !valid_prepare_body) {
        emit_prepare_error(MakeErrorBody(
            "LESSON_ENVELOPE_INVALID", "lesson_prepare body must be an object", false,
            "body"));
        return;
    }
    if (is_prepare && (!version_ok || !profile_ok)) {
        emit_prepare_error(MakeErrorBody(
            "LESSON_VERSION_UNSUPPORTED",
            version_ok ? "unsupported profile" : "unsupported protocolVersion",
            false,
            version_ok ? "profile" : "contract"));
        ESP_LOGW(TAG, "lesson_prepare rejected: version_ok=%d profile_ok=%d",
                 version_ok, profile_ok);
        return;
    }
    if (is_prepare && prepare_asset_pack != nullptr && Blank(prepare_manifest_checksum)) {
        emit_prepare_error(MakeErrorBody(
            "ASSET_PACK_NOT_READY",
            "lesson_prepare assetPack requires manifestRef.manifestChecksum",
            true,
            "assetPack"));
        ESP_LOGW(TAG, "lesson_prepare rejected: assetPack missing manifestChecksum");
        return;
    }
    if (is_prepare && sequence == 0) {
        emit_prepare_error(MakeErrorBody(
            "LESSON_SEQUENCE_INVALID", "lesson sequence must start at 1", false,
            "sequence"));
        ESP_LOGW(TAG, "lesson_prepare rejected: sequence must start at 1");
        return;
    }

    bool prepare_newly_acquired_asset_session = false;
    std::uint64_t prepare_asset_generation = 0;
    if (is_prepare) {
        const auto reservation =
            LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
                assignment_id, session_id);
        if (!reservation.acquired) {
            const char* error_code = "LESSON_RESERVATION_EXHAUSTED";
            const char* error_message = "lesson storage reservation unavailable";
            const char* error_reason = "generation_exhausted";
            bool retryable = false;
            switch (reservation.code) {
            case LessonAssetReservationCode::kMutationActive:
                error_code = "LESSON_ASSET_MUTATION_ACTIVE";
                error_message = "lesson assets are being updated";
                error_reason = "asset_mutation_active";
                retryable = true;
                break;
            case LessonAssetReservationCode::kLessonSessionMismatch:
            case LessonAssetReservationCode::kLessonSessionActive:
                error_code = "LESSON_SESSION_CONFLICT";
                error_message = "another lesson session owns lesson assets";
                error_reason = "lesson_session_mismatch";
                retryable = true;
                break;
            case LessonAssetReservationCode::kInvalidIdentity:
                // Pure envelope validation makes this unreachable in the handler;
                // retain fail-closed mapping if coordinator validation ever tightens.
                error_code = "LESSON_IDENTITY_INVALID";  // GCOVR_EXCL_LINE
                error_message = "lesson identity is invalid";  // GCOVR_EXCL_LINE
                error_reason = "invalid_identity";  // GCOVR_EXCL_LINE
                break;  // GCOVR_EXCL_LINE
            case LessonAssetReservationCode::kGenerationExhausted:
            case LessonAssetReservationCode::kAcquired:
                break;
            }
            emit_isolated_prepare_error(
                root, MakeErrorBody(error_code, error_message, retryable, error_reason));
            ESP_LOGW(TAG, "lesson_prepare storage reservation refused reason=%s", error_reason);
            return;
        }
        prepare_newly_acquired_asset_session = !reservation.idempotent;
        prepare_asset_generation = reservation.generation;
    }

    auto release_new_lesson_asset_session = [&]() {
        if (!prepare_newly_acquired_asset_session || prepare_asset_generation == 0) return;
        if (LessonAssetStorageCoordinator::GetInstance().EndLessonSession(
                assignment_id, session_id, prepare_asset_generation)) {
            prepare_newly_acquired_asset_session = false;
        }
    };

    // --- session context (per (assignmentId,sessionId)) ---
    // Prepare candidates are still uncommitted here. Reject non-prepare frames outside
    // the active
    // (assignmentId, sessionId) stream before version/profile errors,
    // staleness/dedup, or render handling can mutate the active F->S stream.
    if (!is_prepare && (g_session.assignment_id != assignment_id ||
                        g_session.session_id != session_id)) {
        ESP_LOGW(TAG, "lesson_%s for unknown session; dropping", type + 7);
        return;
    }

    if (!is_prepare && (!version_ok || !profile_ok)) {
        cJSON* eb = MakeErrorBody(
            "LESSON_VERSION_UNSUPPORTED",
            version_ok ? "unsupported profile" : "unsupported protocolVersion",
            false,
            version_ok ? "profile" : "contract");
        end_lesson_after_failure();
        emit(root, "lesson_error", eb);
        ESP_LOGW(TAG, "lesson_* rejected: version_ok=%d profile_ok=%d", version_ok, profile_ok);
        return;
    }

    if (is_prepare) {
        g_session.lesson_asset_generation = prepare_asset_generation;
    }

    const bool is_start = strcmp(type, "lesson_start") == 0;
    const bool is_step = strcmp(type, "lesson_step") == 0;
    const bool is_visual = strcmp(type, "lesson_visual_state") == 0;
    const bool is_pause = strcmp(type, "lesson_pause") == 0;
    const bool is_resume = strcmp(type, "lesson_resume") == 0;
    const bool is_stop = strcmp(type, "lesson" "_stop") == 0;
    const bool is_error = strcmp(type, "lesson" "_error") == 0;
    if (is_start && !g_session.prepared) {
        ESP_LOGW(TAG, "lesson_start outside prepared session; dropping");
        return;
    }
    if (is_step && (!g_session.prepared || !g_session.running)) {
        ESP_LOGW(TAG, "lesson_step outside running session; dropping");
        return;
    }
    if (is_visual && (!g_session.prepared || !g_session.running || !g_session.renderer_v2)) {
        ESP_LOGW(TAG, "lesson_visual_state outside renderer-v2 running session; dropping");
        return;
    }
    if ((is_pause || is_resume) && (!g_session.prepared || !g_session.running)) {
        ESP_LOGW(TAG, "lesson_%s outside running session; dropping", type + 7);
        return;
    }
    if ((is_stop || is_error) && !g_session.prepared) {
        ESP_LOGW(TAG, "lesson_%s outside prepared session; dropping", type + 7);
        return;
    }

    // Staleness drop (plan §7.2): ignore a frame carrying an older body.assignmentVersion.
    double av = 0.0;
    if (Num(body, "assignmentVersion", av)) {
        if ((!is_prepare || same_session) && av < g_session.assignment_version) {
            if (is_prepare) release_new_lesson_asset_session();
            ESP_LOGW(TAG, "lesson_* stale assignmentVersion; dropping");
            return;
        }
        if (!is_prepare) g_session.assignment_version = av;
    }

    // Dedup (plan §5.8 / protocol §6): a sequence <= the last processed value is a
    // duplicate -> re-ack idempotently, no re-render / no re-progress.
    // FW-LESSON-02: idempotency means RE-SENDING THE PRIOR lesson_ack body
    // (lesson-robot-protocol.md:436-438), so replay the cached (rendered, degraded) we
    // emitted for this exact acked sequence instead of hardcoding false/false (which
    // would corrupt the rendered/degraded observability the ack body carries). If the
    // duplicate is older than the single cached entry (slice-01 keeps only the last),
    // fall back to false/false — the conservative non-rendered ack.
    if ((!is_prepare || duplicate_prepare) && sequence <= g_session.last_in_sequence) {
        if (g_session.pending_ack && sequence == g_session.pending_server_sequence) {
            ESP_LOGI(TAG, "lesson_* duplicate seq=%ld still awaits visual completion",
                     static_cast<long>(sequence));
            if (is_prepare) release_new_lesson_asset_session();
            return;
        }
        for (auto it = g_session.ack_history.rbegin(); it != g_session.ack_history.rend(); ++it) {
            if (it->sequence == sequence) {
                cJSON* replay_body = cJSON_Parse(it->body_json.c_str());
                ESP_LOGI(TAG, "lesson_* duplicate seq=%ld; replaying exact ack body",
                         static_cast<long>(sequence));
                LogLessonAckEvidence(root, replay_body);
                emit(root, "lesson_ack", replay_body);
                if (is_prepare) release_new_lesson_asset_session();
                return;
            }
        }
        const bool re_rendered = (sequence == g_session.last_ack_sequence)
                                     ? g_session.last_ack_rendered : false;
        const bool re_degraded = (sequence == g_session.last_ack_sequence)
                                     ? g_session.last_ack_degraded : false;
        cJSON* re_asset_pack = nullptr;
        if (sequence == g_session.last_ack_sequence && !g_session.last_ack_asset_pack_json.empty()) {
            re_asset_pack = cJSON_Parse(g_session.last_ack_asset_pack_json.c_str());
        } else if (is_prepare && sequence == g_session.prepare_ack_sequence &&
                   !g_session.prepare_ack_asset_pack_json.empty()) {
            re_asset_pack = cJSON_Parse(g_session.prepare_ack_asset_pack_json.c_str());
        }
        ESP_LOGI(TAG, "lesson_* duplicate seq=%ld; re-acking rendered=%d degraded=%d",
                 static_cast<long>(sequence), re_rendered, re_degraded);
        emit_ack(root, sequence, re_rendered, re_degraded, re_asset_pack, /*cache*/ false,
                 /*render_elapsed_ms*/ -1,
                 g_session.last_ack_degraded_reason.empty() ? nullptr : g_session.last_ack_degraded_reason.c_str());
        if (is_prepare) release_new_lesson_asset_session();
        return;
    }
    if (is_step && g_session.paused) {
        ESP_LOGW(TAG, "lesson_step while paused; dropping");
        return;
    }
    if (g_session.pending_ack && sequence > g_session.pending_server_sequence &&
        (is_step || is_visual)) {
        g_visual_callback_token.fetch_add(1, std::memory_order_acq_rel);
        LessonQueueItem superseded = MakeLessonVisualQueueItem(
            LessonQueueItemKind::kVisualCompleted,
            g_session.current_transport_epoch,
            g_session.visual_generation,
            g_session.pending_server_sequence,
            g_session.assignment_id.c_str(),
            g_session.session_id.c_str(),
            g_session.pending_step_id.empty() ? nullptr : g_session.pending_step_id.c_str(),
            LessonVisualCompletionResult::kRejected,
            "superseded",
            g_session.pending_visual_nonce);
        std::string superseded_ack;
        if (AcceptLessonVisualCompletion(superseded, &superseded_ack) && protocol_) {
            protocol_->SendLessonFrame(superseded_ack);
        }
    }
    if (!is_prepare) g_session.last_in_sequence = sequence;

    // --- slice subset dispatch ---
    if (is_prepare) {
        if (preload_reset_only) {
            clear_stale_lesson_for_fresh_prepare(false);
            g_session = LessonSession{};
            g_session.assignment_id = assignment_id;
            g_session.session_id = session_id;
            g_session.lesson_asset_generation = prepare_asset_generation;
            g_session.current_transport_epoch = frame_transport_epoch;
            g_session.last_in_sequence = sequence;
            if (prepare_has_assignment_version) {
                g_session.assignment_version = prepare_assignment_version;
            }
            emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
            end_lesson_asset_session();
            return;
        }
        cJSON* asset_pack_ack = BuildAssetPackAck(body);
        const bool candidate_asset_pack_ready =
            prepare_asset_pack == nullptr ||
            (asset_pack_ack != nullptr &&
             cJSON_IsTrue(cJSON_GetObjectItem(asset_pack_ack, "ready")));
        if (!candidate_asset_pack_ready) {
            if (prepare_isolated_stream) {
                emit_isolated_prepare_ack(root, sequence, asset_pack_ack);
            } else {
                emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false,
                         asset_pack_ack, /*cache*/ false);
            }
            release_new_lesson_asset_session();
            return;
        }
        const cJSON* runtime_controls = Obj(body, "runtimeControls");
        const auto verified_asset_paths = VerifiedAssetPaths(body);
        const cJSON* motion_enabled = runtime_controls != nullptr
            ? cJSON_GetObjectItem(runtime_controls, "motionPresetsEnabled")
            : nullptr;
        clear_stale_lesson_for_fresh_prepare(!preload_reset_only);
        g_session = LessonSession{};
        g_session.assignment_id = assignment_id;
        g_session.session_id = session_id;
        g_session.lesson_asset_generation = prepare_asset_generation;
        g_session.current_transport_epoch = frame_transport_epoch;
        g_session.last_in_sequence = sequence;
        if (prepare_has_assignment_version) {
            g_session.assignment_version = prepare_assignment_version;
        }
        // Defense in depth: every fresh manifest starts disabled. Only the explicit
        // runtime control boolean can arm lesson motion for this session.
        g_session.motion_presets_enabled = !renderer_v2 && cJSON_IsTrue(motion_enabled);
        g_session.renderer_v2 = renderer_v2;
        g_session.verified_asset_paths = verified_asset_paths;
        g_session.prepared = true;
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false, asset_pack_ack);
        return;
    }
    if (strcmp(type, "lesson_start") == 0) {
        if (g_session.renderer_v2) {
            const cJSON* opening_entrance = Obj(body, "openingEntrance");
            if (!ValidOpeningEntrance(opening_entrance)) {
                emit(root, "lesson_error", MakeErrorBody(
                    "LESSON_FRAME_INVALID",
                    "lesson_start openingEntrance is invalid",
                    false,
                    "unsupportedContract"));
                return;
            }
            if (g_session.opening_entrance_consumed) {
                emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
                return;
            }
        }
        g_session.running = true;
        g_session.paused = false;
        Application::GetInstance().SetLessonRuntimeActive(true);
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        g_layer_state.ClearAll();
        std::uint64_t visual_callback_token = 0;
        if (g_session.renderer_v2) {
            g_session.opening_entrance_consumed = true;
            g_session.entrance_active = true;
            visual_callback_token =
                g_visual_callback_token.fetch_add(1, std::memory_order_acq_rel) + 1;
            ++g_session.visual_generation;
            g_session.pending_server_sequence = sequence;
            g_session.pending_visual_nonce = NextVisualCompletionNonce();
            g_session.pending_protocol_version = protocol_version;
            const char* lesson_id = Str(root, "lessonId");
            g_session.pending_lesson_id = lesson_id != nullptr ? lesson_id : "";
            g_session.pending_has_lesson_version =
                Num(root, "lessonVersion", g_session.pending_lesson_version);
            g_session.pending_step_id.clear();
            g_session.pending_ack = true;
        }
        // Clear stale layers, enter lesson mode, then show a short child-visible loading
        // cue until the first real lesson_step redraws the authored scene.
        std::shared_ptr<std::unique_ptr<LvglImage>> opening_background;
        std::shared_ptr<std::unique_ptr<LvglImage>> opening_robot;
        bool opening_asset_identity_mismatch = false;
        if (g_session.renderer_v2 && !g_session.verified_asset_paths.empty()) {
            const cJSON* opening_entrance = Obj(body, "openingEntrance");
            const char* background_key = Str(opening_entrance, "backgroundAssetKey");
            const char* robot_key = Str(opening_entrance, "robotAssetKey");
            const auto background_path = g_session.verified_asset_paths.find(
                background_key != nullptr ? background_key : "");
            const auto robot_path = g_session.verified_asset_paths.find(
                robot_key != nullptr ? robot_key : "");
            opening_asset_identity_mismatch =
                background_path == g_session.verified_asset_paths.end() ||
                robot_path == g_session.verified_asset_paths.end();
            if (!opening_asset_identity_mismatch) {
                if (auto image = FetchLessonImage(background_path->second.c_str())) {
                    opening_background =
                        std::make_shared<std::unique_ptr<LvglImage>>(std::move(image));
                }
                if (auto image = FetchLessonImage(robot_path->second.c_str())) {
                    opening_robot =
                        std::make_shared<std::unique_ptr<LvglImage>>(std::move(image));
                }
            }
        }
        Display* start_display = Board::GetInstance().GetDisplay();
        LvglDisplay* start_lvgl = dynamic_cast<LvglDisplay*>(start_display);
        if (start_display) {
            const bool renderer_v2_callback = g_session.renderer_v2;
            const std::uint64_t callback_transport_epoch = g_session.current_transport_epoch;
            const std::uint64_t callback_visual_generation = g_session.visual_generation;
            const std::uint64_t callback_visual_nonce = g_session.pending_visual_nonce;
            const std::string callback_assignment_id = g_session.assignment_id;
            const std::string callback_session_id = g_session.session_id;
            const char* opening_layout = Str(Obj(body, "openingEntrance"), "layoutPreset");
            const std::string callback_layout = opening_layout != nullptr ? opening_layout : "";
            Schedule([start_display, start_lvgl, renderer_v2_callback, visual_callback_token,
                      callback_transport_epoch, callback_visual_generation, sequence,
                      callback_visual_nonce, callback_assignment_id, callback_session_id,
                      callback_layout, opening_background, opening_robot,
                      opening_asset_identity_mismatch]() mutable {
                if (renderer_v2_callback &&
                    g_visual_callback_token.load(std::memory_order_acquire) !=
                        visual_callback_token) {
                    return;
                }
                if (start_lvgl) start_lvgl->CancelLessonRobotEntrance();
                if (start_lvgl && opening_background) {
                    start_lvgl->SetLessonBackground(std::move(*opening_background));
                } else if (start_lvgl) {
                    start_lvgl->SetLessonBackground(nullptr);
                }
                if (start_lvgl) start_lvgl->SetLessonObject(nullptr);
                if (start_lvgl && opening_robot) {
                    start_lvgl->SetLessonRobotOverlay(std::move(*opening_robot));
                } else if (start_lvgl) {
                    start_lvgl->SetLessonRobotOverlay(nullptr);
                }
                if (start_lvgl) start_lvgl->SetLessonTeachingWord("");
                start_display->SetLessonCaption("");
                start_display->ClearChatMessages();
                if (start_lvgl) start_lvgl->SetLessonMode(true);
                start_display->SetStatus(Lang::Strings::PLEASE_WAIT);
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                if (renderer_v2_callback && start_lvgl) {
                    if (opening_asset_identity_mismatch) {
                        Application::GetInstance().EnqueueLessonVisualCompletion(
                            LessonQueueItemKind::kVisualCompleted, callback_transport_epoch,
                            callback_visual_generation, sequence, callback_assignment_id.c_str(),
                            callback_session_id.c_str(), nullptr,
                            LessonVisualCompletionResult::kRejected, "assetIdentityMismatch",
                            callback_visual_nonce);
                        return;
                    }
                    start_lvgl->StartLessonRobotEntrance(
                        {callback_layout.c_str(), false},
                        [callback_transport_epoch, callback_visual_generation, sequence,
                         callback_visual_nonce, callback_assignment_id, callback_session_id](
                            LessonVisualApplyResult result, const char* degraded_reason) {
                            Application::GetInstance().EnqueueLessonVisualCompletion(
                                result == LessonVisualApplyResult::kPhaseTimeout
                                    ? LessonQueueItemKind::kVisualTimedOut
                                    : LessonQueueItemKind::kVisualCompleted,
                                callback_transport_epoch, callback_visual_generation, sequence,
                                callback_assignment_id.c_str(), callback_session_id.c_str(), nullptr,
                                QueueCompletionResult(result), degraded_reason,
                                callback_visual_nonce);
                        });
                }
            });
        }
        if (g_session.renderer_v2) {
            if (start_lvgl == nullptr) {
                Application::GetInstance().EnqueueLessonVisualCompletion(
                    LessonQueueItemKind::kVisualCompleted,
                    g_session.current_transport_epoch,
                    g_session.visual_generation,
                    sequence,
                    g_session.assignment_id.c_str(),
                    g_session.session_id.c_str(),
                    nullptr,
                    LessonVisualCompletionResult::kRejected,
                    "unsupportedContract",
                    g_session.pending_visual_nonce);
            }
            return;
        }
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        return;
    }
    if (strcmp(type, "lesson_pause") == 0) {
        InvalidateLessonVisualCompletionState(g_session.current_transport_epoch);
        g_session.opening_entrance_consumed = true;
        g_session.paused = true;
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            const bool renderer_v2_pause = g_session.renderer_v2;
            Schedule([display, lvgl_display, renderer_v2_pause]() {
                if (lvgl_display) lvgl_display->CancelLessonRobotEntrance();
                if (renderer_v2_pause && lvgl_display) {
                    lvgl_display->ApplyLessonVisualState(
                        {LessonVisualStateKind::kTeach, true}, nullptr);
                    lvgl_display->SetLessonMode(true);
                } else {
                    if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                    if (lvgl_display) lvgl_display->SetLessonObject(nullptr);
                    if (lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
                    if (lvgl_display) lvgl_display->SetLessonTeachingWord("");
                    if (lvgl_display) lvgl_display->SetLessonMode(false);
                    display->SetLessonCaption("");
                }
                display->ClearChatMessages();
                display->SetStatus("Tạm dừng bài học");
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
            });
        }
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        return;
    }
    if (strcmp(type, "lesson_resume") == 0) {
        g_session.paused = false;
        Application::GetInstance().SetLessonRuntimeActive(true);
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            const bool renderer_v2_resume = g_session.renderer_v2;
            Schedule([display, lvgl_display, renderer_v2_resume]() {
                if (lvgl_display) lvgl_display->CancelLessonRobotEntrance();
                if (renderer_v2_resume && lvgl_display) {
                    lvgl_display->ApplyLessonVisualState(
                        {LessonVisualStateKind::kTeach, true}, nullptr);
                    lvgl_display->SetLessonMode(true);
                } else {
                    if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                    if (lvgl_display) lvgl_display->SetLessonObject(nullptr);
                    if (lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
                    if (lvgl_display) lvgl_display->SetLessonTeachingWord("");
                    if (lvgl_display) lvgl_display->SetLessonMode(false);
                    display->SetLessonCaption("");
                }
                display->ClearChatMessages();
                display->SetStatus("Đang học...");
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
            });
        }
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        return;
    }
    if (strcmp(type, "lesson_stop") == 0) {
        InvalidateLessonVisualCompletionState(g_session.current_transport_epoch);
        const char* stop_reason = Str(body, "reason");
        const std::string normalized_stop_reason = NormalizeAsciiToken(stop_reason);
        const bool stop_cancelled = normalized_stop_reason == "CANCELLED";
        const bool stop_explicit_failed = normalized_stop_reason == "FAILED";
        const bool stop_completed = stop_reason == nullptr ||
                                    normalized_stop_reason == "COMPLETED" ||
                                    normalized_stop_reason == "SUCCEEDED";
        const bool stop_failed = stop_explicit_failed || (!stop_cancelled && !stop_completed);
        const char* stop_status = stop_failed ? Lang::Strings::ERROR
                                 : stop_cancelled ? "Bài học đã dừng"
                                                  : "Hoàn thành bài học";
        const char* stop_emotion = stop_failed ? "sad"
                                  : stop_cancelled ? "neutral"
                                                   : "happy";
        const char* stop_message = stop_failed ? "Bài học bị gián đoạn."
                                 : stop_cancelled ? "Bài học đã dừng."
                                                  : nullptr;
        const std::string_view stop_sound = stop_failed ? Lang::Sounds::OGG_EXCLAMATION
                                          : stop_cancelled ? Lang::Sounds::OGG_POPUP
                                                           : Lang::Sounds::OGG_SUCCESS;
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        Application::GetInstance().SetLessonRuntimeActive(false);
        g_session.running = false;
        g_session.paused = false;
        g_session.prepared = false;
        g_layer_state.ClearAll();
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        end_lesson_asset_session();
        ClearTerminalLessonCursor();
        // Return the robot display to its realtime face surface (protocol §4.6):
        // clear persistent lesson layers and make the terminal reason perceivable.
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            Schedule([display, lvgl_display, stop_status, stop_emotion, stop_message, stop_sound]() {
                if (lvgl_display) lvgl_display->CancelLessonRobotEntrance();
                if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                if (lvgl_display) lvgl_display->SetLessonObject(nullptr);
                if (lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
                if (lvgl_display) lvgl_display->SetLessonTeachingWord("");
                if (lvgl_display) lvgl_display->SetLessonMode(false);
                display->SetLessonCaption("");
                display->SetStatus(stop_status);
                display->SetEmotion(stop_emotion);
                display->ClearChatMessages();
                if (stop_message != nullptr) display->SetChatMessage("assistant", stop_message);
                Application::GetInstance().PlaySound(stop_sound);
            });
        }
        return;
    }
    if (strcmp(type, "lesson_error") == 0) {
        // ESP can send a terminal lesson_error when it rejects an unsafe lesson payload
        // before it reaches the renderer. Do not ack this status frame back; clear stale
        // layers and show a child-safe failure message instead of leaving the last step
        // frozen on screen.
        end_lesson_after_failure();
        return;
    }
    if (is_visual) {
        std::uint64_t requested_generation = 0;
        if (!ValidVisualStateContract(root, &requested_generation)) {
            emit(root, "lesson_error", MakeErrorBody(
                "LESSON_FRAME_INVALID",
                "lesson_visual_state body is invalid",
                false,
                "unsupportedContract"));
            return;
        }
        g_session.entrance_active = false;
        const std::uint64_t visual_callback_token =
            g_visual_callback_token.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_session.visual_generation = requested_generation;
        g_session.pending_server_sequence = sequence;
        g_session.pending_visual_nonce = NextVisualCompletionNonce();
        g_session.pending_protocol_version = protocol_version;
        const char* lesson_id = Str(root, "lessonId");
        g_session.pending_lesson_id = lesson_id != nullptr ? lesson_id : "";
        g_session.pending_has_lesson_version =
            Num(root, "lessonVersion", g_session.pending_lesson_version);
        g_session.pending_step_id = Str(root, "stepId");
        g_session.pending_ack = true;
        Display* display = Board::GetInstance().GetDisplay();
        if (display == nullptr) {
            Application::GetInstance().EnqueueLessonVisualCompletion(
                LessonQueueItemKind::kVisualCompleted,
                g_session.current_transport_epoch,
                g_session.visual_generation,
                sequence,
                g_session.assignment_id.c_str(),
                g_session.session_id.c_str(),
                g_session.pending_step_id.c_str(),
                LessonVisualCompletionResult::kRejected,
                "unsupportedContract",
                g_session.pending_visual_nonce);
            return;
        }
        const auto presentation = VisualStatePresentation(Str(body, "state"));
        const std::uint64_t callback_transport_epoch = g_session.current_transport_epoch;
        const std::uint64_t callback_visual_generation = g_session.visual_generation;
        const std::uint64_t callback_visual_nonce = g_session.pending_visual_nonce;
        const std::string callback_assignment_id = g_session.assignment_id;
        const std::string callback_session_id = g_session.session_id;
        const std::string callback_step_id = g_session.pending_step_id;
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        const LessonVisualStateKind visual_state = VisualStateKind(Str(body, "state"));
        Schedule([display, lvgl_display, presentation, visual_state, visual_callback_token,
                  callback_transport_epoch, callback_visual_generation, callback_visual_nonce,
                  sequence, callback_assignment_id, callback_session_id, callback_step_id]() {
            if (g_visual_callback_token.load(std::memory_order_acquire) !=
                visual_callback_token) {
                return;
            }
            display->SetEmotion(presentation.emotion);
            display->SetStatus(presentation.status);
            if (lvgl_display) {
                lvgl_display->ApplyLessonVisualState(
                    {visual_state, true},
                    [callback_transport_epoch, callback_visual_generation, sequence,
                     callback_visual_nonce, callback_assignment_id, callback_session_id,
                     callback_step_id](
                        LessonVisualApplyResult result, const char* degraded_reason) {
                        Application::GetInstance().EnqueueLessonVisualCompletion(
                            result == LessonVisualApplyResult::kPhaseTimeout
                                ? LessonQueueItemKind::kVisualTimedOut
                                : LessonQueueItemKind::kVisualCompleted,
                            callback_transport_epoch, callback_visual_generation, sequence,
                            callback_assignment_id.c_str(), callback_session_id.c_str(),
                            callback_step_id.c_str(), QueueCompletionResult(result), degraded_reason,
                            callback_visual_nonce);
                    });
                return;
            }
            Application::GetInstance().EnqueueLessonVisualCompletion(
                LessonQueueItemKind::kVisualCompleted,
                callback_transport_epoch,
                callback_visual_generation,
                sequence,
                callback_assignment_id.c_str(),
                callback_session_id.c_str(),
                callback_step_id.c_str(),
                LessonVisualCompletionResult::kDegraded,
                "missingOverlay",
                callback_visual_nonce);
        });
        return;
    }
    if (strcmp(type, "lesson_step") != 0) {
        // F->S/ESP-synth frames (ack/error/progress/preload_status) are not part
        // of the S->F command subset handled by this renderer.
        ESP_LOGW(TAG, "lesson_%s not handled in slice; dropping", type + 7);
        return;
    }

    // =====================  lesson_step (render s4 "model")  ====================
    const char* lesson_id = Str(root, "lessonId");
    const char* step_id = Str(root, "stepId");
    double lesson_version = -1.0;
    Num(root, "lessonVersion", lesson_version);
    ESP_LOGI(TAG,
             "lesson_step_started assignmentId=%s sessionId=%s lessonId=%s "
             "lessonVersion=%.0f stepId=%s sequence=%ld",
             assignment_id, session_id, lesson_id != nullptr ? lesson_id : "-",
             lesson_version, step_id != nullptr ? step_id : "-", static_cast<long>(sequence));

    const cJSON* scene = Obj(body, "scene");
    const cJSON* bg    = Obj(scene, "backgroundScene");
    const cJSON* to    = Obj(scene, "teachingObject");
    const cJSON* ro    = Obj(scene, "robotOverlay");
    const cJSON* poster_asset = Obj(bg, "poster");
    const cJSON* object_asset = Obj(to, "asset");
    const cJSON* overlay_asset = Obj(ro, "asset");
    const cJSON* raw_template_projection = body != nullptr
        ? cJSON_GetObjectItem(body, "templateProjection") : nullptr;
    const cJSON* tvideo_projection = cJSON_IsObject(raw_template_projection)
        ? raw_template_projection : nullptr;

    // espTft forbids a critical background video — poster only (plan §7.4 /
    // DIV-FW-EXPORT). A manifest forcing video -> ASSET_PROFILE_UNAVAILABLE, render
    // nothing. (Step-addressed error: stepId echoed from the inbound frame.)
    const char* bg_mode = Str(bg, "mode");
    const std::string normalized_bg_mode = NormalizeAsciiToken(bg_mode);
    const cJSON* bg_video = (bg != nullptr) ? cJSON_GetObjectItem(bg, "video") : nullptr;
    const bool video_forced = (bg_mode != nullptr && normalized_bg_mode != "POSTER") ||
                              (bg_video != nullptr && !cJSON_IsNull(bg_video));
    if (video_forced) {
        cJSON* eb = MakeErrorBody("ASSET_PROFILE_UNAVAILABLE",
                                  "espTft requires a poster background (video forbidden)",
                                  false, "profile");
        end_lesson_after_failure();
        emit(root, "lesson_error", eb);
        ESP_LOGW(TAG, "lesson_step rejected: video forced on espTft");
        return;
    }
    if (HasForbiddenMotionMedia(poster_asset) || HasForbiddenMotionMedia(object_asset) ||
        HasForbiddenMotionMedia(overlay_asset) ||
        HasForbiddenMotionMedia(Obj(tvideo_projection, "background")) ||
        HasForbiddenMotionMedia(Obj(tvideo_projection, "arrivedPose")) ||
        HasForbiddenMotionMedia(Obj(tvideo_projection, "atlas"))) {
        cJSON* eb = MakeErrorBody("ASSET_PROFILE_UNAVAILABLE",
                                  "espTft forbids authored motion-media lesson assets",
                                  false, "profile");
        end_lesson_after_failure();
        emit(root, "lesson_error", eb);
        ESP_LOGW(TAG, "lesson_step rejected: authored motion-media asset");
        return;
    }

    // Degraded fallback ladder: poster -> teachingObject PNG -> primitive glyph
    // card -> emoji-face + caption. US-006 image render: instead of only probing the
    // on-device flashed asset image (which never holds per-assignment lesson posters),
    // we now FETCH the authored URL (scene.backgroundScene.poster.src) over HTTP and
    // DECODE it, reusing the proven mcp_server.cc download->LvglAllocatedImage path.
    // The fetched poster bytes are drawn as a full-screen, persistent background via
    // LcdDisplay::SetLessonBackground (distinct from the centered 5s auto-hide preview).
    // The teachingObject asset is fetched through the same verified local URL and drawn
    // as the foreground object layer via LcdDisplay::SetLessonObject. Every layer draws
    // only through FetchLessonImage(); a poster that is merely flashed-present but never
    // decoded/drawn is treated as not-drawn (see the poster_drew block below).
    const char* poster_src = Str(poster_asset, "src");
    const char* poster_key = Str(poster_asset, "key");
    const char* object_src = Str(object_asset, "src");
    const char* object_key = Str(object_asset, "key");
    const char* overlay_src = Str(overlay_asset, "src");
    const char* overlay_key = Str(overlay_asset, "key");
    const bool has_object_src = !Blank(object_src);
    bool has_overlay_src = !Blank(overlay_src);
    bool tvideo_degraded = false;
    const char* tvideo_degraded_reason = nullptr;
    lesson_tvideo::Rect tvideo_arrived_bounds{};
    bool tvideo_use_bounds = false;
    bool tvideo_arrived_pose_linked = false;
    const bool is_first_lesson_step = !g_session.first_lesson_step_seen;
    g_session.first_lesson_step_seen = true;
    const char* projection_template_id = Str(tvideo_projection, "templateId");
    const bool malformed_template_projection = raw_template_projection != nullptr &&
        (tvideo_projection == nullptr || Blank(projection_template_id));
    const bool is_tvideo_projection = malformed_template_projection ||
        (projection_template_id != nullptr &&
         std::strcmp(projection_template_id, "tvideoFlyWalk") == 0);
    if (is_tvideo_projection) {
        const bool contract_valid = is_first_lesson_step && ValidTvideoProjection(tvideo_projection);
        const cJSON* pinned_arrived_pose = Obj(tvideo_projection, "arrivedPose");
        tvideo_arrived_pose_linked = contract_valid && has_overlay_src &&
            MatchesPinnedAsset(overlay_asset, pinned_arrived_pose);
        has_overlay_src = tvideo_arrived_pose_linked;
        const char* layout_preset = Str(tvideo_projection, "layoutPreset");
        lesson_tvideo::StateMachine tvideo({
            "tvideoFlyWalk",
            contract_valid ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
            layout_preset,
            contract_valid ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
            tvideo_arrived_pose_linked,
            false,
            false,
        });
        tvideo_degraded = true;
        tvideo_degraded_reason = lesson_tvideo::DegradedReasonName(tvideo.degraded_reason());
        if (tvideo_arrived_pose_linked) {
            if (const lesson_tvideo::LayoutGeometry* geometry =
                    lesson_tvideo::ArrivedGeometry(layout_preset, 1)) {
                tvideo_arrived_bounds = geometry->robot;
                tvideo_use_bounds = true;
            }
        }
        ESP_LOGI(TAG, "lesson_step tvideo fallback reveal=%d degraded=%d reason=%s",
                 tvideo.content_visible(), tvideo_degraded, tvideo_degraded_reason);
    }
    const char* prompt = Str(body, "prompt");
    const char* story_ask = Str(Obj(body, "storyBeat"), "ask");
    const char* step_type       = Str(body, "stepType");
    const char* completion_class = Str(body, "completionClass");
    const bool passive = IsPassiveStep(completion_class, step_type);
    std::string prompt_caption;
    if (!passive) prompt_caption = CaptionCandidate(story_ask);
    if (prompt_caption.empty()) prompt_caption = CaptionCandidate(prompt);
    if (!passive && prompt_caption.empty()) prompt_caption = CaptionCandidate(story_ask);
    if (passive && LooksLikeChildQuestionCaption(prompt_caption)) prompt_caption.clear();

    if (scene == nullptr || bg == nullptr || to == nullptr || ro == nullptr ||
        Blank(poster_src)) {
        cJSON* eb = MakeErrorBody("LESSON_FRAME_INVALID",
                                  "lesson_step requires scene and backgroundScene poster image source",
                                  false, "scene");
        end_lesson_after_failure();
        emit(root, "lesson_error", eb);
        ESP_LOGW(TAG, "lesson_step rejected: missing required scene or poster image source");
        return;
    }

    const cJSON* teaching_word = Obj(body, "teachingWord");
    const char* teaching_word_value = Str(teaching_word, "displayText");
    if (Blank(teaching_word_value)) teaching_word_value = Str(teaching_word, "text");
    std::string normalized_teaching_word;
    if (!NormalizeLessonTeachingWord(teaching_word_value, normalized_teaching_word)) {
        cJSON* eb = MakeErrorBody("LESSON_FRAME_INVALID",
                                  "teachingWord displayText exceeds 12 codepoints or is invalid UTF-8",
                                  false, "teachingWord");
        end_lesson_after_failure();
        emit(root, "lesson_error", eb);
        return;
    }

    bool motion_degraded = false;
    const char* motion_dispatch = nullptr;
    const char* entry_motion = Str(Obj(body, "motion"), "present");
    if (entry_motion != nullptr) {
        if (g_session.motion_presets_enabled) {
            motion_degraded = DispatchLessonMotionPreset(robot_uart_, entry_motion) ==
                              LessonMotionResult::kDegraded;
            motion_dispatch = motion_degraded ? "failed" : "success";
        } else {
            motion_dispatch = "skipped";
        }
        ESP_LOGI(TAG,
                 "motion_preset outcome=%s assignmentId=%s sessionId=%s lessonId=%s "
                 "stepId=%s sequence=%ld",
                 motion_dispatch, assignment_id, session_id,
                 lesson_id != nullptr ? lesson_id : "-", step_id != nullptr ? step_id : "-",
                 static_cast<long>(sequence));
    }
    if (motion_degraded) {
        ESP_LOGW(TAG,
                 "motion_degraded assignmentId=%s sessionId=%s lessonId=%s stepId=%s "
                 "sequence=%ld",
                 assignment_id, session_id, lesson_id != nullptr ? lesson_id : "-",
                 step_id != nullptr ? step_id : "-", static_cast<long>(sequence));
    }

    // Resolve the display once and require it to be an LvglDisplay (the only class with
    // a real image draw path; OledDisplay/NoDisplay get caption-only). dynamic_cast
    // mirrors mcp_server.cc:255 and yields nullptr on non-LVGL boards.
    Display* base_display = Board::GetInstance().GetDisplay();
    LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(base_display);

    const int64_t render_start_us = esp_timer_get_time();
    SystemInfo::StartHeapPhaseMonitor();
    Application::GetInstance().BeginLessonNetworkRenderQuiet();
    struct LessonNetworkRenderQuietGuard {
        ~LessonNetworkRenderQuietGuard() {
            Application::GetInstance().EndLessonNetworkRenderQuiet();
        }
    } lesson_network_render_quiet_guard;

    // Try to fetch + decode the authored poster. On any failure FetchLessonImage
    // returns nullptr and we fall through to the caption-only render (never crash).
    const LessonLayerPlan background_plan =
        g_layer_state.Plan(LessonLayer::kBackground, poster_key, poster_src);
    const LessonLayerPlan object_plan =
        g_layer_state.Plan(LessonLayer::kObject, object_key, object_src);
    const LessonLayerPlan overlay_plan =
        g_layer_state.Plan(LessonLayer::kOverlay, overlay_key, overlay_src);
    constexpr int kLessonLayerInstallTimeoutMs = 300;
    bool background_clear_ok = true;
    bool object_clear_ok = true;
    bool overlay_clear_ok = true;
    if (lvgl_display != nullptr) {
        if (background_plan.clear_before_load) {
            background_clear_ok = Application::GetInstance().ScheduleAndWait(
                [lvgl_display]() { lvgl_display->SetLessonBackground(nullptr); return true; },
                kLessonLayerInstallTimeoutMs);
            if (background_clear_ok) g_layer_state.Clear(LessonLayer::kBackground);
        }
        if (object_plan.clear_before_load || (!has_object_src && g_layer_state.Has(LessonLayer::kObject))) {
            object_clear_ok = Application::GetInstance().ScheduleAndWait(
                [lvgl_display]() { lvgl_display->SetLessonObject(nullptr); return true; },
                kLessonLayerInstallTimeoutMs);
            if (object_clear_ok) g_layer_state.Clear(LessonLayer::kObject);
        }
        if (overlay_plan.clear_before_load || (!has_overlay_src && g_layer_state.Has(LessonLayer::kOverlay))) {
            overlay_clear_ok = Application::GetInstance().ScheduleAndWait(
                [lvgl_display]() { lvgl_display->SetLessonRobotOverlay(nullptr); return true; },
                kLessonLayerInstallTimeoutMs);
            if (overlay_clear_ok) g_layer_state.Clear(LessonLayer::kOverlay);
        }
    }

    bool poster_drew = background_plan.reused;
    if (lvgl_display != nullptr && poster_src != nullptr && background_clear_ok) {
        std::unique_ptr<LvglImage> bg_image = background_plan.reused ? nullptr : FetchLessonImage(poster_src);
        if (bg_image != nullptr) {
            auto holder = std::make_shared<std::unique_ptr<LvglImage>>(std::move(bg_image));
            poster_drew = Application::GetInstance().ScheduleAndWait(
                [lvgl_display, holder]() mutable {
                    lvgl_display->SetLessonBackground(std::move(*holder));
                    return true;
                }, kLessonLayerInstallTimeoutMs);  // GCOVR_EXCL_LINE
            if (poster_drew) g_layer_state.Commit(LessonLayer::kBackground, poster_key, poster_src);
            ESP_LOGI(TAG, "lesson_step poster fetched+drawn from URL");
        } else if (!background_plan.reused) {
            ESP_LOGW(TAG, "lesson_step poster fetch failed; caption-only fallback");
        }
    }
    // NOTE: a flashed-only poster is NOT treated as drawn here. AssetAvailable() merely
    // probes GetAssetData() presence and discards the bytes; it never decodes nor calls
    // SetLessonBackground(). Setting poster_drew=true on presence alone acked a blank,
    // invisible poster as drawn (degraded=false) and suppressed the stale-poster clear
    // (clear_bg = !poster_drew). A poster with no real draw path must stay not-drawn so
    // the honest degraded ack fires and the previous-step background is cleared.
    bool object_drew = object_plan.reused && has_object_src;
    if (lvgl_display != nullptr && has_object_src && object_clear_ok) {
        std::unique_ptr<LvglImage> object_image = object_plan.reused ? nullptr : FetchLessonImage(object_src);
        if (object_image != nullptr) {
            auto holder = std::make_shared<std::unique_ptr<LvglImage>>(std::move(object_image));
            object_drew = Application::GetInstance().ScheduleAndWait(
                [lvgl_display, holder]() mutable { lvgl_display->SetLessonObject(std::move(*holder)); return true; },
                kLessonLayerInstallTimeoutMs);
            if (object_drew) g_layer_state.Commit(LessonLayer::kObject, object_key, object_src);
            ESP_LOGI(TAG, "lesson_step teaching object fetched+drawn from URL");
        } else if (!object_plan.reused) {
            ESP_LOGW(TAG, "lesson_step teaching object fetch failed; caption fallback");
        }
    }
    bool overlay_drew = overlay_plan.reused && has_overlay_src;
    if (lvgl_display != nullptr && has_overlay_src && overlay_clear_ok) {
        std::unique_ptr<LvglImage> overlay_image = overlay_plan.reused ? nullptr : FetchLessonImage(overlay_src);
        if (overlay_image != nullptr) {
            auto holder = std::make_shared<std::unique_ptr<LvglImage>>(std::move(overlay_image));
            overlay_drew = Application::GetInstance().ScheduleAndWait(
                [lvgl_display, holder, tvideo_use_bounds, tvideo_arrived_bounds]() mutable {
                    if (tvideo_use_bounds) {
                        lvgl_display->SetLessonRobotOverlayBounds(
                            tvideo_arrived_bounds.left, tvideo_arrived_bounds.top,
                            tvideo_arrived_bounds.width, tvideo_arrived_bounds.height);
                    } else {
                        lvgl_display->SetLessonRobotOverlayBounds(0, 0, 0, 0);
                    }
                    lvgl_display->SetLessonRobotOverlay(std::move(*holder));
                    return true;
                }, kLessonLayerInstallTimeoutMs);  // GCOVR_EXCL_LINE
            if (overlay_drew) g_layer_state.Commit(LessonLayer::kOverlay, overlay_key, overlay_src);
            ESP_LOGI(TAG, "lesson_step robot overlay fetched+drawn from URL");
        } else if (!overlay_plan.reused) {
            ESP_LOGW(TAG, "lesson_step robot overlay fetch failed; emoji fallback");
        }
    }
    if (tvideo_arrived_pose_linked && !overlay_drew) {
        tvideo_degraded_reason = "missingOverlay";
        tvideo_use_bounds = false;
    }

    const cJSON* card = Obj(to, "primitiveFallbackCard");
    const char* glyph = Str(card, "glyph");
    const char* label = Str(card, "label");
    if (label == nullptr) label = Str(to, "primaryWord");
    const char* alt = Str(bg, "altCaption");

    // Caption line — AUTHORED lesson content only (COPPA-safe; never child speech,
    // never logged). When Layer-2 falls to the glyph card, fold glyph+label in.
    std::string caption;
    const bool has_caption_prompt = !prompt_caption.empty();
    if (has_caption_prompt) caption = prompt_caption;
    if (!has_caption_prompt) {
        const std::string glyph_caption = CaptionCandidate(glyph);
        const std::string label_caption = CaptionCandidate(label);
        const std::string alt_caption = CaptionCandidate(alt);
        if (!object_drew) {
            if (!glyph_caption.empty()) { caption += glyph_caption; caption += ' '; }
            if (!label_caption.empty()) caption += label_caption;
        } else if (!label_caption.empty()) {
            caption = label_caption;
        }
        if (!alt_caption.empty()) {
            if (!caption.empty()) caption += " - ";
            caption += alt_caption;
        }
    }
    NormalizeCaptionLine(caption);

    // §7.5 degraded semantics: false only when all authored visual layers exist and drew.
    // Missing optional object/overlay sources use caption/emoji fallback and stay degraded.
    const bool optional_asset_missing = !has_object_src || !has_overlay_src;
    const bool render_degraded =
        !(poster_drew && has_object_src && object_drew && has_overlay_src && overlay_drew);
    const bool degraded = tvideo_degraded || motion_degraded || render_degraded;

    // Marshal the draw onto the LVGL task, exactly like the TTS-display pattern
    // (application.cc SetChatMessage Schedule). Layer-3 emoji-face + the caption are
    // ALWAYS drawn (over the poster/object layers when fetched). If THIS step drew no
    // media layer, clear any previous lesson layer so a caption-only step is not shown
    // over a stale picture.
    Display* display = base_display;
    const bool rendered = display != nullptr && dynamic_cast<NoDisplay*>(display) == nullptr;
    const bool has_visible_content = rendered &&
        (!caption.empty() || poster_drew || object_drew || overlay_drew);
    if (display) {
        const bool clear_bg = !poster_drew;
        const bool clear_object = !object_drew;
        const bool clear_overlay = !overlay_drew;
        Schedule([display, lvgl_display, clear_bg, clear_object, clear_overlay,
                  tvideo_use_bounds, tvideo_arrived_bounds,
                  has_visible_content, cap = caption,
                  word = normalized_teaching_word]() {
            if (lvgl_display) lvgl_display->CancelLessonRobotEntrance();
            if (lvgl_display) lvgl_display->SetLessonMode(has_visible_content);
            if (clear_bg && lvgl_display) lvgl_display->SetLessonBackground(nullptr);
            if (clear_object && lvgl_display) lvgl_display->SetLessonObject(nullptr);
            if (lvgl_display) {
                if (tvideo_use_bounds) {
                    lvgl_display->SetLessonRobotOverlayBounds(
                        tvideo_arrived_bounds.left, tvideo_arrived_bounds.top,
                        tvideo_arrived_bounds.width, tvideo_arrived_bounds.height);
                } else {
                    lvgl_display->SetLessonRobotOverlayBounds(0, 0, 0, 0);
                }
            }
            if (clear_overlay && lvgl_display) {
                lvgl_display->SetLessonRobotOverlay(nullptr);
            }
            if (lvgl_display) lvgl_display->SetLessonTeachingWord(word.c_str());
            display->ClearChatMessages();
            if (!has_visible_content) {
                display->SetStatus(Lang::Strings::ERROR);
                display->SetLessonCaption("");
                display->SetChatMessage("assistant", "Bài học chưa tải được.");
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
                return;
            }
            display->SetStatus("Đang học...");
            display->SetLessonCaption(cap.c_str());
        });
    }
    const int64_t render_elapsed_us = esp_timer_get_time() - render_start_us;
    const int64_t render_elapsed_raw_ms = render_elapsed_us < 0 ? 0 : render_elapsed_us / 1000;
    const int64_t render_elapsed_ms = render_elapsed_raw_ms > kLessonRenderElapsedMaxMs
                                          ? kLessonRenderElapsedMaxMs
                                          : render_elapsed_raw_ms;
    SystemInfo::PrintHeapCheckpoint("lesson_render.complete");
    SystemInfo::StopHeapPhaseMonitor();

    cJSON* telemetry = cJSON_CreateObject();
    cJSON_AddNumberToObject(telemetry, "internalFreeBytes",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    cJSON_AddNumberToObject(telemetry, "internalMinimumFreeBytes",
                            static_cast<double>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    cJSON_AddNumberToObject(telemetry, "psramFreeBytes",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    cJSON_AddNumberToObject(telemetry, "renderElapsedMs", static_cast<double>(render_elapsed_ms));
    cJSON_AddBoolToObject(telemetry, "renderDegraded", render_degraded);
    if (motion_dispatch != nullptr) {
        cJSON_AddStringToObject(telemetry, "motionDispatch", motion_dispatch);
    }
    cJSON* reused = cJSON_CreateObject();
    cJSON_AddBoolToObject(reused, "background", background_plan.reused);
    cJSON_AddBoolToObject(reused, "object", object_plan.reused);
    cJSON_AddBoolToObject(reused, "overlay", overlay_plan.reused);
    cJSON_AddItemToObject(telemetry, "layersReused", reused);
    const char* degraded_reason = motion_degraded ? "motionPreset"
        : !poster_drew ? "backgroundUnavailable"
        : has_object_src && !object_drew ? "objectUnavailable"
        : has_overlay_src && !overlay_drew ? "overlayUnavailable"
        : (!has_object_src || !has_overlay_src) ? "optionalLayerMissing" : "";
    cJSON_AddStringToObject(telemetry, "degradedReason", degraded_reason);

    if (render_degraded) {
        ESP_LOGW(TAG,
                 "render_degraded assignmentId=%s sessionId=%s lessonId=%s stepId=%s "
                 "sequence=%ld",
                 assignment_id, session_id, lesson_id != nullptr ? lesson_id : "-",
                 step_id != nullptr ? step_id : "-", static_cast<long>(sequence));
    }
    if (optional_asset_missing) {
        ESP_LOGW(TAG,
                 "optional_asset_missing assignmentId=%s sessionId=%s lessonId=%s stepId=%s "
                 "sequence=%ld",
                 assignment_id, session_id, lesson_id != nullptr ? lesson_id : "-",
                 step_id != nullptr ? step_id : "-", static_cast<long>(sequence));
    }

    // Canonical step-ack first (body.acks echoes the step's sequence). For a PASSIVE
    // narration step this ack IS the completion signal (the ESP auto-advances on it),
    // so it is the ONLY F->S frame for such a step.
    emit_ack(root, sequence, rendered, degraded, nullptr, true, render_elapsed_ms,
             tvideo_degraded ? tvideo_degraded_reason : nullptr, telemetry);
    if (!has_visible_content) {
        Application::GetInstance().CancelLessonInteractiveListening();
        Application::GetInstance().SetLessonRuntimeActive(false);
        g_session.running = false;
        g_session.paused = false;
        g_session.prepared = false;
        end_lesson_asset_session();
        ClearTerminalLessonCursor();
    }

    // FW-01 / FW-LESSON-01: render ack is not child-response evidence. Passive
    // narration steps auto-advance on this ack; interactive steps wait for the
    // server voice transcript path (or a future explicit tap/listen response) to
    // report step_completed. Do not fabricate result="success" with empty detail
    // here, because that advances the lesson before the child answers.
    const bool has_visible_child_prompt = has_caption_prompt && !caption.empty();
    const bool should_listen = !passive && has_visible_content && has_visible_child_prompt;
    if (!should_listen) {
        Application::GetInstance().CancelLessonInteractiveListening();
    }
    if (should_listen) {
        const uint32_t listen_generation =
            Application::GetInstance().BeginLessonInteractiveListeningRequest();
        ESP_LOGI(TAG, "lesson_step interactive opening listen window stepId=%s",
                 step_id != nullptr ? step_id : "?");
        Schedule([listen_generation]() {
            Application::GetInstance().PrepareLessonInteractiveListening(listen_generation);
        });
    }
    ESP_LOGI(TAG, "lesson_step rendered stepId=%s passive=%d degraded=%d renderElapsedMs=%ld",
             step_id != nullptr ? step_id : "?", passive, degraded,
             static_cast<long>(render_elapsed_ms));
}
