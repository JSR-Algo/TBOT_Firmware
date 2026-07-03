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
// US-006 image render: the on-device LVGL decoder + draw path. LvglDisplay carries
// SetLessonBackground (the new persistent full-screen draw) and LvglAllocatedImage is
// the same decoded-bytes wrapper the proven mcp_server.cc preview path uses.
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "jpeg_to_image.h"

#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <memory>
#include <set>

#include <cJSON.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#define TAG "Lesson"

namespace {

// ---- null-safe accessors (every envelope + body.scene.* field is guarded) ----
const char* Str(const cJSON* o, const char* k) {
    if (o == nullptr) return nullptr;
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}
bool Blank(const char* value) {
    if (value == nullptr) return true;
    while (*value != '\0') {
        if (*value != ' ' && *value != '\t' && *value != '\n' && *value != '\r') return false;
        ++value;
    }
    return true;
}

size_t Utf8CharLen(unsigned char ch) {
    if ((ch & 0x80) == 0) return 1;
    if ((ch & 0xe0) == 0xc0) return 2;
    if ((ch & 0xf0) == 0xe0) return 3;
    if ((ch & 0xf8) == 0xf0) return 4;
    return 0;
}

void TruncateUtf8(std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) return;
    size_t pos = 0;
    size_t last_good = 0;
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
        pos += len;
        last_good = pos;
    }
    value.resize(last_good);
}

bool Num(const cJSON* o, const char* k, double& out) {
    if (o == nullptr) return false;
    const cJSON* v = cJSON_GetObjectItem(o, k);
    if (!cJSON_IsNumber(v)) return false;
    out = v->valuedouble;
    return true;
}
const cJSON* Obj(const cJSON* o, const char* k) {
    if (o == nullptr) return nullptr;
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsObject(v) ? v : nullptr;
}

// Copy an identity field from the inbound frame to an outbound frame verbatim,
// preserving its JSON type (string stays string; number stays number — D-LV keeps
// lessonVersion a NUMBER on the wire).
void CopyStr(cJSON* dst, const cJSON* src, const char* k) {
    const char* v = Str(src, k);
    if (v != nullptr) cJSON_AddStringToObject(dst, k, v);
}
void CopyNum(cJSON* dst, const cJSON* src, const char* k) {
    double v = 0.0;
    if (Num(src, k, v)) cJSON_AddNumberToObject(dst, k, v);
}

int64_t NowMs() { return static_cast<int64_t>(time(nullptr)) * 1000LL; }

// ---- single active lesson session (slice-01: one assignment at a time) -------
// HandleLessonMessage runs on the dedicated lesson_worker task, so lesson frames
// are processed sequentially and this state needs no lock. Draws are marshalled to
// the app task via Application::Schedule(). The F->S sequence is the only
// firmware-owned wire state (shared by acks + progress, monotonic per
// (assignmentId,sessionId), starting at 1 — fixture sequenceStreams F->S).
struct LessonSession {
    std::string assignment_id;
    std::string session_id;
    double      assignment_version = -1.0;  // staleness guard (plan §7.2)
    int64_t     last_in_sequence   = 0;     // highest processed S->F sequence (0 = none)
    int64_t     fs_sequence        = 0;     // firmware F->S counter, pre-inc on emit
    bool        prepared           = false;
    bool        running            = false;
    bool        paused             = false;
    // FW-LESSON-02: the (rendered, degraded) of the ack we emitted for the last
    // processed inbound sequence, so a duplicate re-ack can REPLAY the prior ack body
    // idempotently (protocol §6 / lesson-robot-protocol.md:436-438) instead of
    // hardcoding rendered=false/degraded=false. Slice-01 has a single active step, so
    // caching the last ack's flags keyed on its sequence is sufficient.
    int64_t     last_ack_sequence  = 0;     // S->F sequence this cached ack acked (0 = none)
    bool        last_ack_rendered  = false;
    bool        last_ack_degraded  = false;
    std::string last_ack_asset_pack_json;
};
LessonSession g_session;

void ClearTerminalLessonCursor() {
    // A completed/failed lesson is terminal. The server may reuse the same assignmentId
    // / sessionId for the next sample lesson and restart S->F sequence at 1; clear the
    // inbound cursor so fresh prepare is not treated as an old duplicate.
    g_session.last_in_sequence = 0;
    g_session.last_ack_sequence = 0;
    g_session.last_ack_rendered = false;
    g_session.last_ack_degraded = false;
    g_session.last_ack_asset_pack_json.clear();
    g_session.assignment_version = -1.0;
}

// Build an outbound F->S frame. Envelope identity (protocolVersion/assignmentId/
// sessionId/lessonId/lessonVersion/stepId) is echoed verbatim from the inbound
// frame; the firmware owns only type/sequence/timestamp/body. `body` is consumed.
std::string BuildFrame(const cJSON* in, const char* type, int64_t fs_seq, cJSON* body) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    CopyStr(root, in, "protocolVersion");
    CopyStr(root, in, "assignmentId");
    CopyStr(root, in, "sessionId");
    CopyStr(root, in, "lessonId");
    CopyNum(root, in, "lessonVersion");                 // NUMBER on the wire (D-LV)
    const char* step_id = Str(in, "stepId");            // echoed: "s4" on steps, null on lifecycle
    if (step_id != nullptr) cJSON_AddStringToObject(root, "stepId", step_id);
    else                    cJSON_AddNullToObject(root, "stepId");
    cJSON_AddNumberToObject(root, "sequence", static_cast<double>(fs_seq));
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(NowMs()));
    if (body != nullptr) cJSON_AddItemToObject(root, "body", body);
    char* s = cJSON_PrintUnformatted(root);
    std::string out = (s != nullptr) ? std::string(s) : std::string();
    if (s != nullptr) cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

cJSON* MakeErrorBody(const char* code, const char* message, bool retryable, const char* reason) {
    cJSON* b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "code", code);
    cJSON_AddStringToObject(b, "message", message);
    cJSON_AddBoolToObject(b, "retryable", retryable);
    cJSON* ctx = cJSON_CreateObject();
    cJSON_AddStringToObject(ctx, "reason", reason);
    cJSON_AddItemToObject(b, "context", ctx);
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
    return strcmp(step_type, "greeting") == 0 ||
           strcmp(step_type, "review")   == 0 ||
           strcmp(step_type, "focus")    == 0 ||
           strcmp(step_type, "feedback") == 0 ||
           strcmp(step_type, "celebrate") == 0;
}
bool IsPassiveStep(const char* completion_class, const char* step_type) {
    if (completion_class != nullptr) {
        if (strcmp(completion_class, "passive") == 0)     return true;
        if (strcmp(completion_class, "interactive") == 0) return false;
        // unknown completionClass -> fall through to the type-set fallback below.
    }
    return IsPassiveStepType(step_type);
}

// Layer-3 pose -> a valid emoji-collection emotion name. The sprite-atlas has no
// on-device renderer, so the robot overlay collapses to an emoji-face (the defined
// espTft v1 full render — D-ATLAS-CRITICAL / DIV-FW-ATLAS).
const char* ExpressionToEmotion(const char* expr) {
    if (expr == nullptr) return "neutral";
    if (strcmp(expr, "teaching") == 0 || strcmp(expr, "modeling") == 0)   return "happy";
    if (strcmp(expr, "celebrating") == 0)                                 return "laughing";
    if (strcmp(expr, "thinking") == 0 || strcmp(expr, "listening") == 0)  return "thinking";
    return "neutral";
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

constexpr size_t kMaxLessonImageBytes = 512 * 1024;
constexpr size_t kMaxLessonDecodedImageBytes = 320 * 240 * 2;
constexpr int kLessonImageHttpTimeoutMs = 1200;
constexpr size_t kLessonImageCacheMaxEntries = 8;
constexpr size_t kLessonImageCacheMaxBytes = 384 * 1024;

bool LessonDecodedImageFitsBudget(size_t decoded_len, size_t width, size_t height, size_t stride) {
    if (decoded_len == 0 || width == 0 || height == 0 || stride == 0) return false;
    if (width > SIZE_MAX / 2) return false;
    if (stride < width * 2) return false;
    if (height > SIZE_MAX / stride) return false;
    const size_t footprint = stride * height;
    if (footprint == 0 || decoded_len < footprint) return false;
    return footprint <= kMaxLessonDecodedImageBytes && decoded_len <= kMaxLessonDecodedImageBytes;
}

std::unique_ptr<LvglImage> DecodeLessonImageBytes(char* data, size_t content_length,
                                                  const char* log_prefix) {
    if (data == nullptr || content_length == 0) return nullptr;
    if (IsJpegImage(data, content_length)) {
        uint8_t* decoded_data = nullptr;
        size_t decoded_len = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;

        esp_err_t ret = jpeg_to_image(reinterpret_cast<const uint8_t*>(data), content_length,
                                      &decoded_data, &decoded_len, &width, &height, &stride);
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

// GCOVR_EXCL_START: exercised by host behavior tests, but OOM/LRU defensive branches
// are not useful to exhaustively line-cover in the native harness.
struct LessonImageCacheEntry {
    std::string url;
    char* data = nullptr;
    size_t size = 0;
    uint32_t last_used = 0;
};
LessonImageCacheEntry g_lesson_image_cache[kLessonImageCacheMaxEntries];
size_t g_lesson_image_cache_bytes = 0;
uint32_t g_lesson_image_cache_clock = 0;

void FreeLessonImageCacheEntry(LessonImageCacheEntry& entry) {
    if (entry.data != nullptr) {
        heap_caps_free(entry.data);
    }
    entry.url.clear();
    entry.data = nullptr;
    entry.size = 0;
    entry.last_used = 0;
}

void ClearLessonImageCache() {
    for (auto& entry : g_lesson_image_cache) {
        FreeLessonImageCacheEntry(entry);
    }
    g_lesson_image_cache_bytes = 0;
}

void EvictLessonImageCacheEntry(size_t index) {
    if (index >= kLessonImageCacheMaxEntries) return;
    LessonImageCacheEntry& entry = g_lesson_image_cache[index];
    if (entry.data == nullptr) return;
    if (g_lesson_image_cache_bytes >= entry.size) {
        g_lesson_image_cache_bytes -= entry.size;
    } else {
        g_lesson_image_cache_bytes = 0;
    }
    FreeLessonImageCacheEntry(entry);
}

int FindLessonImageCacheEntry(const char* url) {
    if (url == nullptr) return -1;
    for (size_t i = 0; i < kLessonImageCacheMaxEntries; ++i) {
        if (g_lesson_image_cache[i].data != nullptr && g_lesson_image_cache[i].url == url) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int FindLessonImageCacheSlot() {
    for (size_t i = 0; i < kLessonImageCacheMaxEntries; ++i) {
        if (g_lesson_image_cache[i].data == nullptr) return static_cast<int>(i);
    }
    size_t lru_index = 0;
    uint32_t lru_stamp = g_lesson_image_cache[0].last_used;
    for (size_t i = 1; i < kLessonImageCacheMaxEntries; ++i) {
        if (g_lesson_image_cache[i].last_used < lru_stamp) {
            lru_stamp = g_lesson_image_cache[i].last_used;
            lru_index = i;
        }
    }
    return static_cast<int>(lru_index);
}

char* CloneLessonImageBytes(const char* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxLessonImageBytes) return nullptr;
    char* copy = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    if (copy == nullptr) return nullptr;
    memcpy(copy, data, size);
    return copy;
}

std::unique_ptr<LvglImage> LessonImageCacheLookup(const char* url) {
    int index = FindLessonImageCacheEntry(url);
    if (index < 0) return nullptr;
    LessonImageCacheEntry& entry = g_lesson_image_cache[index];
    char* copy = CloneLessonImageBytes(entry.data, entry.size);
    if (copy == nullptr) {
        ESP_LOGW(TAG, "lesson image fetch: cache clone failed");
        return nullptr;
    }
    entry.last_used = ++g_lesson_image_cache_clock;
    ESP_LOGI(TAG, "lesson image fetch: cache hit");
    return DecodeLessonImageBytes(copy, entry.size, "lesson image cache");
}

void LessonImageCacheStore(const char* url, const char* data, size_t size) {
    if (url == nullptr || url[0] == '\0' || data == nullptr || size == 0) return;
    if (size > kMaxLessonImageBytes || size > kLessonImageCacheMaxBytes) return;

    int existing = FindLessonImageCacheEntry(url);
    if (existing >= 0) {
        EvictLessonImageCacheEntry(static_cast<size_t>(existing));
    }
    while (g_lesson_image_cache_bytes + size > kLessonImageCacheMaxBytes) {
        int evict = FindLessonImageCacheSlot();
        if (evict < 0 || g_lesson_image_cache[evict].data == nullptr) break;
        EvictLessonImageCacheEntry(static_cast<size_t>(evict));
    }
    int slot = FindLessonImageCacheSlot();
    if (slot < 0) return;
    if (g_lesson_image_cache[slot].data != nullptr) {
        EvictLessonImageCacheEntry(static_cast<size_t>(slot));
    }

    char* copy = CloneLessonImageBytes(data, size);
    if (copy == nullptr) {
        ESP_LOGW(TAG, "lesson image fetch: cache store alloc failed");
        return;
    }
    LessonImageCacheEntry& entry = g_lesson_image_cache[slot];
    entry.url = url;
    entry.data = copy;
    entry.size = size;
    entry.last_used = ++g_lesson_image_cache_clock;
    g_lesson_image_cache_bytes += size;
}
// GCOVR_EXCL_STOP

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
    if (strncmp(url, "sd://", 5) == 0) {
        const char* tail = url + 5;
        if (strncmp(tail, "sdcard/", 7) == 0) return ValidateLessonPackPath(std::string("/") + tail);
        if (tail[0] == '/') return ValidateLessonPackPath(tail);
        return ValidateLessonPackPath(std::string("/sdcard/") + tail);
    }
    if (strncmp(url, "file://", 7) == 0) return ValidateLessonPackPath(std::string(url + 7));
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
    char* data = static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT));
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
    const cJSON* manifest_ref = Obj(body, "manifestRef");
    const char* manifest_checksum = Str(manifest_ref, "manifestChecksum");
    const bool manifest_checksum_required = !Blank(manifest_checksum);
    const bool cache_key_has_manifest_checksum =
        manifest_checksum_required && cache_key != nullptr && strstr(cache_key, manifest_checksum) != nullptr;
    const cJSON* assets = cJSON_GetObjectItem(pack, "assets");
    const cJSON* critical_assets = cJSON_GetObjectItem(body, "criticalAssets");
    bool ready = manifest_checksum_required && cache_key_has_manifest_checksum &&
                 cJSON_IsArray(assets) && cJSON_GetArraySize(assets) > 0;
    std::set<std::string> ready_asset_keys;
    if (ready) {
        const cJSON* asset = nullptr;
        cJSON_ArrayForEach(asset, assets) {
            const char* asset_key = Str(asset, "key");
            const char* state = Str(asset, "state");
            const cJSON* checksum_ok = cJSON_GetObjectItem(asset, "checksumOk");
            const bool asset_verified =
                state != nullptr && strcmp(state, "READY") == 0 && cJSON_IsTrue(checksum_ok);
            const char* local_path = Str(asset, "localPath");
            double size_value = 0.0;
            bool has_declared_size = Num(asset, "size", size_value) && size_value > 0.0;
            size_t expected_size = 0;
            if (has_declared_size && size_value > 0.0) {
                expected_size = static_cast<size_t>(size_value);
            }
            if (asset_key == nullptr || asset_key[0] == '\0' || !asset_verified ||
                !has_declared_size || !LessonLocalFileReady(local_path, expected_size) ||
                !ready_asset_keys.insert(asset_key).second || !manifest_checksum_required ||
                !cache_key_has_manifest_checksum) {
                ready = false;
                break;
            }
        }
    }
    if (ready) {
        if (cJSON_IsArray(critical_assets) && cJSON_GetArraySize(critical_assets) > 0) {
            const cJSON* critical_asset = nullptr;
            cJSON_ArrayForEach(critical_asset, critical_assets) {
                const char* critical_key = Str(critical_asset, "key");
                if (critical_key == nullptr || critical_key[0] == '\0' ||
                    ready_asset_keys.find(critical_key) == ready_asset_keys.end()) {
                    ready = false;
                    break;
                }
            }
        }
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ready", ready);
    if (cache_key != nullptr) cJSON_AddStringToObject(out, "cacheKey", cache_key);
    return out;
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

    if (strncmp(url, "sd://", 5) == 0 || strncmp(url, "file://", 7) == 0) {
        return FetchLessonLocalImage(url);
    }

    if (auto cached = LessonImageCacheLookup(url)) {
        return cached;
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "lesson image fetch: no network");
        return nullptr;
    }
    auto http = network->CreateHttp(3);
    if (!http) return nullptr;
    http->SetTimeout(kLessonImageHttpTimeoutMs);

    if (!http->Open("GET", url)) {
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
        data = static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT));
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
        data = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_8BIT));
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
                char* grown = static_cast<char*>(heap_caps_realloc(data, new_cap, MALLOC_CAP_8BIT));
                if (grown == nullptr) {
                    ESP_LOGW(TAG, "lesson image fetch: realloc %u failed", (unsigned)new_cap);
                    heap_caps_free(data);
                    http->Close();
                    return nullptr;
                }
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

    LessonImageCacheStore(url, data, content_length);
    return DecodeLessonImageBytes(data, content_length, "lesson image fetch");
}

}  // namespace

// Sub-dispatch the slice subset (lesson_prepare/start/step/stop) and render ONE
// espTft lesson_step. Additive: never reached for the 8 legacy types, the voice
// path, or the MCP arm tools.
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

    if (assignment_id == nullptr || session_id == nullptr || !has_seq) {
        ESP_LOGW(TAG, "lesson_* dropped: missing assignmentId/sessionId/sequence");
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
        const int64_t seq = ++g_session.fs_sequence;
        std::string frame = BuildFrame(in, frame_type, seq, frame_body);
        if (protocol_) protocol_->SendLessonFrame(frame);
    };
    // Canonical lesson_ack (plan §5.3 / P0): body.acks echoes the ACKED sequence;
    // the ack's own envelope.sequence is the firmware F->S counter; there is NO
    // ackFor field. stepId is echoed (null on lifecycle, "s4" on a step).
    auto emit_ack = [&emit](const cJSON* in, int64_t acked, bool rendered, bool degraded,
                            cJSON* asset_pack_ack = nullptr, bool cache = true) {
        cJSON* b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "acks", static_cast<double>(acked));
        cJSON_AddBoolToObject(b, "rendered", rendered);
        cJSON_AddBoolToObject(b, "degraded", degraded);
        // FW-LESSON-02: remember this ack's body so a later duplicate of `acked` replays
        // the EXACT same (rendered, degraded). A re-ack (cache=false) never overwrites
        // the cached original.
        if (cache) {
            g_session.last_ack_sequence = acked;
            g_session.last_ack_rendered = rendered;
            g_session.last_ack_degraded = degraded;
            g_session.last_ack_asset_pack_json.clear();
            if (asset_pack_ack != nullptr) {
                char* printed = cJSON_PrintUnformatted(asset_pack_ack);
                if (printed != nullptr) {
                    g_session.last_ack_asset_pack_json = printed;
                    cJSON_free(printed);
                }
            }
        }
        if (asset_pack_ack != nullptr) cJSON_AddItemToObject(b, "assetPack", asset_pack_ack);
        emit(in, "lesson_ack", b);
    };
    auto show_lesson_failure_display = [this]() {
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            Schedule([display, lvgl_display]() {
                if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                if (lvgl_display) lvgl_display->SetLessonObject(nullptr);
                if (lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
                if (lvgl_display) lvgl_display->SetLessonMode(false);
                display->SetLessonCaption("");
                display->SetStatus(Lang::Strings::ERROR);
                display->SetEmotion("sad");
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
    auto end_lesson_after_failure = [this, &show_lesson_failure_display, &abort_speaking_if_needed]() {
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        Application::GetInstance().SetLessonRuntimeActive(false);
        g_session.running = false;
        g_session.paused = false;
        g_session.prepared = false;
        ClearTerminalLessonCursor();
        show_lesson_failure_display();
    };

    const bool is_prepare = strcmp(type, "lesson_prepare") == 0;
    double prepare_assignment_version = 0.0;
    const bool prepare_has_newer_assignment_version =
        is_prepare &&
        Num(body, "assignmentVersion", prepare_assignment_version) &&
        prepare_assignment_version > g_session.assignment_version;
    const bool duplicate_prepare =
        is_prepare &&
        g_session.assignment_id == assignment_id &&
        g_session.session_id == session_id &&
        sequence <= g_session.last_in_sequence &&
        !prepare_has_newer_assignment_version;

    // FW-02: a session-opening lesson_prepare resets the F->S counter + inbound cursor
    // BEFORE the version/profile gate, so that even a REJECTED fresh prepare sources
    // its lesson_error at envelope sequence=1 (the frozen per-session "F->S restarts at
    // 1" contract — fixture sequenceStreams F->S). Without this, a fresh prepare whose
    // profile/protocolVersion is unsupported would emit lesson_error on the STALE prior
    // session's advanced counter (e.g. seq=6), violating restart-at-1 and poisoning the
    // ESP's fresh inbound cursor. Mid-session lesson_step/lesson_start rejects (the case
    // the gate comment below protects) still ride the live continued counter because
    // they do NOT reset here.
    if (is_prepare && !duplicate_prepare) {
        // lesson_prepare (re)establishes the single active session and resets the
        // F->S counter + the inbound sequence cursor for this assignment.
        ClearLessonImageCache();
        g_session = LessonSession{};
        g_session.assignment_id = assignment_id;
        g_session.session_id = session_id;
    }

    // --- session context (per (assignmentId,sessionId)) ---
    // The session reset for a session-opening prepare already ran BEFORE the gate
    // (FW-02). Here we reject non-prepare frames outside the active
    // (assignmentId, sessionId) stream before version/profile errors,
    // staleness/dedup, or render handling can mutate the active F->S stream.
    if (!is_prepare && (g_session.assignment_id != assignment_id ||
                        g_session.session_id != session_id)) {
        ESP_LOGW(TAG, "lesson_%s for unknown session; dropping", type + 7);
        return;
    }

    // --- contract-identity + profile gate (plan §7.2 / DO #6) ---
    // A frame whose contract identity we cannot honor is rejected with
    // LESSON_VERSION_UNSUPPORTED and is NOT processed/acked/rendered. Negotiation
    // is EXACT-STRING (never a semver). Profile is checked where it is carried
    // (prepare/step bodies). For a session-opening prepare the counter was already
    // reset to 0 immediately above, so a rejected fresh prepare emits at sequence=1.
    // For a MID-SESSION reject the error emits through the shared F->S counter
    // (emit()), NOT a hardcoded sequence: a bad profile can ride a mid-session
    // lesson_step/lesson_start after prepare+start has already advanced fs_sequence, so
    // a fixed sequence would go BACKWARD and trip PROTOCOL_SEQUENCE_ERROR. lesson_error
    // is an F->S frame (protocol §4) and MUST participate in the sender stream
    // monotonically (lesson-robot-protocol.md:116).
    const bool version_ok = (protocol_version != nullptr) &&
                            strcmp(protocol_version, kLessonProtocolVersion) == 0;
    const char* profile = Str(body, "profile");  // present on prepare/step
    const bool profile_ok = (profile == nullptr) ||
                            strcmp(profile, kLessonProfileEspTft) == 0;
    if (!version_ok || !profile_ok) {
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

    const bool is_start = strcmp(type, "lesson_start") == 0;
    const bool is_step = strcmp(type, "lesson_step") == 0;
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
        if (av < g_session.assignment_version) {
            ESP_LOGW(TAG, "lesson_* stale assignmentVersion; dropping");
            return;
        }
        g_session.assignment_version = av;
    }

    // Dedup (plan §5.8 / protocol §6): a sequence <= the last processed value is a
    // duplicate -> re-ack idempotently, no re-render / no re-progress.
    // FW-LESSON-02: idempotency means RE-SENDING THE PRIOR lesson_ack body
    // (lesson-robot-protocol.md:436-438), so replay the cached (rendered, degraded) we
    // emitted for this exact acked sequence instead of hardcoding false/false (which
    // would corrupt the rendered/degraded observability the ack body carries). If the
    // duplicate is older than the single cached entry (slice-01 keeps only the last),
    // fall back to false/false — the conservative non-rendered ack.
    if (sequence <= g_session.last_in_sequence) {
        const bool re_rendered = (sequence == g_session.last_ack_sequence)
                                     ? g_session.last_ack_rendered : false;
        const bool re_degraded = (sequence == g_session.last_ack_sequence)
                                     ? g_session.last_ack_degraded : false;
        cJSON* re_asset_pack = nullptr;
        if (sequence == g_session.last_ack_sequence && !g_session.last_ack_asset_pack_json.empty()) {
            re_asset_pack = cJSON_Parse(g_session.last_ack_asset_pack_json.c_str());
        }
        ESP_LOGI(TAG, "lesson_* duplicate seq=%lld; re-acking rendered=%d degraded=%d",
                 (long long)sequence, re_rendered, re_degraded);
        emit_ack(root, sequence, re_rendered, re_degraded, re_asset_pack, /*cache*/ false);
        return;
    }
    if (is_step && g_session.paused) {
        ESP_LOGW(TAG, "lesson_step while paused; dropping");
        return;
    }
    g_session.last_in_sequence = sequence;

    // --- slice subset dispatch ---
    if (is_prepare) {
        const cJSON* asset_pack = Obj(body, "assetPack");
        const cJSON* manifest_ref = Obj(body, "manifestRef");
        const char* manifest_checksum = Str(manifest_ref, "manifestChecksum");
        if (asset_pack != nullptr && Blank(manifest_checksum)) {
            cJSON* eb = MakeErrorBody(
                "ASSET_PACK_NOT_READY",
                "lesson_prepare assetPack requires manifestRef.manifestChecksum",
                true,
                "assetPack");
            end_lesson_after_failure();
            emit(root, "lesson_error", eb);
            ESP_LOGW(TAG, "lesson_prepare rejected: assetPack missing manifestChecksum");
            return;
        }
        g_session.prepared = true;
        cJSON* asset_pack_ack = BuildAssetPackAck(body);
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false, asset_pack_ack);
        return;
    }
    if (strcmp(type, "lesson_start") == 0) {
        g_session.running = true;
        g_session.paused = false;
        Application::GetInstance().SetLessonRuntimeActive(true);
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        // Clear stale layers, enter lesson mode, then show a short child-visible loading
        // cue until the first real lesson_step redraws the authored scene.
        Display* start_display = Board::GetInstance().GetDisplay();
        LvglDisplay* start_lvgl = dynamic_cast<LvglDisplay*>(start_display);
        if (start_display) {
            Schedule([start_display, start_lvgl]() {
                if (start_lvgl) start_lvgl->SetLessonBackground(nullptr);
                if (start_lvgl) start_lvgl->SetLessonObject(nullptr);
                if (start_lvgl) start_lvgl->SetLessonRobotOverlay(nullptr);
                start_display->SetLessonCaption("");
                start_display->ClearChatMessages();
                if (start_lvgl) start_lvgl->SetLessonMode(true);
                start_display->SetStatus(Lang::Strings::PLEASE_WAIT);
                start_display->SetEmotion("thinking");
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
            });
        }
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        return;
    }
    if (strcmp(type, "lesson_pause") == 0) {
        g_session.paused = true;
        abort_speaking_if_needed();
        Application::GetInstance().CancelLessonInteractiveListening();
        Display* display = Board::GetInstance().GetDisplay();
        if (display) {
            Schedule([display]() {
                display->SetLessonCaption("");
                display->ClearChatMessages();
                display->SetStatus("Tạm dừng bài học");
                display->SetEmotion("thinking");
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
        if (display) {
            Schedule([display]() {
                display->SetLessonCaption("");
                display->ClearChatMessages();
                display->SetStatus("Đang học...");
                display->SetEmotion("thinking");
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
            });
        }
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        return;
    }
    if (strcmp(type, "lesson_stop") == 0) {
        const char* stop_reason = Str(body, "reason");
        const bool stop_failed = stop_reason != nullptr && strcmp(stop_reason, "FAILED") == 0;
        const bool stop_cancelled = stop_reason != nullptr && strcmp(stop_reason, "CANCELLED") == 0;
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
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        ClearTerminalLessonCursor();
        // Return the robot display to its realtime face surface (protocol §4.6):
        // clear persistent lesson layers and make the terminal reason perceivable.
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            Schedule([display, lvgl_display, stop_status, stop_emotion, stop_message, stop_sound]() {
                if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                if (lvgl_display) lvgl_display->SetLessonObject(nullptr);
                if (lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
                if (lvgl_display) lvgl_display->SetLessonMode(false);
                display->SetLessonCaption("");
                display->SetStatus(stop_status);
                display->SetEmotion(stop_emotion);
                if (stop_message != nullptr) display->SetChatMessage("assistant", stop_message);
                else display->ClearChatMessages();
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
    if (strcmp(type, "lesson_step") != 0) {
        // F->S/ESP-synth frames (ack/error/progress/preload_status) are not part
        // of the S->F command subset handled by this renderer.
        ESP_LOGW(TAG, "lesson_%s not handled in slice; dropping", type + 7);
        return;
    }

    // =====================  lesson_step (render s4 "model")  ====================
    const cJSON* scene = Obj(body, "scene");
    const cJSON* bg    = Obj(scene, "backgroundScene");
    const cJSON* to    = Obj(scene, "teachingObject");
    const cJSON* ro    = Obj(scene, "robotOverlay");

    // espTft forbids a critical background video — poster only (plan §7.4 /
    // DIV-FW-EXPORT). A manifest forcing video -> ASSET_PROFILE_UNAVAILABLE, render
    // nothing. (Step-addressed error: stepId echoed from the inbound frame.)
    const char* bg_mode = Str(bg, "mode");
    const cJSON* bg_video = (bg != nullptr) ? cJSON_GetObjectItem(bg, "video") : nullptr;
    const bool video_forced = (bg_mode != nullptr && strcmp(bg_mode, "poster") != 0) ||
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
    const char* poster_src = Str(Obj(bg, "poster"), "src");
    const char* object_src = Str(Obj(to, "asset"), "src");
    const char* overlay_src = Str(Obj(ro, "asset"), "src");
    const char* prompt = Str(body, "prompt");
    if (Blank(prompt)) prompt = Str(Obj(body, "storyBeat"), "ask");

    if (scene == nullptr || bg == nullptr || to == nullptr || ro == nullptr ||
        Blank(poster_src) || Blank(object_src) || Blank(overlay_src)) {
        cJSON* eb = MakeErrorBody("LESSON_FRAME_INVALID",
                                  "lesson_step requires backgroundScene, teachingObject, and robotOverlay image sources",
                                  false, "scene");
        end_lesson_after_failure();
        emit(root, "lesson_error", eb);
        ESP_LOGW(TAG, "lesson_step rejected: missing required three-layer image source");
        return;
    }

    // Resolve the display once and require it to be an LvglDisplay (the only class with
    // a real image draw path; OledDisplay/NoDisplay get caption-only). dynamic_cast
    // mirrors mcp_server.cc:255 and yields nullptr on non-LVGL boards.
    Display* base_display = Board::GetInstance().GetDisplay();
    LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(base_display);

    Application::GetInstance().BeginLessonNetworkRenderQuiet();
    struct LessonNetworkRenderQuietGuard {
        ~LessonNetworkRenderQuietGuard() {
            Application::GetInstance().EndLessonNetworkRenderQuiet();
        }
    } lesson_network_render_quiet_guard;

    // Try to fetch + decode the authored poster. On any failure FetchLessonImage
    // returns nullptr and we fall through to the caption-only render (never crash).
    bool poster_drew = false;
    if (lvgl_display != nullptr && poster_src != nullptr) {
        std::unique_ptr<LvglImage> bg_image = FetchLessonImage(poster_src);
        if (bg_image != nullptr) {
            // Marshal the persistent full-screen draw onto the LVGL/app task (the LVGL
            // object tree is owned there). Schedule() takes a COPYABLE std::function, so
            // we cannot capture the move-only unique_ptr directly; hand off ownership as
            // a raw pointer (release) and re-wrap it in a unique_ptr inside the lambda,
            // which then transfers ownership to SetLessonBackground. The lambda runs
            // exactly once, so the raw pointer is always re-owned (no leak).
            LvglImage* raw_bg = bg_image.release();
            Schedule([lvgl_display, raw_bg]() {
                lvgl_display->SetLessonBackground(std::unique_ptr<LvglImage>(raw_bg));
            });
            poster_drew = true;
            ESP_LOGI(TAG, "lesson_step poster fetched+drawn from URL");
        } else {
            ESP_LOGW(TAG, "lesson_step poster fetch failed; caption-only fallback");
        }
    }
    // NOTE: a flashed-only poster is NOT treated as drawn here. AssetAvailable() merely
    // probes GetAssetData() presence and discards the bytes; it never decodes nor calls
    // SetLessonBackground(). Setting poster_drew=true on presence alone acked a blank,
    // invisible poster as drawn (degraded=false) and suppressed the stale-poster clear
    // (clear_bg = !poster_drew). A poster with no real draw path must stay not-drawn so
    // the honest degraded ack fires and the previous-step background is cleared.
    bool object_drew = false;
    if (lvgl_display != nullptr && object_src != nullptr) {
        std::unique_ptr<LvglImage> object_image = FetchLessonImage(object_src);
        if (object_image != nullptr) {
            LvglImage* raw_object = object_image.release();
            Schedule([lvgl_display, raw_object]() {
                lvgl_display->SetLessonObject(std::unique_ptr<LvglImage>(raw_object));
            });
            object_drew = true;
            ESP_LOGI(TAG, "lesson_step teaching object fetched+drawn from URL");
        } else {
            ESP_LOGW(TAG, "lesson_step teaching object fetch failed; caption fallback");
        }
    }
    bool overlay_drew = false;
    if (lvgl_display != nullptr && overlay_src != nullptr) {
        std::unique_ptr<LvglImage> overlay_image = FetchLessonImage(overlay_src);
        if (overlay_image != nullptr) {
            LvglImage* raw_overlay = overlay_image.release();
            Schedule([lvgl_display, raw_overlay]() {
                lvgl_display->SetLessonRobotOverlay(std::unique_ptr<LvglImage>(raw_overlay));
            });
            overlay_drew = true;
            ESP_LOGI(TAG, "lesson_step robot overlay fetched+drawn from URL");
        } else {
            ESP_LOGW(TAG, "lesson_step robot overlay fetch failed; emoji fallback");
        }
    }

    const cJSON* card = Obj(to, "primitiveFallbackCard");
    const char* glyph = Str(card, "glyph");
    const char* label = Str(card, "label");
    if (label == nullptr) label = Str(to, "primaryWord");
    const char* emotion = ExpressionToEmotion(Str(ro, "expression"));
    const char* alt = Str(bg, "altCaption");

    // Caption line — AUTHORED lesson content only (COPPA-safe; never child speech,
    // never logged). When Layer-2 falls to the glyph card, fold glyph+label in.
    std::string caption;
    const bool has_prompt = !Blank(prompt);
    if (has_prompt) caption = prompt;
    if (!has_prompt) {
        if (!object_drew) {
            if (glyph != nullptr) { caption += glyph; caption += ' '; }
            if (label != nullptr) caption += label;
        } else if (label != nullptr) {
            caption = label;
        }
        if (alt != nullptr) {
            if (!caption.empty()) caption += " - ";
            caption += alt;
        }
    }
    TruncateUtf8(caption, 96);  // truncate for the 480px line (STORYBOARD §46-48)

    // §7.5 degraded semantics: false only when required poster/object drew, and any
    // authored robot overlay image drew. Emoji-face + caption still render as fallback.
    const bool degraded = !(poster_drew && object_drew && (overlay_src == nullptr || overlay_drew));

    // Marshal the draw onto the LVGL task, exactly like the TTS-display pattern
    // (application.cc SetChatMessage Schedule). Layer-3 emoji-face + the caption are
    // ALWAYS drawn (over the poster/object layers when fetched). If THIS step drew no
    // media layer, clear any previous lesson layer so a caption-only step is not shown
    // over a stale picture.
    Display* display = base_display;
    if (display) {
        const bool clear_bg = !poster_drew;
        const bool clear_object = !object_drew;
        const bool clear_overlay = !overlay_drew;
        Schedule([display, lvgl_display, clear_bg, clear_object, clear_overlay,
                  emo = std::string(emotion), cap = caption]() {
            if (lvgl_display) lvgl_display->SetLessonMode(true);
            if (clear_bg && lvgl_display) lvgl_display->SetLessonBackground(nullptr);
            if (clear_object && lvgl_display) lvgl_display->SetLessonObject(nullptr);
            if (clear_overlay && lvgl_display) lvgl_display->SetLessonRobotOverlay(nullptr);
            display->ClearChatMessages();
            display->SetEmotion(emo.c_str());
            display->SetLessonCaption(cap.c_str());
        });
    }

    // Canonical step-ack first (body.acks echoes the step's sequence). For a PASSIVE
    // narration step this ack IS the completion signal (the ESP auto-advances on it),
    // so it is the ONLY F->S frame for such a step.
    emit_ack(root, sequence, /*rendered*/ true, degraded);

    // FW-01 / FW-LESSON-01: render ack is not child-response evidence. Passive
    // narration steps auto-advance on this ack; interactive steps wait for the
    // server voice transcript path (or a future explicit tap/listen response) to
    // report step_completed. Do not fabricate result="success" with empty detail
    // here, because that advances the lesson before the child answers.
    const char* step_type       = Str(body, "stepType");
    const char* completion_class = Str(body, "completionClass");
    const bool passive = IsPassiveStep(completion_class, step_type);
    const char* sid = Str(root, "stepId");
    if (!passive) {
        ESP_LOGI(TAG, "lesson_step interactive opening listen window stepId=%s",
                 sid != nullptr ? sid : "?");
        Schedule([]() {
            Application::GetInstance().PrepareLessonInteractiveListening();
        });
    }
    ESP_LOGI(TAG, "lesson_step rendered stepId=%s passive=%d degraded=%d",
             sid != nullptr ? sid : "?", passive, degraded);
}
