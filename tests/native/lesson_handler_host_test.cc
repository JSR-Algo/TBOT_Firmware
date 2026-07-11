// Host-native statement-coverage harness for main/lesson_handler.cc (the additive
// US-006 lesson_* renderer). Mirrors the afsk_demod_host_test.cc precedent: compiles
// the REAL .cc against tests/native_stubs_lesson/* fakes, drives every reachable wire
// path, and asserts the REAL outbound JSON envelopes / draw calls / listen lifecycle.
//
// Scope = learning-flow only. These tests EXECUTE C++ lines (unlike the source-text
// reader tests/test_lesson_*.py which compile/flash nothing). Coverage measured with
// gcovr (see scripts/run_host_native_lesson_coverage.sh).
//
// Non-tautology proof (see NOTE markers below): several asserts pin an EXACT outbound
// value (sequence number, ready flag, error code, degraded flag, draw count) that a
// plausible source mutation would flip — named inline so a reviewer can confirm the test
// would fail under that mutation.

#include "application.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "assets.h"
#include "jpeg_to_image.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lesson_handler.h"
#include "lesson_motion_presets.h"

#include <cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>  // access() for the /sdcard writability probe

#ifdef fread
#undef fread
#endif
#include <stdio.h>
extern "C" size_t fread(void* ptr, size_t size, size_t count, FILE* stream);

bool& HostLessonFreadShortReadOnce() {
    static bool v = false;
    return v;
}

extern "C" size_t HostLessonFread(void* ptr, size_t size, size_t count, FILE* stream) {
    if (HostLessonFreadShortReadOnce()) {
        HostLessonFreadShortReadOnce() = false;
        if (count == 0) return 0;
        return ::fread(ptr, size, count - 1, stream);
    }
    return ::fread(ptr, size, count, stream);
}

namespace {

int g_checks = 0;
void require(bool cond, const char* msg) {
    g_checks++;
    if (!cond) {
        std::cerr << "lesson host test FAILED: " << msg << "\n";
        std::exit(1);
    }
}

Application& App() { return Application::GetInstance(); }
const std::vector<std::string>& Sent() { return App().protocol_->sent_frames; }

std::string FrameType(size_t i) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* t = cJSON_GetObjectItem(f, "type");
    std::string out = (t && cJSON_IsString(t)) ? t->valuestring : "";
    cJSON_Delete(f);
    return out;
}
double FrameSeq(size_t i) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* s = cJSON_GetObjectItem(f, "sequence");
    double out = (s && cJSON_IsNumber(s)) ? s->valuedouble : -999;
    cJSON_Delete(f);
    return out;
}
// stepId at envelope level: "" returned for JSON null too (we distinguish via has-key).
bool FrameStepIdIsNull(size_t i) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* v = cJSON_GetObjectItem(f, "stepId");
    bool out = v != nullptr && cJSON_IsNull(v);
    cJSON_Delete(f);
    return out;
}
std::string FrameStepId(size_t i) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* v = cJSON_GetObjectItem(f, "stepId");
    std::string out = (v && cJSON_IsString(v)) ? v->valuestring : "";
    cJSON_Delete(f);
    return out;
}
bool FrameBodyBool(size_t i, const char* key, bool dflt) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* b = cJSON_GetObjectItem(f, "body");
    cJSON* v = b ? cJSON_GetObjectItem(b, key) : nullptr;
    bool out = v ? cJSON_IsTrue(v) : dflt;
    cJSON_Delete(f);
    return out;
}
double FrameBodyNum(size_t i, const char* key) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* b = cJSON_GetObjectItem(f, "body");
    cJSON* v = b ? cJSON_GetObjectItem(b, key) : nullptr;
    double out = (v && cJSON_IsNumber(v)) ? v->valuedouble : -999;
    cJSON_Delete(f);
    return out;
}
std::string FrameBodyJson(size_t i) {
    cJSON* frame = cJSON_Parse(Sent()[i].c_str());
    cJSON* body = cJSON_GetObjectItem(frame, "body");
    char* printed = body ? cJSON_PrintUnformatted(body) : nullptr;
    std::string out = printed ? printed : "";
    if (printed) cJSON_free(printed);
    cJSON_Delete(frame);
    return out;
}
std::string FrameBodyStr(size_t i, const char* obj, const char* key) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* b = cJSON_GetObjectItem(f, "body");
    cJSON* o = (b && obj) ? cJSON_GetObjectItem(b, obj) : b;
    cJSON* v = o ? cJSON_GetObjectItem(o, key) : nullptr;
    std::string out = (v && cJSON_IsString(v)) ? v->valuestring : "";
    cJSON_Delete(f);
    return out;
}
// assetPack.ready inside an ack body.
bool FrameAssetPackReady(size_t i) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* b = cJSON_GetObjectItem(f, "body");
    cJSON* ap = b ? cJSON_GetObjectItem(b, "assetPack") : nullptr;
    cJSON* r = ap ? cJSON_GetObjectItem(ap, "ready") : nullptr;
    bool out = r ? cJSON_IsTrue(r) : false;
    cJSON_Delete(f);
    return out;
}
bool FrameHasAssetPack(size_t i) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* b = cJSON_GetObjectItem(f, "body");
    cJSON* ap = b ? cJSON_GetObjectItem(b, "assetPack") : nullptr;
    bool out = ap != nullptr;
    cJSON_Delete(f);
    return out;
}

bool IsValidUtf8(const std::string& text) {
    int remaining = 0;
    for (unsigned char ch : text) {
        if (remaining == 0) {
            if ((ch & 0x80) == 0) {
                continue;
            }
            if ((ch & 0xe0) == 0xc0) {
                remaining = 1;
            } else if ((ch & 0xf0) == 0xe0) {
                remaining = 2;
            } else if ((ch & 0xf8) == 0xf0) {
                remaining = 3;
            } else {
                return false;
            }
        } else {
            if ((ch & 0xc0) != 0x80) return false;
            --remaining;
        }
    }
    return remaining == 0;
}

void Handle(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    require(root != nullptr, "test JSON parsed");
    App().HandleLessonMessage(root);
    cJSON_Delete(root);
}

void ResetObservable() { App().HostReset(); }

// Per-test unique session identity. The renderer's g_session is FILE-STATIC and cannot be
// reset from the test, so each test bumps these ids; a fresh assignmentId makes the
// renderer's duplicate_prepare==false (assignment mismatch), firing the real fresh-session
// reset (g_session = LessonSession{}) so F->S restarts at 1 deterministically per test.
int g_aid_counter = 0;
std::string g_aid = "a1";
std::string g_sid = "s1";
void FreshSession() {
    g_aid_counter++;
    g_aid = "a" + std::to_string(g_aid_counter);
    g_sid = "s" + std::to_string(g_aid_counter);
}
const char* AID() { return g_aid.c_str(); }
const char* SID() { return g_sid.c_str(); }

// ---- frame builders ------------------------------------------------------

std::string PrepareFrame(int seq, const std::string& extra_body = "") {
    return std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\"" + extra_body + "}}";
}
std::string StartFrame(int seq) {
    return std::string("{\"type\":\"lesson_start\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{}}";
}
std::string StopFrame(int seq, const std::string& body = "") {
    return std::string("{\"type\":\"lesson_stop\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{" + body + "}}";
}
std::string PauseFrame(int seq) {
    return std::string("{\"type\":\"lesson_pause\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{}}";
}
std::string ResumeFrame(int seq) {
    return std::string("{\"type\":\"lesson_resume\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{}}";
}
std::string ErrorFrame(int seq) {
    return std::string("{\"type\":\"lesson_error\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{\"code\":\"STEP_TIMEOUT\"}}";
}
// Full three-layer lesson_step. Caller controls scene srcs + extra body fields.
std::string StepFrame(int seq, const std::string& step_id,
                      const std::string& poster_src, const std::string& object_src,
                      const std::string& overlay_src, const std::string& extra_body = "",
                      const std::string& extra_scene = "") {
    std::string scene =
        "\"scene\":{"
        "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"" + poster_src + "\"}},"
        "\"teachingObject\":{\"asset\":{\"src\":\"" + object_src + "\"}},"
        "\"robotOverlay\":{\"asset\":{\"src\":\"" + overlay_src + "\"},\"expression\":\"teaching\"}"
        + extra_scene + "}";
    return std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"stepId\":\"" + step_id + "\",\"lessonVersion\":3,\"lessonId\":\"L1\"," +
           "\"sequence\":" + std::to_string(seq) + ",\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\"" + extra_body + "," + scene + "}}";
}

// Open a fresh session: bump the per-test ids, then prepare(1)+start(2). Returns next
// sequence (3). The id bump guarantees the renderer's fresh-session reset fires so F->S
// restarts at 1 regardless of what a prior test left in the file-static g_session.
int OpenSession() {
    FreshSession();
    Handle(PrepareFrame(1));
    Handle(StartFrame(2));
    return 3;
}

int OpenMotionEnabledSession() {
    FreshSession();
    Handle(PrepareFrame(1, ",\"runtimeControls\":{\"motionPresetsEnabled\":true}"));
    Handle(StartFrame(2));
    return 3;
}

// A small valid JPEG-magic body the fake Http/decoder accept.
std::vector<unsigned char> JpegBody() {
    return {0xff, 0xd8, 0xff, 0x10, 0x20, 0x30};
}
std::vector<unsigned char> PngLikeBody() {  // non-JPEG -> LvglAllocatedImage(data,size) path
    return {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a};
}

// ==========================================================================
// 1. Envelope guards
// ==========================================================================
void test_envelope_guards() {
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;

    Handle("{\"foo\":1}");  // type==null -> return
    require(Sent().empty(), "missing type emits nothing");

    // missing assignmentId/sessionId/sequence -> dropped
    Handle("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"x\"}");
    require(Sent().empty(), "missing identity emits nothing");

    // has assignment+session but no sequence -> dropped (has_seq false)
    Handle("{\"type\":\"lesson_prepare\",\"assignmentId\":\"a\",\"sessionId\":\"s\"}");
    require(Sent().empty(), "missing sequence emits nothing");
}

// ==========================================================================
// 2. Prepare + assetPack ack ladder
// ==========================================================================
void test_prepare_basic() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;

    Handle(PrepareFrame(1));
    require(Sent().size() == 1, "prepare emits one ack");
    require(FrameType(0) == "lesson_ack", "prepare -> lesson_ack");
    // NOTE non-tautology: fresh prepare MUST reset F->S to emit at seq 1. Mutation:
    // remove the `g_session = LessonSession{}` reset -> seq would not be 1.
    require(FrameSeq(0) == 1, "fresh prepare ack rides F->S seq 1");
    require(FrameBodyNum(0, "acks") == 1, "ack echoes the acked inbound sequence");
    require(FrameBodyBool(0, "rendered", true) == false, "prepare ack rendered=false");
    require(FrameBodyBool(0, "degraded", true) == false, "prepare ack degraded=false");
    // lifecycle prepare echoes stepId as JSON null (no stepId in frame)
    require(FrameStepIdIsNull(0), "lifecycle ack stepId is null");
    require(!FrameHasAssetPack(0), "prepare without assetPack carries no assetPack ack");
}

void test_prepare_assetpack_not_ready_branches() {
    // assetPack present but empty assets array -> ready=false (assets size 0 branch)
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck1-abcdef1234567890\",\"assets\":[]}"));
    require(FrameHasAssetPack(0), "assetPack present -> ack carries assetPack");
    require(FrameAssetPackReady(0) == false, "empty assets array -> not ready");
    require(FrameBodyStr(0, "assetPack", "cacheKey") == "ck1-abcdef1234567890", "cacheKey echoed");

    // assetPack asset not READY (state mismatch) -> ready=false
    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck2-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"DOWNLOADING\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/x.png\",\"size\":10}]}"));
    require(FrameAssetPackReady(0) == false, "non-READY asset -> not ready");

    // asset READY+checksum but missing declared size -> not ready (has_declared_size false)
    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck3-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/x.png\"}]}"));
    require(FrameAssetPackReady(0) == false, "missing size -> not ready");

    // asset READY+size but local file not on disk -> LessonLocalFileReady false -> not ready
    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck4-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/missing.png\",\"size\":10}]}"));
    require(FrameAssetPackReady(0) == false, "missing local file -> not ready");
}

void test_prepare_assetpack_ready_with_real_file() {
    // Stage a real file on a host-only SD root. The wire localPath stays canonical
    // sd://sdcard/tbot/lesson-assets/..., while TBOT_HOST_LESSON_ASSET_ROOT maps it
    // to a deterministic temp directory so this test never depends on host /sdcard.
    const char* dir = "/tmp/tbot-host-sd/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd && mkdir -p /tmp/tbot-host-sd/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd/lesson-assets/ready.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "staged asset file opens");
    const char bytes[10] = {1,2,3,4,5,6,7,8,9,10};
    fwrite(bytes, 1, 10, fp);
    fclose(fp);
    (void)dir;

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"\tk1\n\"}],"
                          "\"assetPack\":{\"cacheKey\":\"ck5-abcdef1234567890\",\"assets\":["
                          "{\"key\":\" k1 \",\"state\":\" ready \",\"checksumOk\":true,"
                          "\"localPath\":\" \nsd://sdcard/tbot/lesson-assets/ready.png\t\",\"size\":10}]}"));
    require(FrameAssetPackReady(0) == true,
            "whitespace/case READY asset state + trimmed key/localPath + covered critical -> ready");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_trims_manifest_checksum_for_cache_key() {
    const char* dir = "/tmp/tbot-host-sd-manifest-checksum/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-manifest-checksum && mkdir -p /tmp/tbot-host-sd-manifest-checksum/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-manifest-checksum/lesson-assets/ready.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "manifest-checksum asset fixture opens");
    const char bytes[10] = {1,2,3,4,5,6,7,8,9,10};
    fwrite(bytes, 1, 10, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\" \\nabcdef1234567890\\t\"},"
                          "\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"ck-trimmed-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/ready.png\",\"size\":10}]}"));
    require(FrameHasAssetPack(0), "trimmed manifest checksum assetPack returns correlated ack");
    require(FrameAssetPackReady(0) == true,
            "manifest checksum whitespace is trimmed before cacheKey readiness check");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_trims_cache_key_before_ack() {
    const char* dir = "/tmp/tbot-host-sd-cache-key-trim/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-cache-key-trim && mkdir -p /tmp/tbot-host-sd-cache-key-trim/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-cache-key-trim/lesson-assets/ready.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "cache-key-trim asset fixture opens");
    const char bytes[10] = {1,2,3,4,5,6,7,8,9,10};
    fwrite(bytes, 1, 10, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\" \\nck-cache-abcdef1234567890\\t \",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/ready.png\",\"size\":10}]}"));
    require(FrameHasAssetPack(0), "trimmed cacheKey assetPack returns correlated ack");
    require(FrameAssetPackReady(0) == true,
            "cacheKey whitespace is trimmed before assetPack readiness check");
    require(FrameBodyStr(0, "assetPack", "cacheKey") == "ck-cache-abcdef1234567890",
            "cacheKey whitespace is trimmed before echoing ack");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_fractional_size_not_ready() {
    const char* dir = "/tmp/tbot-host-sd-fractional-size/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-fractional-size && mkdir -p /tmp/tbot-host-sd-fractional-size/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-fractional-size/lesson-assets/ready.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "fractional-size asset fixture opens");
    const char bytes[10] = {1,2,3,4,5,6,7,8,9,10};
    fwrite(bytes, 1, 10, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"ck-fractional-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/ready.png\",\"size\":10.5}]}"));
    require(FrameHasAssetPack(0), "fractional-size assetPack still returns correlated ack");
    require(FrameAssetPackReady(0) == false,
            "fractional declared asset size is invalid and not truncated to ready");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_ready_without_critical_asset_list() {
    // Some older ESP prepare frames may omit criticalAssets while still carrying a
    // fully materialized assetPack. Firmware must verify every asset on disk and
    // accept the pack instead of treating the absent list as a missing layer.
    const char* dir = "/tmp/tbot-host-sd-no-critical/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-no-critical && mkdir -p /tmp/tbot-host-sd-no-critical/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-no-critical/lesson-assets/ready.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "no-critical asset fixture opens");
    const char bytes[6] = {1,2,3,4,5,6};
    fwrite(bytes, 1, 6, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck6-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"backgroundScene.poster\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/ready.png\",\"size\":6}]}"));
    require(FrameHasAssetPack(0), "no-critical assetPack still carries ack body");
    require(FrameBodyStr(0, "assetPack", "cacheKey") == "ck6-abcdef1234567890",
            "no-critical cacheKey echoed");
    require(FrameAssetPackReady(0) == true,
            "verified assetPack without criticalAssets list is ready");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_prefix_only_cache_key_not_ready() {
    const char* dir = "/tmp/tbot-host-sd-prefix/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-prefix && mkdir -p /tmp/tbot-host-sd-prefix/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-prefix/lesson-assets/prefix-only.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "prefix-only fixture opens under host SD root");
    const char bytes[10] = {1,2,3,4,5,6,7,8,9,10};
    fwrite(bytes, 1, 10, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"w01-d01/v3-abcdef12\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/prefix-only.png\",\"size\":10}]}"));
    require(FrameAssetPackReady(0) == false,
            "prefix-only assetPack cacheKey is not ready for full manifest checksum");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_stale_checksum_cache_key_not_ready() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"newchecksum9876543210\"},"
                          "\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"w01-d01/v3-oldchecksum1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/missing.png\",\"size\":10}]}"));
    require(FrameHasAssetPack(0), "stale checksum cacheKey still returns assetPack ack");
    require(FrameBodyStr(0, "assetPack", "cacheKey") == "w01-d01/v3-oldchecksum1234567890",
            "stale cacheKey echoed for ESP correlation");
    require(FrameAssetPackReady(0) == false,
            "stale assetPack cacheKey is not ready for current manifest checksum");
}

void test_prepare_assetpack_missing_declared_critical_key_not_ready() {
    const char* dir = "/tmp/tbot-host-sd-missing-critical/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-missing-critical && mkdir -p /tmp/tbot-host-sd-missing-critical/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-missing-critical/lesson-assets/background.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "missing-critical fixture opens");
    const char bytes[8] = {1,2,3,4,5,6,7,8};
    fwrite(bytes, 1, 8, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"backgroundScene.poster\"},{\"key\":\"teachingObject.barn\"}],"
                          "\"assetPack\":{\"cacheKey\":\"ck-missing-critical-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"backgroundScene.poster\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/background.png\",\"size\":8}]}"));
    require(FrameHasAssetPack(0), "missing critical key still returns correlated assetPack ack");
    require(FrameAssetPackReady(0) == false,
            "declared critical asset missing from READY pack -> not ready");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_malformed_critical_assets_not_ready() {
    const char* dir = "/tmp/tbot-host-sd-malformed-critical/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-malformed-critical && mkdir -p /tmp/tbot-host-sd-malformed-critical/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-malformed-critical/lesson-assets/background.png";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "malformed-critical fixture opens");
    const char bytes[8] = {1,2,3,4,5,6,7,8};
    fwrite(bytes, 1, 8, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":{\"key\":\"backgroundScene.poster\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck-malformed-critical-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"backgroundScene.poster\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/background.png\",\"size\":8}]}"));
    require(FrameHasAssetPack(0), "malformed criticalAssets still returns correlated assetPack ack");
    require(FrameAssetPackReady(0) == false,
            "non-array criticalAssets shape is invalid and not treated as omitted");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_rejects_unbounded_count_and_total_size() {
    const char* dir = "/tmp/tbot-host-sd-assetpack-bounds/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-assetpack-bounds && mkdir -p /tmp/tbot-host-sd-assetpack-bounds/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);

    std::string many_assets;
    for (int i = 0; i < 65; ++i) {
        std::string name = "asset-" + std::to_string(i) + ".bin";
        std::string path = std::string(dir) + "/" + name;
        FILE* fp = fopen(path.c_str(), "wb");
        require(fp != nullptr, "asset-count fixture opens");
        fputc('x', fp);
        fclose(fp);
        if (!many_assets.empty()) many_assets += ",";
        many_assets += "{\"key\":\"k" + std::to_string(i) +
                       "\",\"state\":\"READY\",\"checksumOk\":true,"
                       "\"localPath\":\"sd://sdcard/tbot/lesson-assets/" + name +
                       "\",\"size\":1}";
    }

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck-count-abcdef1234567890\",\"assets\":[" +
                          many_assets + "]}"));
    require(FrameHasAssetPack(0), "oversized-count assetPack still returns correlated ack");
    require(FrameAssetPackReady(0) == false,
            "assetPack with more than the bounded asset count is not ready");

    std::string huge_assets;
    const size_t file_size = 512 * 1024;
    for (int i = 0; i < 9; ++i) {
        std::string name = "huge-" + std::to_string(i) + ".bin";
        std::string path = std::string(dir) + "/" + name;
        FILE* fp = fopen(path.c_str(), "wb");
        require(fp != nullptr, "asset-total fixture opens");
        require(fseek(fp, static_cast<long>(file_size - 1), SEEK_SET) == 0,
                "asset-total fixture seeks");
        fputc('x', fp);
        fclose(fp);
        if (!huge_assets.empty()) huge_assets += ",";
        huge_assets += "{\"key\":\"h" + std::to_string(i) +
                       "\",\"state\":\"READY\",\"checksumOk\":true,"
                       "\"localPath\":\"sd://sdcard/tbot/lesson-assets/" + name +
                       "\",\"size\":" + std::to_string(file_size) + "}";
    }

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck-total-abcdef1234567890\",\"assets\":[" +
                          huge_assets + "]}"));
    require(FrameHasAssetPack(0), "oversized-total assetPack still returns correlated ack");
    require(FrameAssetPackReady(0) == false,
            "assetPack above aggregate declared byte budget is not ready");

    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_requires_manifest_checksum_before_ack() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"w01-d01/v3-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/missing.png\",\"size\":10}]}"));
    require(Sent().size() == 1, "missing manifest checksum emits one frame");
    require(FrameType(0) == "lesson_error", "missing manifest checksum rejects prepare before ack");
    require(FrameBodyStr(0, nullptr, "code") == "ASSET_PACK_NOT_READY",
            "missing manifest checksum error code");
    require(!FrameHasAssetPack(0), "missing manifest checksum does not emit assetPack ack");

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"\"},"
                          "\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"w01-d01/v3-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/missing.png\",\"size\":10}]}"));
    require(FrameType(0) == "lesson_error", "blank manifest checksum rejects prepare before ack");
    require(FrameBodyStr(0, nullptr, "code") == "ASSET_PACK_NOT_READY",
            "blank manifest checksum error code");
    require(!FrameHasAssetPack(0), "blank manifest checksum does not emit assetPack ack");

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"   \"},"
                          "\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"w01-d01/v3-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/missing.png\",\"size\":10}]}}"));
    require(FrameType(0) == "lesson_error", "whitespace manifest checksum rejects prepare before ack");
    require(FrameBodyStr(0, nullptr, "code") == "ASSET_PACK_NOT_READY",
            "whitespace manifest checksum error code");
    require(!FrameHasAssetPack(0), "whitespace manifest checksum does not emit assetPack ack");
}

void test_prepare_reject_clears_stale_lesson_scene() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "old-step", "http://x/old-p.jpg", "http://x/old-o.jpg",
                     "http://x/old-r.jpg", ",\"prompt\":\"Old prompt\"", ""));
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "old valid step rendered background before rejected prepare");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Old prompt",
            "old valid step rendered prompt before rejected prepare");

    FreshSession();
    Handle(PrepareFrame(1, ",\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"w01-d01/v3-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/missing.png\",\"size\":10}]}"));

    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "ASSET_PACK_NOT_READY",
            "rejected prepare emits ASSET_PACK_NOT_READY");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "rejected prepare clears stale background layer");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "rejected prepare clears stale object layer");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "rejected prepare clears stale overlay layer");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
            "rejected prepare restores idle face instead of stale lesson mode");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "rejected prepare clears stale lesson prompt");
    require(disp.last_emotion == "sad", "rejected prepare shows sad face");
    require(App().cancel_listen_calls >= 1, "rejected prepare cancels interactive listening");
    require(App().lesson_runtime_active == false, "rejected prepare clears active lesson runtime flag");
}

// ==========================================================================
// 3. Version / profile gate
// ==========================================================================
void test_version_profile_gate() {
    // bad protocolVersion on a fresh prepare -> lesson_error at seq 1 ("contract")
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"WRONG\",")
           + "\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\",\"sequence\":1,"
             "\"body\":{\"profile\":\"" + kLessonProfileEspTft + "\"}}");
    require(Sent().size() == 1, "bad version -> one error frame");
    require(FrameType(0) == "lesson_error", "bad version -> lesson_error");
    require(FrameSeq(0) == 1, "fresh rejected prepare error at seq 1");
    require(FrameBodyStr(0, nullptr, "code") == "LESSON_VERSION_UNSUPPORTED", "version error code");
    require(FrameBodyStr(0, "context", "reason") == "contract", "version error reason=contract");

    // good version, bad profile -> lesson_error reason="profile"
    ResetObservable();
    FreshSession();
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":1,\"body\":{\"profile\":\"oledTiny\"}}");
    require(FrameType(0) == "lesson_error", "bad profile -> lesson_error");
    require(FrameBodyStr(0, "context", "reason") == "profile", "profile error reason=profile");
}

void test_fresh_prepare_contract_reject_clears_stale_lesson_scene() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "old-step", "http://x/old-p.jpg", "http://x/old-o.jpg",
                     "http://x/old-r.jpg", ",\"prompt\":\"Old prompt\"", ""));
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "old valid step rendered background before rejected fresh prepare");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Old prompt",
            "old valid step rendered prompt before rejected fresh prepare");

    FreshSession();
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":1,\"body\":{\"profile\":\"oledTiny\"}}");

    require(FrameType(Sent().size()-1) == "lesson_error", "bad fresh prepare profile -> lesson_error");
    require(FrameSeq(Sent().size()-1) == 1, "bad fresh prepare profile keeps fresh F->S sequence");
    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "LESSON_VERSION_UNSUPPORTED",
            "bad fresh prepare profile emits contract error");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "bad fresh prepare clears stale background layer");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "bad fresh prepare clears stale object layer");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "bad fresh prepare clears stale overlay layer");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
            "bad fresh prepare restores idle face instead of stale lesson mode");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "bad fresh prepare clears stale lesson prompt");
    require(disp.last_emotion == "sad", "bad fresh prepare shows sad face");
    require(App().cancel_listen_calls >= 1, "bad fresh prepare cancels interactive listening");
    require(App().lesson_runtime_active == false, "bad fresh prepare clears active lesson runtime flag");
}

// ==========================================================================
// 4. Unknown-session drop + staleness drop
// ==========================================================================
void test_unknown_session_and_staleness() {
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    OpenSession();  // a1/s1 active, seq now 1,2 processed
    size_t base = Sent().size();

    // a lesson_step for a DIFFERENT session -> dropped (not prepare, session mismatch)
    Handle(std::string("{\"type\":\"lesson_start\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"aX\",\"sessionId\":\"sX\","
           "\"sequence\":9,\"body\":{}}");
    require(Sent().size() == base, "unknown-session frame dropped silently");

    // staleness: a step carrying an OLDER assignmentVersion than the recorded one.
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    Handle(PrepareFrame(1, ",\"assignmentVersion\":5"));   // records av=5
    size_t b2 = Sent().size();
    // start carrying older av=2 -> stale drop, no ack
    Handle(std::string("{\"type\":\"lesson_start\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":2,\"body\":{\"assignmentVersion\":2}}");
    require(Sent().size() == b2, "stale assignmentVersion dropped");
}

// ==========================================================================
// 5. Dedup / idempotent re-ack (replays cached rendered/degraded + assetPack)
// ==========================================================================
void test_dedup_reack() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;

    // prepare with a (not-ready) assetPack so the cached ack carries an assetPack body.
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ckD-abcdef1234567890\",\"assets\":[]}"));
    require(Sent().size() == 1, "prepare ack");
    bool first_ready = FrameAssetPackReady(0);

    // duplicate prepare (same assignment/session, sequence <= last) -> re-ack path.
    // duplicate_prepare==true so NO session reset; sequence<=last triggers replay.
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ckD-abcdef1234567890\",\"assets\":[]}"));
    require(Sent().size() == 2, "duplicate prepare re-acks");
    require(FrameType(1) == "lesson_ack", "re-ack is an ack");
    // NOTE non-tautology: re-ack must REPLAY the cached assetPack body. Mutation: drop the
    // cached-assetPack replay (re_asset_pack=nullptr) -> the re-ack would carry NO assetPack.
    require(FrameHasAssetPack(1), "duplicate re-ack replays cached assetPack body");
    require(FrameAssetPackReady(1) == first_ready, "re-ack replays cached ready flag");

    // a duplicate of an OLDER sequence than the cached one -> conservative false/false,
    // no assetPack. Advance to seq 3 first via start(2)+a step? Simpler: send seq 0 dup.
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":0,\"body\":{\"profile\":\"" + kLessonProfileEspTft + "\"}}");
    require(Sent().size() == 3, "older duplicate also re-acks");
    require(FrameBodyBool(2, "rendered", true) == false, "older dup re-ack rendered=false");
    require(!FrameHasAssetPack(2), "older dup carries no cached assetPack");
}

void test_delayed_duplicate_prepare_replays_assetpack_after_start_ack() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;

    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck-delayed-abcdef1234567890\",\"assets\":[]}"));
    require(Sent().size() == 1, "initial prepare emits assetPack ack");
    require(FrameHasAssetPack(0), "initial prepare ack carries assetPack");

    Handle(StartFrame(2));
    require(Sent().size() == 2, "start emits lifecycle ack");
    require(!FrameHasAssetPack(1), "start ack carries no assetPack");

    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ck-delayed-abcdef1234567890\",\"assets\":[]}"));
    require(Sent().size() == 3, "delayed duplicate prepare re-acks");
    require(FrameHasAssetPack(2), "delayed duplicate prepare replays original assetPack");
    require(FrameBodyStr(2, "assetPack", "cacheKey") == "ck-delayed-abcdef1234567890",
            "delayed duplicate prepare replays original assetPack cacheKey");
}

void test_prepare_new_assignment_version_same_session_resets_stream() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;

    Handle(PrepareFrame(1, ",\"assignmentVersion\":1,\"manifestRef\":{\"manifestChecksum\":\"oldchecksum1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"lesson/v1-oldchecksum1234567890\",\"assets\":[]}"));
    require(Sent().size() == 1, "initial version prepare ack");
    require(FrameSeq(0) == 1, "initial version starts F->S stream at 1");
    require(FrameBodyStr(0, "assetPack", "cacheKey") == "lesson/v1-oldchecksum1234567890",
            "initial version ack carries old assetPack cacheKey");

    Handle(PrepareFrame(1, ",\"assignmentVersion\":2,\"manifestRef\":{\"manifestChecksum\":\"newchecksum9876543210\"},"
                          "\"assetPack\":{\"cacheKey\":\"lesson/v2-newchecksum9876543210\",\"assets\":[]}"));

    require(Sent().size() == 2, "republished version prepare emits fresh ack");
    require(FrameSeq(1) == 1, "republished version resets F->S stream for ESP runtime cursor");
    require(FrameBodyStr(1, "assetPack", "cacheKey") == "lesson/v2-newchecksum9876543210",
            "republished version prepare acks new assetPack cacheKey, not cached stale pack");
}

void test_fresh_prepare_clears_stale_active_lesson_before_start() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "old-interactive", "http://x/old-p.jpg", "http://x/old-o.jpg",
                     "http://x/old-r.jpg",
                     ",\"prompt\":\"Con nói: barn.\""
                     ",\"stepType\":\"ask\""
                     ",\"completionClass\":\"interactive\"",
                     ""));
    require(App().prepare_listen_calls == 1,
            "old interactive step opened a child-response window");
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "old active lesson rendered a background");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Con nói: barn.",
            "old active lesson rendered child-turn prompt");

    const int cancel_after_old_step = App().cancel_listen_calls;
    FreshSession();
    Handle(PrepareFrame(1, ",\"assignmentVersion\":2"));

    require(FrameType(Sent().size() - 1) == "lesson_ack",
            "fresh valid prepare is acked");
    require(App().cancel_listen_calls > cancel_after_old_step,
            "fresh valid prepare cancels stale child-response listening");
    require(App().lesson_runtime_active == false,
            "fresh valid prepare clears the old active lesson runtime flag before start");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "fresh valid prepare clears stale background before new start");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "fresh valid prepare clears stale object before new start");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "fresh valid prepare clears stale overlay before new start");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "fresh valid prepare clears stale child-turn caption before new start");
    require(disp.chat_messages.empty(),
            "fresh valid prepare clears stale child-turn chat copy before new start");
    require(disp.last_status == "Vui lòng đợi...",
            "fresh valid prepare shows a neutral wait state before new start");
    require(disp.last_emotion == "thinking",
            "fresh valid prepare shows a calm thinking face before new start");
}

void test_prepare_after_stop_same_session_resets_stream() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;

    Handle(PrepareFrame(1, ",\"assignmentVersion\":1"));
    Handle(StartFrame(2));
    Handle(StopFrame(3));
    require(Sent().size() == 3, "first lesson lifecycle emits prepare/start/stop acks");
    require(FrameType(2) == "lesson_ack", "first lesson stop acks");
    require(FrameBodyNum(2, "acks") == 3, "first stop ack echoes inbound sequence 3");

    Handle(PrepareFrame(1, ",\"assignmentVersion\":1"));

    require(Sent().size() == 4, "new lesson prepare after stop emits a fresh ack");
    require(FrameType(3) == "lesson_ack", "new lesson prepare is acked, not lesson_error");
    require(FrameSeq(3) == 1, "new lesson prepare after stop restarts F->S stream at 1");
    require(FrameBodyNum(3, "acks") == 1, "new lesson prepare acks inbound sequence 1");
}

void test_prepare_during_running_same_session_resets_stream() {
    ResetObservable();
    FreshSession();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;

    Handle(PrepareFrame(1, ",\"assignmentVersion\":1"));
    Handle(StartFrame(2));
    Handle(StepFrame(3, "s1", "https://example.test/bg.jpg", "", ""));
    require(Sent().size() == 3, "running lesson emitted prepare/start/step acks");

    Handle(PrepareFrame(1, ",\"assignmentVersion\":1"));

    require(Sent().size() == 4, "running same-session restart emits a fresh prepare ack");
    require(FrameType(3) == "lesson_ack", "running restart prepare is acked");
    require(FrameSeq(3) == 1, "running restart prepare restarts F->S stream at 1");
    require(FrameBodyNum(3, "acks") == 1, "running restart prepare acks inbound sequence 1");
}

void test_prepare_after_terminal_error_same_session_resets_stream() {
    ResetObservable();
    FreshSession();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;

    Handle(PrepareFrame(1, ",\"assignmentVersion\":1"));
    Handle(StartFrame(2));
    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    Handle(ErrorFrame(3));
    require(Sent().size() == 2, "inbound lesson_error is terminal and not acked");
    require(disp.last_emotion == "sad", "terminal lesson_error shows sad face");
    require(disp.chat_messages.size() == 1,
            "terminal lesson_error clears stale child-turn chat before failure copy");
    require(disp.chat_messages.back().second == "Bài học chưa tải được.",
            "terminal lesson_error shows child-safe failure copy");

    Handle(PrepareFrame(1, ",\"assignmentVersion\":1"));

    require(Sent().size() == 3, "new lesson prepare after terminal error emits fresh ack");
    require(FrameType(2) == "lesson_ack", "new lesson prepare after error is acked");
    require(FrameSeq(2) == 1, "new lesson prepare after terminal error restarts F->S stream at 1");
    require(FrameBodyNum(2, "acks") == 1, "new lesson prepare after error acks inbound sequence 1");
}

// ==========================================================================
// 6. start / stop / error lifecycle (with display clears via inline Schedule)
// ==========================================================================
void test_start_stop_error_lifecycle() {
    ResetObservable();
    FreshSession();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;

    Handle(PrepareFrame(1));
    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    App().device_state = kDeviceStateSpeaking;
    const int emotion_calls_before_start = disp.set_emotion_calls;
    Handle(StartFrame(2));
    require(FrameType(1) == "lesson_ack" && FrameSeq(1) == 2, "start acks at seq 2");
    require(App().abort_speaking_calls == 1, "start aborts stale speech before lesson loading");
    require(App().last_abort_reason == kAbortReasonNone, "start abort is a normal lesson transition");
    require(App().cancel_listen_calls >= 1, "start cancels stale child listening before lesson loading");
    // lesson_start turns OFF the idle realtime emoji face so only the 3 lesson layers show.
    // NOTE non-tautology: drop the SetLessonMode(true) in the lesson_start handler and this
    // fails (the smiley would bleed through / reappear on a caption-only step).
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == true,
            "start hides the realtime emoji face");
    require(disp.chat_messages.empty(), "start clears stale bottom-bar chat copy");
    require(disp.last_status == "Vui lòng đợi...", "start replaces stale status with wait cue");
    require(disp.set_emotion_calls == emotion_calls_before_start,
            "start does not draw realtime emotion over lesson layers");
    require(App().last_sound == "popup", "start plays audible loading cue");

    // lesson_stop: acks, cancels listening, clears all three layers + child-visible completion cue.
    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    Handle(std::string("{\"type\":\"lesson_stop\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":3,\"body\":{}}");
    require(FrameType(2) == "lesson_ack", "stop acks");
    require(App().cancel_listen_calls >= 1, "stop cancels interactive listening");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "stop clears background layer");
    require(disp.last_status == "Hoàn thành bài học", "completed stop shows completion status");
    require(disp.last_emotion == "happy", "stop shows happy completion face");
    require(disp.chat_messages.empty(), "completed stop clears stale child-turn chat copy");
    require(App().last_sound == "success", "stop plays audible completion cue");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
            "stop restores the realtime emoji face");

    const size_t after_stop_frames = Sent().size();
    const int after_stop_sounds = App().play_sound_calls;
    Handle(std::string("{\"type\":\"lesson_stop\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":3,\"body\":{}}");
    require(Sent().size() == after_stop_frames,
            "duplicate terminal stop does not replay ack");
    require(App().play_sound_calls == after_stop_sounds,
            "duplicate terminal stop does not replay completion sound");

    // A late terminal error after completed stop must not overwrite completion.
    size_t before = Sent().size();
    Handle(std::string("{\"type\":\"lesson_error\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":4,\"body\":{}}");
    require(Sent().size() == before, "late terminal lesson_error after stop is not acked");
    require(disp.last_status == "Hoàn thành bài học",
            "late terminal lesson_error after stop does not overwrite completion status");
    require(disp.last_emotion == "happy",
            "late terminal lesson_error after stop does not overwrite completion face");
    require(App().play_sound_calls == after_stop_sounds,
            "late terminal lesson_error after stop does not replay audio cue");

}

void test_start_clears_stale_lesson_scene_before_first_step() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "old-step", "http://x/old-p.jpg", "http://x/old-o.jpg",
                     "http://x/old-r.jpg", ",\"prompt\":\"Old prompt\"", ""));
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "old lesson rendered background");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Old prompt",
            "old lesson rendered prompt caption");

    OpenSession();
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "new lesson start clears stale background before first step");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "new lesson start clears stale object before first step");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "new lesson start clears stale overlay before first step");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "new lesson start clears stale prompt caption before first step");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == true,
            "new lesson start keeps lesson mode active after clearing stale scene");
}

void test_pause_resume_drop_outside_running_and_unhandled_status_frames() {
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    FreshSession();

    Handle(PrepareFrame(1));
    size_t before_pause = Sent().size();
    Handle(PauseFrame(2));
    require(Sent().size() == before_pause, "pause before start is dropped without ack");

    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    size_t before_status = Sent().size();
    Handle(std::string("{\"type\":\"lesson_preload_status\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":3,\"body\":{\"state\":\"READY\"}}");
    require(Sent().size() == before_status, "firmware-origin status frames are dropped on S->F path");
}

void test_pause_resume_is_acknowledged_and_child_visible() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    App().device_state = kDeviceStateSpeaking;
    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    disp.lesson_captions.emplace_back("Old child question");
    disp.background_calls.push_back(true);
    disp.object_calls.push_back(true);
    disp.overlay_calls.push_back(true);
    const int emotion_calls_before_pause = disp.set_emotion_calls;
    Handle(PauseFrame(3));
    require(FrameType(2) == "lesson_ack", "pause is acked");
    require(FrameBodyNum(2, "acks") == 3, "pause ack echoes inbound sequence");
    require(App().cancel_listen_calls >= 1, "pause cancels any child listening window");
    require(App().abort_speaking_calls == 1, "pause aborts any speaking prompt");
    require(App().last_abort_reason == kAbortReasonNone, "pause abort is a normal lesson pause");
    require(App().lesson_runtime_active == true, "pause keeps lesson runtime active");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "pause clears stale lesson background behind realtime face");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "pause clears stale lesson object behind realtime face");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "pause clears stale lesson overlay behind realtime face");
    require(disp.chat_messages.empty(), "pause clears stale child-turn chat copy");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "pause clears stale child-turn caption");
    require(disp.last_status == "Tạm dừng bài học", "pause shows child-visible paused status");
    require(disp.set_emotion_calls == emotion_calls_before_pause,
            "pause does not draw realtime emotion over lesson layers");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
            "pause restores realtime thinking face instead of hiding it");
    require(App().last_sound == "popup", "pause plays audible transition cue");

    const size_t before_paused_step = Sent().size();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(4, "paused-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"prompt\":\"Should not draw\"", ""));
    require(Sent().size() == before_paused_step, "paused lesson does not ack or render new steps");
    require(disp.last_status == "Tạm dừng bài học", "paused step drop leaves paused status visible");

    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    disp.lesson_captions.emplace_back("Old child question");
    disp.background_calls.push_back(true);
    disp.object_calls.push_back(true);
    disp.overlay_calls.push_back(true);
    const int emotion_calls_before_resume = disp.set_emotion_calls;
    Handle(ResumeFrame(5));
    require(FrameType(Sent().size() - 1) == "lesson_ack", "resume is acked");
    require(FrameBodyNum(Sent().size() - 1, "acks") == 5, "resume ack echoes inbound sequence");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "resume clears stale lesson background behind realtime face");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "resume clears stale lesson object behind realtime face");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "resume clears stale lesson overlay behind realtime face");
    require(disp.chat_messages.empty(), "resume clears stale child-turn chat copy");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "resume clears stale child-turn caption");
    require(disp.last_status == "Đang học...", "resume restores lesson-active status");
    require(disp.set_emotion_calls == emotion_calls_before_resume,
            "resume does not draw realtime emotion over lesson layers");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
            "resume restores realtime thinking face until the next lesson step renders");
}

void test_stop_reason_controls_terminal_cue() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    Handle(StopFrame(3, "\"reason\":\"FAILED\""));
    require(FrameType(2) == "lesson_ack", "failed stop is acked");
    require(disp.last_status == "Lỗi", "failed stop shows error status");
    require(disp.last_emotion == "sad", "failed stop shows sad face");
    require(disp.chat_messages.size() == 1,
            "failed stop clears stale child-turn chat before terminal copy");
    require(!disp.chat_messages.empty() &&
            disp.chat_messages.back().second == "Bài học bị gián đoạn.",
            "failed stop shows child-safe interruption copy");
    require(App().last_sound == "exclamation", "failed stop plays failure cue");

    ResetObservable();
    disp.chat_messages.clear();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    App().device_state = kDeviceStateSpeaking;
    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    Handle(StopFrame(3, "\"reason\":\"CANCELLED\""));
    require(FrameType(2) == "lesson_ack", "cancelled stop is acked");
    require(App().abort_speaking_calls == 1, "cancelled stop aborts any speaking prompt");
    require(App().last_abort_reason == kAbortReasonNone, "cancelled stop abort is normal");
    require(disp.last_status == "Bài học đã dừng", "cancelled stop shows stopped status");
    require(disp.last_emotion == "neutral", "cancelled stop restores neutral face");
    require(disp.chat_messages.size() == 1,
            "cancelled stop clears stale child-turn chat before terminal copy");
    require(!disp.chat_messages.empty() &&
            disp.chat_messages.back().second == "Bài học đã dừng.",
            "cancelled stop shows child-safe stopped copy");
    require(App().last_sound == "popup", "cancelled stop plays neutral transition cue");

    ResetObservable();
    disp.chat_messages.clear();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(StopFrame(3, "\"reason\":\" cancelled \""));
    require(FrameType(2) == "lesson_ack", "whitespace lowercase cancelled stop is acked");
    require(disp.last_status == "Bài học đã dừng",
            "whitespace lowercase cancelled stop does not show error status");
    require(disp.last_emotion == "neutral",
            "whitespace lowercase cancelled stop keeps a neutral face");
    require(!disp.chat_messages.empty() &&
            disp.chat_messages.back().second == "Bài học đã dừng.",
            "whitespace lowercase cancelled stop shows stopped copy");
    require(App().last_sound == "popup",
            "whitespace lowercase cancelled stop plays neutral transition cue");

    ResetObservable();
    disp.chat_messages.clear();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    disp.chat_messages.emplace_back("system", "Con nói nhé.");
    Handle(StopFrame(3, "\"reason\":\"TIMEOUT\""));
    require(FrameType(2) == "lesson_ack", "timeout stop is acked");
    require(disp.last_status == "Lỗi", "unknown stop reason does not show completion status");
    require(disp.last_emotion == "sad", "unknown stop reason does not show a happy completion face");
    require(!disp.chat_messages.empty() &&
            disp.chat_messages.back().second == "Bài học bị gián đoạn.",
            "unknown stop reason shows child-safe interruption copy");
    require(App().last_sound == "exclamation", "unknown stop reason plays failure cue");
}

// stop/error when display is a plain (non-LVGL) Display* -> lvgl_display branch nullptr,
// and when display is null entirely.
void test_lifecycle_display_variants() {
    // non-LVGL display: dynamic_cast yields null, emotion still set, no layer clears.
    ResetObservable();
    FreshSession();
    Display plain;
    Board::GetInstance().display_ = &plain;
    Board::GetInstance().network_ = nullptr;
    Handle(PrepareFrame(1));
    Handle(std::string("{\"type\":\"lesson_stop\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":2,\"body\":{}}");
    require(plain.last_emotion == "happy", "non-LVGL stop still shows completion face");
    require(App().last_sound == "success", "non-LVGL stop still plays completion cue");

    // null display: stop schedules nothing display-side, still acks.
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1));
    size_t b = Sent().size();
    Handle(std::string("{\"type\":\"lesson_stop\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":2,\"body\":{}}");
    require(FrameType(b) == "lesson_ack", "stop with null display still acks");
}

// ==========================================================================
// 7. lesson_step: video reject + missing-layer reject
// ==========================================================================
void test_step_rejects() {
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    // video forced via bg.mode != poster
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s4\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\",\"scene\":{\"backgroundScene\":{\"mode\":\"video\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"u\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"u\"}}}}}");
    require(FrameType(Sent().size()-1) == "lesson_error", "video mode -> lesson_error");
    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "ASSET_PROFILE_UNAVAILABLE",
            "video reject code");
    require(FrameStepId(Sent().size()-1) == "s4", "step error echoes stepId");

    // video forced via non-null bg.video
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s5\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\",\"scene\":{\"backgroundScene\":{\"mode\":\"poster\",\"video\":{\"src\":\"v\"}},"
           "\"teachingObject\":{\"asset\":{\"src\":\"u\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"u\"}}}}}");
    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "ASSET_PROFILE_UNAVAILABLE",
            "non-null video -> reject");

    // Poster mode token drift from upstream JSON must remain poster, not a video
    // profile request. The backend already normalizes similar step tokens.
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s5b\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\",\"prompt\":\"Look at the barn.\",\"scene\":{\"backgroundScene\":{\"mode\":\" Poster \","
           "\"poster\":{\"src\":\"http://x/p.jpg\"}},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"teaching\"}}}}");
    require(FrameType(Sent().size()-1) == "lesson_ack",
            "whitespace/case poster mode token still renders");
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "normalized poster mode draws background");
    require(FrameBodyBool(Sent().size()-1, "rendered", false) == true,
            "normalized poster mode ack is rendered");

    // missing required layer (no poster src) -> LESSON_FRAME_INVALID
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\"," 
           "\"stepId\":\"s6\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\",\"scene\":{\"backgroundScene\":{\"mode\":\"poster\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"u\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"u\"}}}}}");
    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "missing layer -> LESSON_FRAME_INVALID");

    // Blank-but-present required poster source must be rejected like backend/ESP trim
    // guards; otherwise firmware would ack a frame with no required background.
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\"," 
           "\"stepId\":\"s7\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\",\"scene\":{\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"   \"}},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"}}}}}");
    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "blank required poster source -> LESSON_FRAME_INVALID");
}

void test_invalid_step_clears_stale_lesson_scene() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "old-step", "http://x/old-p.jpg", "http://x/old-o.jpg",
                     "http://x/old-r.jpg", ",\"prompt\":\"Old prompt\"", ""));
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "old valid step rendered background before invalid step");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Old prompt",
            "old valid step rendered child prompt before invalid step");

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"bad-step\",\"sequence\":4,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\",\"scene\":{\"backgroundScene\":{\"mode\":\"poster\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"}}}}}");

    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "invalid step emits LESSON_FRAME_INVALID");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "invalid step clears stale background layer");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "invalid step clears stale object layer");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "invalid step clears stale overlay layer");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
            "invalid step restores idle face instead of stale lesson mode");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "invalid step clears stale lesson prompt");
    require(disp.last_emotion == "sad", "invalid step shows sad face");
    require(App().cancel_listen_calls >= 1, "invalid step cancels interactive listening");
    require(App().lesson_runtime_active == false, "invalid step clears active lesson runtime flag");
    require(!disp.chat_messages.empty() &&
            disp.chat_messages.back().second == "Bài học chưa tải được.",
            "invalid step shows child-safe failure caption");

    const size_t sent_after_reject = Sent().size();
    Handle(StartFrame(5));
    require(Sent().size() == sent_after_reject,
            "late start after fatal invalid step is dropped");

    Handle(StepFrame(6, "late-after-invalid", "http://x/new-p.jpg", "http://x/new-o.jpg",
                     "http://x/new-r.jpg", ",\"prompt\":\"Late prompt\"", ""));
    require(Sent().size() == sent_after_reject,
            "late step after fatal invalid step is dropped");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "late step after fatal invalid step does not repaint stale failure screen");
    require(disp.last_emotion == "sad",
            "late step after fatal invalid step leaves sad failure face visible");
}

void test_contract_reject_clears_stale_lesson_scene() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "old-step", "http://x/old-p.jpg", "http://x/old-o.jpg",
                     "http://x/old-r.jpg", ",\"prompt\":\"Old prompt\"", ""));
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "old valid step rendered background before contract reject");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Old prompt",
            "old valid step rendered prompt before contract reject");

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"bad-profile\",\"sequence\":4,\"body\":{\"profile\":\"oledTiny\"}}");

    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "LESSON_VERSION_UNSUPPORTED",
            "bad active lesson profile emits contract error");
    require(FrameBodyStr(Sent().size()-1, "context", "reason") == "profile",
            "bad active lesson profile reports profile reason");
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "contract reject clears stale background layer");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "contract reject clears stale object layer");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "contract reject clears stale overlay layer");
    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
            "contract reject restores idle face instead of stale lesson mode");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "contract reject clears stale lesson prompt");
    require(disp.last_emotion == "sad", "contract reject shows sad face");
    require(App().cancel_listen_calls >= 1, "contract reject cancels interactive listening");
    require(App().lesson_runtime_active == false, "contract reject clears active lesson runtime flag");
}

// ==========================================================================
// 8. lesson_step full render: HTTP fetch + decode all three layers (LvglDisplay)
// ==========================================================================
void test_step_full_render_http() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();
    const size_t lesson_mode_calls_before_step = disp.lesson_mode_calls.size();

    // Known Content-Length JPEG body for all three fetches; decoder mode 0 = success.
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;

    int seq = 3;
    disp.chat_messages.emplace_back("assistant", "Old transcript");
    const int emotion_calls_before_step = disp.set_emotion_calls;
    // passive step (greeting) so degraded computes from drew flags and NO listen window.
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s4\",\"lessonVersion\":3,\"lessonId\":\"L1\",\"sequence\":" +
           std::to_string(seq) + ",\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\",\"prompt\":\"Xin chào\",\"stepType\":\"greeting\",\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\" Teaching \"}}}}");
    size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "rendered step acks");
    // NOTE non-tautology: all three layers fetched+drew so degraded MUST be false.
    // Mutation: force overlay fetch to fail (degraded becomes true) -> this flips.
    require(FrameBodyBool(idx, "rendered", false) == true, "step ack rendered=true");
    require(FrameBodyBool(idx, "degraded", true) == false,
            "all three layers drew -> degraded=false");
    require(disp.lesson_mode_calls.size() > lesson_mode_calls_before_step &&
            disp.lesson_mode_calls.back() == true,
            "step hides the start loading face before drawing scene layers");
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "background image drawn back layer");
    require(!disp.object_calls.empty() && disp.object_calls.back() == true,
            "teaching object image drawn");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == true,
            "robot overlay image drawn");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Xin chào", "authored prompt caption drawn");
    require(disp.last_status == "Đang học...", "rendered step replaces loading status with active lesson status");
    require(disp.chat_messages.empty(), "new lesson step clears stale chat and does not enter normal chat history");
    require(disp.set_emotion_calls == emotion_calls_before_step,
            "lesson step does not draw realtime emotion over lesson layers");
    // passive greeting: no interactive listen window opened.
    require(App().prepare_listen_calls == 0, "passive step opens NO listen window");
}

void test_step_ignores_story_metadata_while_rendering_layers() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    Assets::GetInstance().Clear();
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s-story", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"TeeBot kể chuyện về barn.\""
                     ",\"stepType\":\"greeting\""
                     ",\"story\":{\"beatId\":\"intro\",\"text\":\"TeeBot and the child visit a barn.\"}"
                     ",\"storyText\":\"TeeBot and the child visit a barn.\""
                     ",\"storyBeat\":{\"ask\":\"What animal do you see?\",\"waitForChild\":true}"
                     ",\"vocab\":{\"word\":\"barn\",\"partOfSpeech\":\"noun\"}",
                     ""));

    const size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "story metadata step still acks");
    require(FrameStepId(idx) == "s-story", "story metadata step echoes stepId");
    require(FrameBodyBool(idx, "rendered", false) == true, "story metadata step renders");
    require(FrameBodyBool(idx, "degraded", true) == false, "story metadata step keeps all layers non-degraded");
    require(HostHttp().open_calls.size() == 3, "story metadata does not add extra asset fetches");
    require(HostHttp().open_calls[0].url == "http://x/p.jpg", "story metadata background URL fetches first");
    require(HostHttp().open_calls[1].url == "http://x/o.jpg", "story metadata teaching URL fetches second");
    require(HostHttp().open_calls[2].url == "http://x/r.jpg", "story metadata overlay URL fetches third");
    require(disp.background_calls.size() >= 1 && disp.object_calls.size() >= 1 && disp.overlay_calls.size() >= 1,
            "story metadata frame draws all three lesson layers");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "TeeBot kể chuyện về barn.",
            "story metadata frame keeps authored prompt as caption");

    // Backend manifest marks storyBeat.waitForChild as completionClass=interactive
    // even when the visual stepType is a normally-passive story/greeting frame.
    // Firmware must follow completionClass, render the layers, open the child
    // response window, and still avoid fabricating progress/scoring on render ack.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    size_t before_wait = Sent().size();
    Handle(StepFrame(3, "s-story-wait", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"What animal do you see?\""
                     ",\"stepType\":\"greeting\""
                     ",\"completionClass\":\"interactive\""
                     ",\"storyBeat\":{\"ask\":\"What animal do you see?\",\"waitForChild\":true}"
                     ",\"vocab\":{\"word\":\"barn\",\"promptKind\":\"guided-speaking\"}",
                     ""));
    require(Sent().size() == before_wait + 1, "story waitForChild render emits only ack");
    require(FrameType(Sent().size() - 1) == "lesson_ack", "story waitForChild frame acks render");
    require(FrameStepId(Sent().size() - 1) == "s-story-wait", "story waitForChild echoes stepId");
    require(FrameBodyBool(Sent().size() - 1, "rendered", false) == true,
            "story waitForChild renders layers");
    require(FrameBodyBool(Sent().size() - 1, "degraded", true) == false,
            "story waitForChild remains non-degraded");
    require(App().prepare_listen_calls == 1,
            "storyBeat.waitForChild completionClass=interactive opens child response window");
    for (const auto& frame : Sent()) {
        require(frame.find("lesson_progress") == std::string::npos,
                "story waitForChild render does not fabricate progress");
        require(frame.find("pronunciation") == std::string::npos,
                "story waitForChild render ack contains no pronunciation scoring");
        require(frame.find("score") == std::string::npos,
                "story waitForChild render ack contains no score field");
    }

    // Some authored/custom interactive steps carry storyBeat.ask without repeating
    // waitForChild. The backend/ESP contract still marks these interactive through
    // completionClass, and firmware must open the listen window from that class.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    Handle(StepFrame(3, "s-story-ask-only", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"Look at the picture.\""
                     ",\"stepType\":\"model\""
                     ",\"completionClass\":\"interactive\""
                     ",\"storyBeat\":{\"ask\":\"Which animal is beside the barn?\"}"
                     ",\"vocab\":{\"word\":\"barn\",\"promptKind\":\"guided-speaking\"}",
                     ""));
    require(FrameType(Sent().size() - 1) == "lesson_ack", "story ask-only frame acks render");
    require(FrameStepId(Sent().size() - 1) == "s-story-ask-only", "story ask-only echoes stepId");
    require(FrameBodyBool(Sent().size() - 1, "rendered", false) == true,
            "story ask-only renders layers");
    require(FrameBodyBool(Sent().size() - 1, "degraded", true) == false,
            "story ask-only remains non-degraded");
    require(App().prepare_listen_calls == 1,
            "completionClass interactive opens child response window without waitForChild flag");
    require(!disp.background_calls.empty() && !disp.object_calls.empty() && !disp.overlay_calls.empty(),
            "story ask-only frame draws all three lesson layers");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Which animal is beside the barn?",
            "interactive storyBeat.ask is the visible child question over generic prompt");

    // If the backend sends only the canonical storyBeat.ask question (no duplicate
    // prompt string), the child should still see the actual question while the mic
    // opens instead of falling back to a generic asset caption.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    Handle(StepFrame(3, "s-story-ask-caption", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"stepType\":\"model\""
                     ",\"completionClass\":\"interactive\""
                     ",\"storyBeat\":{\"ask\":\"Which animal is beside the barn?\"}"
                     ",\"vocab\":{\"word\":\"barn\",\"promptKind\":\"guided-speaking\"}",
                     ""));
    require(App().prepare_listen_calls == 1,
            "storyBeat.ask without prompt opens child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Which animal is beside the barn?",
            "storyBeat.ask without prompt becomes the visible child question");

    // A passive frame must not show an authored question unless the firmware will
    // also open the child's response window; otherwise the child sees a question
    // and the robot silently ignores the answer.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-ask-only\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"storyBeat\":{\"ask\":\"Which animal is beside the barn?\"},"
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");
    require(App().prepare_listen_calls == 0,
            "passive storyBeat.ask frame does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive storyBeat.ask without prompt falls back to non-question caption");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-question-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"Which animal is beside the barn?\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");
    require(App().prepare_listen_calls == 0,
            "passive question prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive question prompt falls back to non-question caption");

    // Step type is a protocol token, not child copy. Whitespace/case drift must
    // not turn a passive narration step into a question that opens the mic.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-step-token\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\" Greeting \","
           "\"prompt\":\"Look at the barn.\","
           "\"storyBeat\":{\"ask\":\"Which animal is beside the barn?\"},"
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Barn picture.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");
    require(App().prepare_listen_calls == 0,
            "whitespace/case passive stepType does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "whitespace/case passive stepType keeps narration caption over question");
}

void test_step_fetches_canonical_layer_urls_in_order() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s4",
                     " \nhttps://cdn.example.test/bg/poster.jpg\t",
                     "\thttps://cdn.example.test/object/barn.png ",
                     " https://cdn.example.test/robot/teach.png\n",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));

    require(HostHttp().open_calls.size() == 3, "three lesson layer URLs fetched");
    require(HostHttp().open_calls[0].method == "GET", "background fetch uses GET");
    require(HostHttp().open_calls[0].url == "https://cdn.example.test/bg/poster.jpg",
            "backgroundScene poster URL is trimmed and fetched first");
    require(HostHttp().open_calls[1].url == "https://cdn.example.test/object/barn.png",
            "teachingObject asset URL is trimmed and fetched second");
    require(HostHttp().open_calls[2].url == "https://cdn.example.test/robot/teach.png",
            "robotOverlay asset URL is trimmed and fetched third");
}

void test_step_http_fetch_sets_short_timeout_before_open() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s4",
                     "https://cdn.example.test/bg/poster.jpg",
                     "https://cdn.example.test/object/barn.png",
                     "https://cdn.example.test/robot/teach.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));

    require(HostHttp().open_calls.size() == 3, "three HTTP lesson fetches still occur");
    require(HostHttp().timeout_calls.size() == 3, "each HTTP lesson fetch sets a timeout");
    for (int timeout_ms : HostHttp().timeout_calls) {
        require(timeout_ms == 1200, "lesson image timeout is short enough for fallback-before-idle");
    }
}

void test_step_reuses_cached_layer_bytes_for_repeated_urls() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;

    const char* poster = "http://cache.test/bg/reused-poster.jpg";
    const char* object = "http://cache.test/object/reused-barn.png";
    const char* overlay = "http://cache.test/robot/reused-listen.png";

    Handle(StepFrame(3, "s-cache-1", poster, object, overlay,
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    Handle(StepFrame(4, "s-cache-2", poster, object, overlay,
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));

    require(HostHttp().open_calls.size() == 3,
            "repeated layer URLs are decoded from lesson image cache without refetch");
    require(disp.background_calls.size() >= 2 && disp.object_calls.size() >= 2 &&
                disp.overlay_calls.size() >= 2,
            "cached layer bytes still draw every repeated step");
    require(FrameBodyBool(Sent().size() - 1, "degraded", true) == false,
            "cache-hit repeated step remains non-degraded");
}

// interactive step (no completionClass, non-passive stepType) with an authored prompt
// opens a listen window.
void test_step_interactive_opens_listen() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s7", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"What do you see?\",\"stepType\":\"ask\"", ""));
    // NOTE non-tautology: an interactive step MUST open the listen window. Mutation:
    // classify "ask" as passive -> prepare_listen_calls stays 0.
    require(App().prepare_listen_calls == 1, "interactive step opens listen window");

    // Canonical s7 fillBlank frame: backend/ESP send helperText + choices for the
    // guided speaking turn. Firmware must tolerate those fields, draw the prompt,
    // ACK render, open exactly one listen window, and NOT fabricate lesson_progress
    // or pronunciation scoring from render success.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    size_t before_s7 = Sent().size();
    Handle(StepFrame(
        3, "s7", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
        ",\"prompt\":\"TeeBot says: This is a ___. You can say barn.\""
        ",\"stepType\":\"fillBlank\""
        ",\"completionClass\":\"interactive\""
        ",\"helperText\":\"TeeBot waits for your voice.\""
        ",\"choices\":[{\"id\":\"c1\",\"label\":\"barn\",\"isCorrect\":true},"
        "{\"id\":\"c2\",\"label\":\"house\",\"isCorrect\":false}]",
        ""));
    require(Sent().size() == before_s7 + 1, "s7 render emits only one ack frame");
    require(FrameType(Sent().size() - 1) == "lesson_ack", "canonical s7 render -> lesson_ack");
    require(App().prepare_listen_calls == 1, "canonical s7 opens one child-response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "TeeBot says: This is a ___. You can say barn.",
            "canonical s7 prompt caption drawn without scoring text");
    for (const auto& frame : Sent()) {
        require(frame.find("lesson_progress") == std::string::npos,
                "canonical s7 render does not fabricate progress/scoring");
        require(frame.find("pronunciation") == std::string::npos,
                "canonical s7 render ack contains no pronunciation scoring");
        require(frame.find("score") == std::string::npos,
                "canonical s7 render ack contains no score field");
    }

    // explicit completionClass=passive overrides the (interactive) type set.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    Handle(StepFrame(3, "s8", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"stepType\":\"ask\",\"completionClass\":\"passive\"", ""));
    require(App().prepare_listen_calls == 0, "completionClass=passive forces passive");

    // explicit completionClass=interactive overrides a passive type.
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    Handle(StepFrame(3, "s9", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"Can you say barn?\",\"stepType\":\"greeting\","
                     "\"completionClass\":\"interactive\"", ""));
    require(App().prepare_listen_calls == 1, "completionClass=interactive forces interactive");

    // unknown completionClass falls through to the type set (greeting=passive).
    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    Handle(StepFrame(3, "s10", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"stepType\":\"review\",\"completionClass\":\"weird\"", ""));
    require(App().prepare_listen_calls == 0, "unknown completionClass -> type-set fallback (review passive)");
}

void test_passive_step_cancels_prior_interactive_listen() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s-interactive", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"What animal is this?\""
                     ",\"stepType\":\"ask\""
                     ",\"completionClass\":\"interactive\"",
                     ""));
    require(App().prepare_listen_calls == 1, "interactive step opens the child listen window");
    const int cancel_after_interactive = App().cancel_listen_calls;

    Handle(StepFrame(4, "s-passive", "http://x/p2.jpg", "http://x/o2.jpg", "http://x/r2.jpg",
                     ",\"prompt\":\"Great listening. Now watch TeeBot show the next animal.\""
                     ",\"stepType\":\"feedback\""
                     ",\"completionClass\":\"passive\"",
                     ""));

    require(App().prepare_listen_calls == 1, "passive follow-up does not reopen the listen window");
    require(App().cancel_listen_calls > cancel_after_interactive,
            "passive follow-up cancels the prior interactive listen window");
}

void test_passive_imperative_prompt_falls_back_to_narration_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-imperative-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"Say barn.\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");

    require(App().prepare_listen_calls == 0,
            "passive imperative prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive imperative prompt falls back to narration caption");
}

void test_passive_polite_imperative_prompt_falls_back_to_narration_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-polite-imperative-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"Please say barn.\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");

    require(App().prepare_listen_calls == 0,
            "passive polite imperative prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive polite imperative prompt falls back to narration caption");
}

void test_passive_try_saying_prompt_falls_back_to_narration_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-try-saying-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"Try saying barn.\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");

    require(App().prepare_listen_calls == 0,
            "passive try-saying prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive try-saying prompt falls back to narration caption");
}

void test_passive_can_you_say_prompt_falls_back_to_narration_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-can-you-say-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"Can you say barn.\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");

    require(App().prepare_listen_calls == 0,
            "passive can-you-say prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive can-you-say prompt falls back to narration caption");
}

void test_passive_embedded_you_can_say_prompt_falls_back_to_narration_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-you-can-say-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"TeeBot says: This is a ___. You can say barn.\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");

    require(App().prepare_listen_calls == 0,
            "passive embedded you-can-say prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive embedded you-can-say prompt falls back to narration caption");
}

void test_passive_can_you_tell_me_prompt_falls_back_to_narration_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-can-you-tell-me-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"Can you tell me what animal this is.\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");

    require(App().prepare_listen_calls == 0,
            "passive can-you-tell-me prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive can-you-tell-me prompt falls back to narration caption");
}

void test_passive_can_you_find_prompt_falls_back_to_narration_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"stepId\":\"s-passive-can-you-find-prompt\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\","
           "\"prompt\":\"Can you find the barn.\","
           "\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
           "\"altCaption\":\"Look at the barn.\"},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}");

    require(App().prepare_listen_calls == 0,
            "passive can-you-find prompt does not open child response window");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "passive can-you-find prompt falls back to narration caption");
}

void test_passive_step_invalidates_queued_interactive_listen_prepare() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    App().defer_scheduled_callbacks = true;
    Handle(StepFrame(3, "s-queued-interactive",
                     "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"What animal is this?\""
                     ",\"stepType\":\"ask\""
                     ",\"completionClass\":\"interactive\"",
                     ""));
    require(App().prepare_listen_calls == 0,
            "interactive listen prepare is still queued before the app task flush");

    Handle(StepFrame(4, "s-passive-after-queued",
                     "http://x/p2.jpg", "http://x/o2.jpg", "http://x/r2.jpg",
                     ",\"prompt\":\"Nice. Now watch the next animal.\""
                     ",\"stepType\":\"feedback\""
                     ",\"completionClass\":\"passive\"",
                     ""));
    require(App().cancel_listen_calls >= 1,
            "passive follow-up cancels before the queued interactive prepare flushes");

    App().defer_scheduled_callbacks = false;
    App().FlushScheduledCallbacks();

    require(App().prepare_listen_calls == 0,
            "passive follow-up invalidates the queued interactive listen prepare");
}

void test_step_no_display_does_not_open_listen() {
    ResetObservable();
    NetworkInterface net;
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = &net;
    OpenSession();

    Handle(StepFrame(3, "s-no-display-listen",
                     "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"What animal is beside the barn?\""
                     ",\"stepType\":\"model\""
                     ",\"completionClass\":\"interactive\"",
                     ""));

    const size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "no-display interactive step still acks");
    require(FrameBodyBool(idx, "rendered", true) == false,
            "no-display interactive step reports rendered=false");
    require(App().prepare_listen_calls == 0,
            "no-display interactive step does not open mic for unseen prompt");
}

void test_step_no_display_object_does_not_open_listen() {
    ResetObservable();
    NoDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    Handle(StepFrame(3, "s-no-display-object-listen",
                     "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"What animal is beside the barn?\""
                     ",\"stepType\":\"model\""
                     ",\"completionClass\":\"interactive\"",
                     ""));

    const size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "NoDisplay object step still acks");
    require(FrameBodyBool(idx, "rendered", true) == false,
            "NoDisplay object reports rendered=false");
    require(App().prepare_listen_calls == 0,
            "NoDisplay object does not open mic for unseen prompt");
}

void test_visual_only_interactive_step_does_not_open_listen_without_caption() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;

    std::string frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
        kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
        "\"stepId\":\"s-visual-only\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
        "\",\"stepType\":\"model\",\"completionClass\":\"interactive\",\"scene\":{"
        "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}},"
        "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
        "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"listening\"}}}}";
    Handle(frame);

    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "visual-only interactive step draws the poster");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "visual-only interactive step has no child instruction caption");
    require(App().prepare_listen_calls == 0,
            "visual-only interactive step does not open mic without a child instruction caption");
}

void test_step_blank_visible_content_does_not_open_listen() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    std::string frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
        kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
        "\"stepId\":\"s-blank-visible\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
        "\",\"stepType\":\"model\",\"completionClass\":\"interactive\",\"scene\":{"
        "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}},"
        "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
        "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"listening\"}}}}";
    const int emotion_calls_before_step = disp.set_emotion_calls;
    Handle(frame);

    const size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "blank-visible-content step still acks");
    require(FrameBodyBool(idx, "rendered", false) == true,
            "display-present blank-content step reports rendered=true");
    require(FrameBodyBool(idx, "degraded", false) == true,
            "blank visible content after asset failure is degraded");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "blank visible content leaves caption empty");
    require(disp.last_status == "Lỗi",
            "blank visible content shows a failure status instead of active lesson status");
	    require(disp.set_emotion_calls == emotion_calls_before_step,
	            "blank visible content failure does not draw realtime emotion over lesson layers");
	    require(!disp.lesson_mode_calls.empty() && disp.lesson_mode_calls.back() == false,
	            "blank visible content restores realtime layer visibility after clearing lesson layers");
    require(!disp.chat_messages.empty() &&
            disp.chat_messages.back().second == "Bài học chưa tải được.",
            "blank visible content shows child-safe failure copy");
    require(App().last_sound == "exclamation",
            "blank visible content plays an audible failure cue");
    require(App().prepare_listen_calls == 0,
            "blank visible content does not open mic for an unseen prompt");
    require(App().lesson_runtime_active == false,
            "blank visible content clears active lesson runtime flag");

    const size_t sent_after_blank = Sent().size();
    Handle(StartFrame(4));
    require(Sent().size() == sent_after_blank,
            "late start after blank visible content failure is dropped");
    require(disp.last_status == "Lỗi",
            "late start after blank visible content failure leaves failure status visible");

    Handle(StepFrame(5, "late-after-blank", "http://x/new-p.jpg", "http://x/new-o.jpg",
                     "http://x/new-r.jpg", ",\"prompt\":\"Late prompt\"", ""));
    require(Sent().size() == sent_after_blank,
            "late step after blank visible content failure is dropped");
    require(disp.last_status == "Lỗi",
            "late step after blank visible content failure leaves failure status visible");
}

// ==========================================================================
// 9. degraded ladder: fetch failures + caption/glyph fallback + no network
// ==========================================================================
void test_step_degraded_and_caption_fallback() {
    // No network at all: FetchLessonImage returns nullptr for every layer; caption-only.
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;   // "no network" branch in FetchLessonImage
    OpenSession();

    // No prompt; teachingObject has a primitiveFallbackCard so glyph+label fold in.
    std::string extra_scene =
        ",\"backgroundScene_unused\":0";  // keep builder simple; add card via object below
    // Build a step where teachingObject carries primitiveFallbackCard + primaryWord and
    // backgroundScene has an altCaption -> exercises the caption assembly w/o prompt.
	    std::string frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
	        kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
	        "\"stepId\":\"s4\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
	        "\",\"stepType\":\"focus\",\"scene\":{"
	        "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
	        "\"altCaption\":\"alt text\"},"
	        "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"},"
	        "\"primitiveFallbackCard\":{\"glyph\":\"G\",\"label\":\"Lab\"},\"primaryWord\":\"Word\"},"
	        "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}";
	    const int emotion_calls_before_step = disp.set_emotion_calls;
	    Handle(frame);
    size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "caption-only step still acks");
    // NOTE non-tautology: nothing drew (no network) so degraded MUST be true. Mutation:
    // hardcode degraded=false -> flips.
    require(FrameBodyBool(idx, "degraded", false) == true, "no media drew -> degraded=true");
    // caption: glyph + label + " - " + alt (object did NOT draw, so glyph branch taken)
    require(!disp.lesson_captions.empty(), "caption drawn");
    require(disp.lesson_captions.back() == std::string("G Lab - alt text"),
            "glyph+label+alt caption assembled");
    require(disp.set_emotion_calls == emotion_calls_before_step,
            "caption-only step does not draw realtime emotion");
    // caption-only step clears any stale layers (clear_bg true -> background_calls false)
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "caption-only clears stale background");
    (void)extra_scene;

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    std::string whitespace_prompt_frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
        kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
        "\"stepId\":\"s4b\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
        "\",\"prompt\":\"   \",\"stepType\":\"focus\",\"scene\":{"
        "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"},"
        "\"altCaption\":\"alt text\"},"
        "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"},"
        "\"primitiveFallbackCard\":{\"glyph\":\"G\",\"label\":\"Lab\"},\"primaryWord\":\"Word\"},"
        "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"thinking\"}}}}";
    Handle(whitespace_prompt_frame);
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "G Lab - alt text",
            "whitespace prompt falls back to readable glyph+label+alt caption");
}

void test_step_missing_optional_object_overlay_uses_prompt_fallback() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();

    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;

    std::string frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
        kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
        "\"stepId\":\"s-optional-assets\",\"sequence\":3,\"body\":{\"profile\":\"" + kLessonProfileEspTft +
        "\",\"prompt\":\"Which animal is beside the barn?\",\"stepType\":\"model\","
        "\"completionClass\":\"interactive\",\"scene\":{"
        "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}},"
        "\"teachingObject\":{\"primitiveFallbackCard\":{\"glyph\":\"B\",\"label\":\"barn\"}},"
        "\"robotOverlay\":{\"expression\":\"listening\"}}}}";
    const int emotion_calls_before_step = disp.set_emotion_calls;
    Handle(frame);

    const size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack",
            "missing optional object/overlay assets still ack through prompt fallback");
    require(FrameBodyBool(idx, "rendered", false) == true,
            "prompt fallback frame is still rendered");
    require(FrameBodyBool(idx, "degraded", false) == true,
            "missing optional object/overlay assets mark degraded");
    require(HostHttp().open_calls.size() == 1,
            "only required poster is fetched when optional assets are absent");
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "required poster still draws");
    require(!disp.object_calls.empty() && disp.object_calls.back() == false,
            "missing object source clears stale object layer");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == false,
            "missing overlay source clears stale overlay layer");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Which animal is beside the barn?",
            "authored prompt remains visible when optional assets are absent");
    require(disp.set_emotion_calls == emotion_calls_before_step,
            "missing optional overlay does not draw realtime emotion");
    require(App().prepare_listen_calls == 1,
            "interactive prompt fallback still opens child response window");
}

void test_caption_truncation_preserves_utf8_boundary() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;

    std::string long_prompt(95, 'A');
    long_prompt += "éX";
    Handle(StepFrame(3, "s-utf8-caption", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"" + long_prompt + "\",\"stepType\":\"greeting\"",
                     ""));

    require(!disp.lesson_captions.empty(), "long UTF-8 prompt caption drawn");
    require(disp.lesson_captions.back().size() <= 96, "caption remains capped");
    require(IsValidUtf8(disp.lesson_captions.back()),
            "caption truncation never cuts a UTF-8 codepoint");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    std::string wordy_prompt =
        "Today we visit a bright barn with farm animals and hay. "
        "This caption should stop before the choppedwordtail";
    Handle(StepFrame(3, "s-word-caption", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"" + wordy_prompt + "\",\"stepType\":\"greeting\"",
                     ""));
    require(!disp.lesson_captions.empty(), "wordy long prompt caption drawn");
    require(disp.lesson_captions.back().size() <= 96, "wordy caption remains capped");
    require(IsValidUtf8(disp.lesson_captions.back()), "wordy caption remains valid UTF-8");
    require(disp.lesson_captions.back().size() >= 3 &&
                disp.lesson_captions.back().substr(disp.lesson_captions.back().size() - 3) == "...",
            "wordy caption shows ellipsis when shortened");
    require(disp.lesson_captions.back().find("choppedwordtail") == std::string::npos,
            "wordy caption does not keep a chopped trailing word");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    std::string three_byte_prompt = "€" + std::string(96, 'B');
    Handle(StepFrame(3, "s-utf8-3byte", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"" + three_byte_prompt + "\",\"stepType\":\"greeting\"",
                     ""));
    require(IsValidUtf8(disp.lesson_captions.back()), "3-byte UTF-8 caption remains valid");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    std::string four_byte_prompt = "😀" + std::string(96, 'C');
    Handle(StepFrame(3, "s-utf8-4byte", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"" + four_byte_prompt + "\",\"stepType\":\"greeting\"",
                     ""));
    require(IsValidUtf8(disp.lesson_captions.back()), "4-byte UTF-8 caption remains valid");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    std::string invalid_continuation_prompt = std::string("\xc3X", 2) + std::string(96, 'D');
    Handle(StepFrame(3, "s-utf8-invalid-cont", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"" + invalid_continuation_prompt + "\",\"stepType\":\"greeting\"",
                     ""));
    require(IsValidUtf8(disp.lesson_captions.back()),
            "invalid continuation is not preserved in caption");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    std::string invalid_lead_prompt = std::string("\xff", 1) + std::string(96, 'E');
    Handle(StepFrame(3, "s-utf8-invalid-lead", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"" + invalid_lead_prompt + "\",\"stepType\":\"greeting\"",
                     ""));
    require(IsValidUtf8(disp.lesson_captions.back()),
            "invalid UTF-8 lead byte is not preserved in caption");
}

void test_invalid_utf8_interactive_prompt_does_not_open_listen() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    std::string invalid_prompt = std::string("\xff", 1) + " barn?";
    Handle(StepFrame(3, "s-invalid-prompt-listen", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"" + invalid_prompt + "\",\"stepType\":\"ask\","
                     "\"completionClass\":\"interactive\"",
                     ""));

    require(!disp.lesson_captions.empty() && disp.lesson_captions.back().empty(),
            "invalid UTF-8 interactive prompt is not shown as a child instruction");
    require(App().prepare_listen_calls == 0,
            "invalid UTF-8 interactive prompt does not open mic for an unseen question");
}

void test_prompt_caption_collapses_internal_ascii_whitespace() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s-caption-whitespace", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"  Can\\tyou\\nsay   barn?  \",\"stepType\":\"ask\","
                     "\"completionClass\":\"interactive\"",
                     ""));

    require(!disp.lesson_captions.empty() &&
                disp.lesson_captions.back() == "Can you say barn?",
            "interactive prompt caption collapses internal ASCII whitespace");
    require(App().prepare_listen_calls == 1,
            "collapsed visible prompt still opens the child response window");
}

void test_prompt_caption_collapses_ascii_form_feed_and_vertical_tab() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s-caption-control-whitespace", "http://x/p.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"  Can\\fyou\\u000bsay barn?  \",\"stepType\":\"ask\","
                     "\"completionClass\":\"interactive\"",
                     ""));

    require(!disp.lesson_captions.empty() &&
                disp.lesson_captions.back() == "Can you say barn?",
            "interactive prompt caption collapses ASCII form-feed and vertical-tab");
    require(App().prepare_listen_calls == 1,
            "control-whitespace-collapsed prompt still opens the child response window");
}

void test_invalid_story_ask_falls_back_to_prompt_for_interactive_turn() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    std::string invalid_ask = std::string("\xff", 1) + " Which animal is beside the barn?";
    Handle(StepFrame(3, "s-invalid-story-ask", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"Tell me what you see.\""
                     ",\"stepType\":\"ask\",\"completionClass\":\"interactive\""
                     ",\"storyBeat\":{\"ask\":\"" + invalid_ask + "\"}",
                     ""));

    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Tell me what you see.",
            "invalid storyBeat.ask falls back to valid prompt caption");
    require(App().prepare_listen_calls == 1,
            "valid prompt fallback still opens the child response window");
}

void test_invalid_story_ask_suffix_falls_back_to_prompt_for_interactive_turn() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    std::string invalid_ask = "Which animal is beside the barn? " + std::string("\xff", 1);
    Handle(StepFrame(3, "s-invalid-story-ask-tail", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"Tell me what you see.\""
                     ",\"stepType\":\"ask\",\"completionClass\":\"interactive\""
                     ",\"storyBeat\":{\"ask\":\"" + invalid_ask + "\"}",
                     ""));

    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Tell me what you see.",
            "invalid storyBeat.ask suffix falls back to valid prompt caption");
    require(App().prepare_listen_calls == 1,
            "valid prompt fallback still opens after invalid story ask suffix");
}

void test_invalid_fallback_label_uses_valid_alt_caption() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;
    OpenSession();

    std::string invalid_label = "barn" + std::string("\xff", 1);
    std::string frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
        kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
        "\"stepId\":\"s-invalid-fallback-label\",\"sequence\":3,\"body\":{\"profile\":\"" +
        kLessonProfileEspTft + "\",\"stepType\":\"greeting\",\"completionClass\":\"passive\","
        "\"scene\":{\"backgroundScene\":{\"mode\":\"poster\",\"altCaption\":\"Look at the barn.\","
        "\"poster\":{\"src\":\"http://x/p.jpg\"}},\"teachingObject\":{\"primaryWord\":\"" +
        invalid_label + "\",\"asset\":{\"src\":\"http://x/o.jpg\"}},"
        "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"happy\"}}}}";
    Handle(frame);

    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Look at the barn.",
            "invalid fallback label is skipped so valid alt caption remains visible");
    require(IsValidUtf8(disp.lesson_captions.back()),
            "fallback caption remains valid UTF-8");
}

void test_interactive_prompt_caption_trims_ascii_whitespace() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s-trimmed-prompt-listen", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"prompt\":\"\\n  What animal is this? \\t\","
                     "\"stepType\":\"ask\",\"completionClass\":\"interactive\"",
                     ""));

    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "What animal is this?",
            "interactive prompt caption trims leading/trailing ASCII whitespace");
    require(App().prepare_listen_calls == 1,
            "trimmed interactive prompt still opens the child response window");
}

// HTTP status != 200, open fail, create-null, and read-error / chunked / size-cap paths.
void test_step_http_error_paths() {
    LvglDisplay disp;
    NetworkInterface net;

    // status 404 -> fetch nullptr -> degraded
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().status = 404; HostHttp().body = JpegBody();
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "404 -> degraded");

    // Open() fails
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().open_ok = false;
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "open-fail -> degraded");

    // CreateHttp returns null
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().create_null = true;
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "create-null -> degraded");

    // Content-Length exceeds cap -> dropped
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody();
    HostHttp().content_length_override = 600 * 1024;  // > 512KB cap
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "oversize CL -> dropped");

    // read error mid-body (Content-Length path)
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody();
    HostHttp().content_length_override = 6;  // matches body size, force multi-read
    HostHttp().max_chunk = 2;                // 3 reads to fill
    HostHttp().read_error_after = 1;         // 2nd read errors
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "CL read error -> degraded");

    // short read (server closed early in CL path) -> total_read < content_length
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody();
    HostHttp().content_length_override = 6;
    HostHttp().max_chunk = 2;
    HostHttp().early_close_after = 1;  // 2nd read returns 0 -> short read
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "CL short read -> degraded");
}

// chunked (no Content-Length) success + growth, plus chunked empty + chunked oversize.
void test_step_http_chunked_paths() {
    LvglDisplay disp;
    NetworkInterface net;

    // chunked success with growth: body > initial 16KB cap forces a realloc.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().use_content_length = false;          // GetBodyLength()==0 -> chunked path
    HostHttp().body.assign(20 * 1024, 0x41);        // 20KB
    HostHttp().body[0] = 0x89; HostHttp().body[1] = 0x50;  // PNG-ish, non-JPEG
    HostHttp().body[2] = 0x4e;
    HostHttp().max_chunk = 4096;                    // multiple reads -> exercise grow loop
    HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", true) == false,
            "chunked growth fetch draws all layers -> not degraded");
    require(disp.background_calls.back() == true, "chunked poster drew");

    // chunked empty body -> total_read==0 -> nullptr
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().use_content_length = false;
    HostHttp().body.clear();   // empty -> first Read returns 0
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "chunked empty -> degraded");

    // chunked oversize -> exceeds cap during growth -> dropped
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().use_content_length = false;
    HostHttp().body.assign(600 * 1024, 0x41);  // > 512KB, will blow the cap mid-grow
    HostHttp().max_chunk = 64 * 1024;
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "chunked oversize -> dropped");
}

// JPEG decode-failure + degenerate-geometry branches; non-JPEG undecodable throw branch.
void test_decode_failure_branches() {
    LvglDisplay disp;
    NetworkInterface net;

    // JPEG decode returns non-OK
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 1;  // decode fail
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "JPEG decode-fail -> degraded");

    // JPEG decode OK but degenerate geometry (width==0) -> rejected
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 2;  // ESP_OK + zero width
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "degenerate JPEG -> degraded");

    // JPEG decode OK but decoded RGB565 footprint exceeds the firmware budget -> rejected
    // before LvglAllocatedImage can retain a large buffer.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody();
    HostJpegDecodeMode() = 3;  // ESP_OK + 400x400 RGB565 decoded buffer
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "oversized decoded JPEG -> degraded");
    require(disp.background_calls.empty() || disp.background_calls.back() == false,
            "oversized decoded JPEG is not drawn");

    // non-JPEG undecodable -> LvglAllocatedImage(data,size) ctor throws -> caught -> nullptr
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = PngLikeBody();
    HostImageDecodeShouldThrow() = true;  // throws on the FIRST construction (poster)
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "undecodable image throw is caught -> degraded, no crash");
}

// OOM guards: heap_caps_malloc returns null on the pre-sized (CL) alloc and on the
// chunked initial alloc.
void test_oom_guards() {
    LvglDisplay disp;
    NetworkInterface net;

    // Content-Length path alloc fails (first heap_caps_malloc).
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostHttp().use_content_length = true;
    HostHeapFailAfter() = 0; HostHeapCallCount() = 0;  // fail the very first alloc
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    HostHeapFailAfter() = -1;  // disable hook
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "CL alloc OOM -> degraded");

    // chunked path initial alloc fails.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().use_content_length = false; HostHttp().body = JpegBody();
    HostHeapFailAfter() = 0; HostHeapCallCount() = 0;
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    HostHeapFailAfter() = -1;
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "chunked alloc OOM -> degraded");
}

// ==========================================================================
// 10. local sd:// / file:// fetch path (FetchLessonLocalImage + path validation)
// ==========================================================================
void test_local_file_fetch() {
    LvglDisplay disp;
    NetworkInterface net;

    const char* dir = "/tmp/tbot-host-sd-local/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-local && mkdir -p /tmp/tbot-host-sd-local/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);

    // unsafe path (..) rejected -> nullptr -> degraded
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    Handle(StepFrame(3, "s4", "sd://sdcard/tbot/lesson-assets/../evil.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "path traversal poster rejected -> degraded");

    // non-pack-root local path rejected
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    Handle(StepFrame(3, "s4", "file:///etc/passwd", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "non-pack-root local poster rejected -> degraded");

    const char* p = "/tmp/tbot-host-sd-local/lesson-assets/poster.jpg";
    FILE* fp = fopen(p, "wb");
    require(fp != nullptr, "local poster fixture opens under host SD root");
    auto b = JpegBody();
    fwrite(b.data(), 1, b.size(), fp);
    fclose(fp);
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostJpegDecodeMode() = 0;
    // poster local (decodes), object+overlay via HTTP success
    HostHttp().body = JpegBody();
    Handle(StepFrame(3, "s4", "sd://sdcard/tbot/lesson-assets/poster.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(disp.background_calls.back() == true, "local sd:// poster decoded+drawn");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_local_file_fetch_error_branches() {
    LvglDisplay disp;
    NetworkInterface net;

    const char* dir = "/tmp/tbot-host-sd-local-errors/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-local-errors && mkdir -p /tmp/tbot-host-sd-local-errors/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);

    // Missing file under a valid sd:// pack path -> fopen fails, degraded fallback.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "s-local-missing",
                     "sd://sdcard/tbot/lesson-assets/missing-poster.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "missing local poster -> degraded fallback");

    // FIFO under the pack root opens as a local path but cannot seek, so the
    // local file reader must close it and degrade instead of blocking/crashing.
    int fifo_rc = system("mkfifo /tmp/tbot-host-sd-local-errors/lesson-assets/poster.pipe && (printf x > /tmp/tbot-host-sd-local-errors/lesson-assets/poster.pipe &)");
    require(fifo_rc == 0, "local FIFO fixture is created");
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "s-local-fifo",
                     "sd://sdcard/tbot/lesson-assets/poster.pipe",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "unseekable local poster -> degraded fallback");

    // Empty file -> invalid local size, degraded fallback, no crash.
    FILE* empty = fopen("/tmp/tbot-host-sd-local-errors/lesson-assets/empty.jpg", "wb");
    require(empty != nullptr, "empty local fixture opens");
    fclose(empty);
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    Handle(StepFrame(3, "s-local-empty",
                     "sd://sdcard/tbot/lesson-assets/empty.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "empty local poster -> degraded fallback");

    // Local malloc failure -> degraded fallback, object/overlay can still fetch.
    FILE* poster = fopen("/tmp/tbot-host-sd-local-errors/lesson-assets/poster.jpg", "wb");
    require(poster != nullptr, "alloc-fail local fixture opens");
    auto jpeg = JpegBody();
    fwrite(jpeg.data(), 1, jpeg.size(), poster);
    fclose(poster);
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    HostHeapFailAfter() = 0; HostHeapCallCount() = 0;
    Handle(StepFrame(3, "s-local-alloc",
                     "sd://sdcard/tbot/lesson-assets/poster.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    HostHeapFailAfter() = -1;
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "local poster alloc failure -> degraded fallback");

    // Local file short-read after a successful ftell/alloc must free the buffer and
    // degrade instead of treating partial bytes as a decoded layer.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    HostLessonFreadShortReadOnce() = true;
    Handle(StepFrame(3, "s-local-short-read",
                     "sd://sdcard/tbot/lesson-assets/poster.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "local poster short read -> degraded fallback");

    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_sd_pack_step_draws_all_local_layers_and_waits_for_child() {
    LvglDisplay disp;
    NetworkInterface net;

    const char* dir = "/tmp/tbot-host-sd-pack/lesson-assets/w01-d01/v3-abcdef";
    system("rm -rf /tmp/tbot-host-sd-pack && mkdir -p /tmp/tbot-host-sd-pack/lesson-assets/w01-d01/v3-abcdef");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", "/tmp/tbot-host-sd-pack/lesson-assets", 1);

    const std::vector<std::string> files = {
        "/tmp/tbot-host-sd-pack/lesson-assets/w01-d01/v3-abcdef/backgroundScene.poster",
        "/tmp/tbot-host-sd-pack/lesson-assets/w01-d01/v3-abcdef/teachingObject.barn",
        "/tmp/tbot-host-sd-pack/lesson-assets/w01-d01/v3-abcdef/robotOverlay.teach",
    };
    auto jpeg = JpegBody();
    for (const auto& path : files) {
        FILE* fp = fopen(path.c_str(), "wb");
        require(fp != nullptr, "sd-pack layer fixture opens under host SD root");
        fwrite(jpeg.data(), 1, jpeg.size(), fp);
        fclose(fp);
    }

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostJpegDecodeMode() = 0;

    Handle(StepFrame(3, "s-sd-story",
                     "sd://sdcard/tbot/lesson-assets/w01-d01/v3-abcdef/backgroundScene.poster",
                     "sd://sdcard/tbot/lesson-assets/w01-d01/v3-abcdef/teachingObject.barn",
                     "sd://sdcard/tbot/lesson-assets/w01-d01/v3-abcdef/robotOverlay.teach",
                     ",\"prompt\":\"What animal do you see?\""
                     ",\"stepType\":\"greeting\""
                     ",\"completionClass\":\"interactive\""
                     ",\"storyBeat\":{\"ask\":\"What animal do you see?\",\"waitForChild\":true}"
                     ",\"vocab\":{\"word\":\"barn\",\"promptKind\":\"guided-speaking\"}",
                     ""));

    const size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "sd-pack story step acks render");
    require(FrameStepId(idx) == "s-sd-story", "sd-pack story step echoes stepId");
    require(FrameBodyBool(idx, "rendered", false) == true, "sd-pack story step renders");
    require(FrameBodyBool(idx, "degraded", true) == false,
            "all three sd-pack layers drew -> non-degraded");
    require(HostHttp().open_calls.empty(), "sd-pack local layers do not use HTTP fetch");
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "sd-pack background layer drawn");
    require(!disp.object_calls.empty() && disp.object_calls.back() == true,
            "sd-pack teaching object layer drawn");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == true,
            "sd-pack robot overlay layer drawn");
    require(App().prepare_listen_calls == 1,
            "sd-pack guided story opens child response window");
    for (const auto& frame : Sent()) {
        require(frame.find("lesson_progress") == std::string::npos,
                "sd-pack guided story render does not fabricate progress");
        require(frame.find("pronunciation") == std::string::npos,
                "sd-pack guided story render ack contains no pronunciation scoring");
        require(frame.find("score") == std::string::npos,
                "sd-pack guided story render ack contains no score field");
    }
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

// AssetAvailable on-device fallback: no network poster, but poster name is in the flashed
// asset table -> poster_drew via AssetAvailable.
void test_asset_available_fallback() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = nullptr;  // no HTTP
    OpenSession();
    Assets::GetInstance().Clear();
    // Flash the poster src name so AssetAvailable(poster_src) returns true.
    Assets::GetInstance().table["flashed_poster"] = {1, 2, 3, 4};
    Handle(StepFrame(3, "s4", "flashed_poster", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    // poster_drew=true via AssetAvailable; object/overlay had no network -> not drawn ->
    // degraded true (object did not draw). Still proves the AssetAvailable rung executed.
    require(FrameType(Sent().size()-1) == "lesson_ack", "asset-fallback step acks");
}

// REGRESSION (poster_drew flag-without-draw): a poster whose name is in the flashed asset
// table but whose authored URL FAILS to fetch must be treated as NOT drawn. The old
// `if (!poster_drew && AssetAvailable(poster_src)) poster_drew = true;` rung flipped
// poster_drew=true WITHOUT ever scheduling a SetLessonBackground draw (AssetAvailable only
// probes GetAssetData presence and discards the bytes). That produced two dishonest
// outcomes: (1) degraded was acked false for a blank/invisible poster, and (2) clear_bg
// (= !poster_drew) went false, so the stale previous-step poster was NOT cleared.
//
// Here all three scene.*.src are present, the poster URL is also a flashed asset key, but
// every HTTP fetch returns status 404 -> FetchLessonImage(poster) == nullptr. Honest
// behavior AFTER the fix: poster is NOT drawn -> degraded=true AND the background layer is
// explicitly cleared (SetLessonBackground(nullptr) -> background_calls.back()==false).
// The background-clear assertion is the one that FAILS before the fix (the rung sets
// poster_drew=true, so clear_bg is false and SetLessonBackground(nullptr) is never called,
// leaving background_calls EMPTY).
void test_flashed_poster_without_draw_is_not_drawn() {
    ResetObservable();
    LvglDisplay disp;
    NetworkInterface net;
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;   // network present so the HTTP path is taken
    OpenSession();

    // Flash the poster key so the (buggy) AssetAvailable rung would see it as "present".
    Assets::GetInstance().Clear();
    Assets::GetInstance().table["flashed_poster"] = {0xde, 0xad, 0xbe, 0xef};

    // All fetches fail: status != 200 -> FetchLessonImage returns nullptr for every layer.
    ResetHostHttp();
    HostHttp().status = 404;
    HostHttp().body = JpegBody();

    Handle(StepFrame(3, "s-flashed", "flashed_poster", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));

    const size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "flashed-poster-no-draw step still acks");
    // HONEST behavior after the fix: a flashed-but-not-drawn poster is degraded.
    require(FrameBodyBool(idx, "degraded", false) == true,
            "flashed-only poster with no draw path -> degraded=true");
    // DISTINGUISHING assertion (fails before the fix): because the poster was NOT drawn,
    // clear_bg is true and the renderer clears the (possibly stale) background layer.
    require(!disp.background_calls.empty() && disp.background_calls.back() == false,
            "no-draw poster clears stale background instead of acking it as drawn");
}

// Remaining reachable branches: chunked realloc-fail + chunked read-error guards, the
// JPEG decoded-image rejected throw, LessonLocalPath sd:// tail variants + non-local
// assetPack localPath, the object-drew caption branch, and the unknown-expression
// neutral-emotion default.
void test_remaining_reachable_branches() {
    LvglDisplay disp;
    NetworkInterface net;

    // --- chunked realloc-fail guard (479-481): initial 16KB malloc OK, then the grow
    // realloc returns null. Body must exceed 16KB so a realloc is attempted.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().use_content_length = false;
    HostHttp().body.assign(20 * 1024, 0x41);
    HostHttp().max_chunk = 4096;
    HostHeapFailAfter() = 1; HostHeapCallCount() = 0;  // malloc#0 OK, realloc#1 null
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    HostHeapFailAfter() = -1;
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "chunked realloc-fail -> degraded");

    // --- chunked read-error guard (489-491): chunked path, a Read() returns -1.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().use_content_length = false;
    HostHttp().body.assign(8 * 1024, 0x41);  // fits initial 16KB cap (no realloc)
    HostHttp().max_chunk = 1024;
    HostHttp().read_error_after = 1;         // 2nd read errors
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true, "chunked read-error -> degraded");

    // --- JPEG decoded-image-rejected throw (226-231): decode succeeds (mode 0) but the
    // 6-arg LvglAllocatedImage ctor throws -> caught -> nullptr.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    HostImageDecodeShouldThrow() = true;  // throws on the first (poster) JPEG construction
    Handle(StepFrame(3, "s4", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", false) == true,
            "decoded-JPEG rejected throw is caught -> degraded");

    // --- LessonLocalPath sd:// variants via assetPack localPath (LessonLocalFileReady ->
    // LessonLocalPath). These resolve the path (lines 268-273) even though the file is
    // absent on the host. Three localPaths: "/"-prefixed tail (269), bare tail (270), and
    // a non-local http localPath (273 -> "").
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ckP-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd:///sdcard/tbot/lesson-assets/a.png\",\"size\":4}]}"));
    require(FrameAssetPackReady(0) == false, "sd:// slash-tail path resolves, file absent -> not ready");

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ckP2-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://tbot/x.png\",\"size\":4}]}"));
    require(FrameAssetPackReady(0) == false, "sd:// bare-tail path resolves, file absent -> not ready");

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"assetPack\":{\"cacheKey\":\"ckP3-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"http://x/y.png\",\"size\":4}]}"));
    require(FrameAssetPackReady(0) == false, "non-local localPath -> empty path -> not ready");

    // --- object-drew caption branch (848-850): no prompt, teachingObject fetches+draws,
    // label resolves from primaryWord -> caption = label.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    {
        std::string frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
            kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
            SID() + "\",\"stepId\":\"s4\",\"sequence\":3,\"body\":{\"profile\":\"" +
            kLessonProfileEspTft + "\",\"stepType\":\"greeting\",\"scene\":{"
            "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}},"
            "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"},\"primaryWord\":\"Mèo\"},"
            "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"celebrating\"}}}}";
        const int emotion_calls_before_step = disp.set_emotion_calls;
        Handle(frame);
        // object drew + no prompt + label(primaryWord) -> caption == label
        require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Mèo",
                "object-drew + primaryWord -> caption is the label");
        require(disp.set_emotion_calls == emotion_calls_before_step,
                "celebrating expression does not draw realtime emotion");
    }

    // --- unknown expression is ignored; lesson visuals stay in the authored layers.
    ResetObservable();
    Board::GetInstance().display_ = &disp; Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    {
        std::string frame = std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
            kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
            SID() + "\",\"stepId\":\"s4\",\"sequence\":3,\"body\":{\"profile\":\"" +
            kLessonProfileEspTft + "\",\"prompt\":\"Q\",\"stepType\":\"greeting\",\"scene\":{"
            "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}},"
            "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
            "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"mysterious\"}}}}";
        const int emotion_calls_before_step = disp.set_emotion_calls;
        Handle(frame);
        require(disp.set_emotion_calls == emotion_calls_before_step,
                "unknown expression does not draw realtime emotion");
    }
}

// protocol_ null leak-guard branch: emit builds the frame but skips the send.
void test_protocol_null_send_skip() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    App().protocol_.reset();  // null protocol_ -> emit's `if (protocol_)` false branch
    // Should not crash; nothing recorded (protocol_ is null so Sent() would deref) -> we
    // simply assert no crash by completing the call.
    cJSON* root = cJSON_Parse(PrepareFrame(1).c_str());
    App().HandleLessonMessage(root);
    cJSON_Delete(root);
    require(true, "null protocol_ build-without-send did not crash");
    App().HostReset();  // restore protocol_ for any later test
}

void test_safe_motion_presets_and_auto_rest() {
    ResetObservable();
    HostEspTimerCreateOk() = false;
    require(DispatchLessonMotionPreset(App().robot_uart_, "listen") == LessonMotionResult::kDegraded,
            "timer creation failure degrades");
    HostEspTimerCreateOk() = true;

    struct Case { const char* preset; std::vector<std::string> calls; };
    const std::vector<Case> cases = {
        {"rest", {"both_arms_lower", "head_center"}},
        {"teach", {"right_arm_raise", "head_center"}},
        {"presentLeft", {"left_arm_raise", "right_arm_lower", "head_turn_left"}},
        {"presentRight", {"right_arm_raise", "left_arm_lower", "head_turn_right"}},
        {"listen", {"both_arms_lower", "head_center"}},
        {"thinking", {"left_arm_raise", "right_arm_lower", "head_turn_left"}},
        {"encourage", {"both_arms_raise", "head_center"}},
        {"tryAgain", {"both_arms_lower", "head_turn_right"}},
        {"celebrate", {"both_arms_raise", "head_center"}},
        {"goodbye", {"right_arm_raise", "left_arm_lower", "head_turn_right"}},
    };
    for (const auto& tc : cases) {
        ResetObservable();
        const auto result = DispatchLessonMotionPreset(App().robot_uart_, tc.preset);
        require(result == LessonMotionResult::kApplied, "named motion preset applies");
        require(App().robot_uart_.calls == tc.calls, "named preset exact RobotUart call order");
        if (std::string(tc.preset) != "rest") {
            HostEspAdvanceTimeUs(1500LL * 1000LL);
            HostEspFireTimer();
            auto expected = tc.calls;
            expected.push_back("both_arms_lower");
            expected.push_back("head_center");
            require(App().robot_uart_.calls == expected, "bounded preset auto-rests");
        }
    }
}

void test_motion_unknown_raw_failures_and_stale_rest_are_nonfatal_degrades() {
    ResetObservable();
    require(DispatchLessonMotionPreset(App().robot_uart_, nullptr) == LessonMotionResult::kDegraded,
            "null preset degrades");
    require(DispatchLessonMotionPreset(App().robot_uart_, "wave") == LessonMotionResult::kDegraded,
            "unknown preset degrades");
    require(App().robot_uart_.calls.empty(), "unknown preset sends nothing");
    require(DispatchLessonMotionPreset(App().robot_uart_, "90") == LessonMotionResult::kDegraded,
            "raw-looking value degrades");

    App().robot_uart_.calls.clear();
    require(DispatchLessonMotionPreset(App().robot_uart_, "celebrate") == LessonMotionResult::kApplied,
            "bounded motion is armed before unknown input");
    require(DispatchLessonMotionPreset(App().robot_uart_, "rawSweep") == LessonMotionResult::kDegraded,
            "unknown input remains degraded");
    HostEspAdvanceTimeUs(1500LL * 1000LL);
    HostEspFireTimer();
    const std::vector<std::string> preserved_rest = {
        "both_arms_raise", "head_center", "both_arms_lower", "head_center"};
    require(App().robot_uart_.calls == preserved_rest,
            "unknown input does not cancel the active preset rest");

    App().robot_uart_.send_ok = false;
    require(DispatchLessonMotionPreset(App().robot_uart_, "teach") == LessonMotionResult::kDegraded,
            "UART send failure degrades");
    App().robot_uart_.send_ok = true;

    HostEspTimerStartOk() = false;
    require(DispatchLessonMotionPreset(App().robot_uart_, "listen") == LessonMotionResult::kDegraded,
            "timer failure degrades");
    HostEspTimerStartOk() = true;

    App().robot_uart_.calls.clear();
    require(DispatchLessonMotionPreset(App().robot_uart_, "celebrate") == LessonMotionResult::kApplied,
            "first bounded preset applies");
    require(DispatchLessonMotionPreset(App().robot_uart_, "presentLeft") == LessonMotionResult::kApplied,
            "new preset cancels stale rest");
    HostEspAdvanceTimeUs(1500LL * 1000LL);
    HostEspFireTimer();
    const std::vector<std::string> expected = {
        "both_arms_raise", "head_center", "left_arm_raise", "right_arm_lower",
        "head_turn_left", "both_arms_lower", "head_center"};
    require(App().robot_uart_.calls == expected, "only newest generation auto-rest runs");
}

void test_queued_old_timer_callback_cannot_rest_a_new_pose_early() {
    ResetObservable();
    HostEspNowUs() = 1000;
    require(DispatchLessonMotionPreset(App().robot_uart_, "celebrate") == LessonMotionResult::kApplied,
            "old bounded pose schedules rest");
    const auto old_callback = HostEspQueueTimerCallback();
    require(DispatchLessonMotionPreset(App().robot_uart_, "presentLeft") == LessonMotionResult::kApplied,
            "new pose replaces the old deadline");
    const auto before_early_callback = App().robot_uart_.calls;
    HostEspInvokeQueuedCallback(old_callback);
    require(App().robot_uart_.calls == before_early_callback,
            "queued old callback does not rest before the new absolute deadline");

    HostEspAdvanceTimeUs(1500LL * 1000LL);
    HostEspFireTimer();
    auto expected = before_early_callback;
    expected.push_back("both_arms_lower");
    expected.push_back("head_center");
    require(App().robot_uart_.calls == expected, "new deadline performs exactly one rest");
    HostEspInvokeQueuedCallback(old_callback);
    require(App().robot_uart_.calls == expected, "late duplicate callback cannot rest twice");
}

void test_step_reads_only_body_motion_present_and_motion_degrades_ack() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    App().robot_uart_.send_ok = true;
    int seq = OpenMotionEnabledSession();
    const std::string extra =
        ",\"motion\":{\"present\":\"teach\",\"angle\":90,\"percent\":50,\"step\":2,\"delay\":1},"
        "\"outcomeMotion\":{\"present\":\"celebrate\"},\"motionPreset\":\"goodbye\"";
    Handle(StepFrame(seq, "motion", "http://poster", "http://object", "http://overlay", extra));
    const std::vector<std::string> expected = {"right_arm_raise", "head_center"};
    require(App().robot_uart_.calls == expected, "step entry reads only motion.present");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    seq = OpenMotionEnabledSession();
    Handle(StepFrame(seq, "bad-motion", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"rawSweep\",\"angle\":90}"));
    require(FrameType(Sent().size() - 1) == "lesson_ack", "unknown motion is nonfatal");
    require(FrameBodyBool(Sent().size() - 1, "degraded", false), "unknown motion marks ack degraded");
}

void test_motion_runtime_control_defaults_disabled_and_resets_per_manifest() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    App().robot_uart_.send_ok = true;
    int seq = OpenSession();
    App().robot_uart_.calls.clear();
    Handle(StepFrame(seq, "motion-off", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"celebrate\"}"));
    require(App().robot_uart_.calls.empty(), "motion defaults disabled without runtime control");
    require(FrameType(Sent().size() - 1) == "lesson_ack", "disabled motion remains nonfatal");
    require(!FrameBodyBool(Sent().size() - 1, "degraded", true),
            "operator-disabled motion is intentional, not render degradation");

    seq = OpenMotionEnabledSession();
    App().robot_uart_.calls.clear();
    Handle(StepFrame(seq, "motion-on", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"celebrate\"}"));
    require(!App().robot_uart_.calls.empty(), "manifest control enables named motion");

    seq = OpenSession();
    App().robot_uart_.calls.clear();
    Handle(StepFrame(seq, "motion-reset", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"celebrate\"}"));
    require(App().robot_uart_.calls.empty(), "fresh manifest resets motion control to disabled");
}

void test_teaching_word_telemetry_reuse_and_duplicate_ack_parity() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    int seq = OpenSession();
    Handle(StepFrame(seq, "word", "http://word-bg", "http://word-object", "http://word-overlay",
                     ",\"teachingWord\":{\"text\":\"TOO LONG FOR TFT\",\"displayText\":\"BARN\"}"));
    require(!disp.teaching_word_calls.empty() && disp.teaching_word_calls.back() == "BARN",
            "displayText renders as teaching word");
    const size_t first_ack_index = Sent().size() - 1;
    const std::string first_ack = Sent().back();
    const std::string first_body = FrameBodyJson(first_ack_index);
    require(first_ack.find("\"internalFreeBytes\"") != std::string::npos,
            "step ack reports internal SRAM telemetry");
    require(first_ack.find("\"psramFreeBytes\"") != std::string::npos,
            "step ack reports PSRAM telemetry");
    require(first_ack.find("\"layersReused\"") != std::string::npos,
            "step ack reports reuse flags");

    Handle(StepFrame(seq, "word", "http://word-bg", "http://word-object", "http://word-overlay",
                     ",\"teachingWord\":{\"displayText\":\"BARN\"}"));
    require(Sent().back().find("\"telemetry\"") != std::string::npos,
            "duplicate ack replays telemetry");
    require(FrameBodyJson(Sent().size() - 1) == first_body,
            "duplicate ack body exactly matches original serialized body");
    Handle(StopFrame(seq + 1));
    require(!disp.teaching_word_calls.empty() && disp.teaching_word_calls.back().empty(),
            "lesson stop clears teaching word pill");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    seq = OpenSession();
    Handle(StepFrame(seq, "bad-word", "http://bad-bg", "http://bad-object", "http://bad-overlay",
                     ",\"teachingWord\":{\"text\":\"ABCDEFGHIJKLM\"}"));
    require(FrameType(Sent().size() - 1) == "lesson_error", "overlong teaching word rejected");
}

void test_ack_replay_window_handles_delayed_and_expired_duplicates() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    int sequence = OpenSession();
    std::vector<std::string> bodies(21);
    for (; sequence <= 20; ++sequence) {
        Handle((sequence % 2) ? PauseFrame(sequence) : ResumeFrame(sequence));
        bodies[sequence] = FrameBodyJson(Sent().size() - 1);
    }

    Handle(PauseFrame(5));
    require(FrameBodyJson(Sent().size() - 1) == bodies[5],
            "delayed duplicate inside replay window preserves exact body");
    Handle(ResumeFrame(6));
    require(FrameBodyJson(Sent().size() - 1) == bodies[6],
            "multiple older duplicates replay independently by sequence");
    Handle(PauseFrame(3));
    require(!FrameBodyBool(Sent().size() - 1, "rendered", true) &&
            !FrameBodyBool(Sent().size() - 1, "degraded", true),
            "duplicate outside replay window receives conservative ack");
}

void test_layer_install_timeout_degrades_without_committing_layer_state() {
    ResetObservable();
    LvglDisplay disp;
    Board::GetInstance().display_ = &disp;
    int sequence = OpenSession();
    App().schedule_wait_succeeds = false;
    Handle(StepFrame(sequence, "install-timeout", "http://timeout-bg", "http://timeout-object",
                     "http://timeout-overlay", ",\"prompt\":\"Look\""));
    require(FrameBodyBool(Sent().size() - 1, "degraded", false),
            "application-task install timeout degrades step");
    require(disp.background_calls.empty() || !disp.background_calls.back(),
            "timed-out holder never installs background");

    App().schedule_wait_succeeds = true;
    Handle(StepFrame(sequence + 1, "install-retry", "http://timeout-bg", "http://timeout-object",
                     "http://timeout-overlay", ",\"prompt\":\"Look\""));
    require(!disp.background_calls.empty() && disp.background_calls.back(),
            "retry reloads and installs layer because timeout did not commit state");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    sequence = OpenSession();
    const size_t background_calls_before = disp.background_calls.size();
    App().schedule_wait_succeeds = false;
    App().schedule_wait_starts_before_timeout = true;
    Handle(StepFrame(sequence, "install-running", "http://running-bg", "http://running-object",
                     "http://running-overlay", ",\"prompt\":\"Look\""));
    require(disp.background_calls.size() == background_calls_before + 1 && disp.background_calls.back(),
            "callback already running at timeout completes authoritatively");
    const size_t installed_count = disp.background_calls.size();
    App().schedule_wait_succeeds = true;
    App().schedule_wait_starts_before_timeout = false;
    Handle(StepFrame(sequence + 1, "install-running-reuse", "http://running-bg",
                     "http://running-object", "http://running-overlay", ",\"prompt\":\"Look\""));
    require(disp.background_calls.size() == installed_count,
            "completed running callback commits layer state for reuse");
}

}  // namespace

int main() {
    test_envelope_guards();
    test_prepare_basic();
    test_prepare_assetpack_not_ready_branches();
    test_prepare_assetpack_ready_with_real_file();
    test_prepare_assetpack_trims_manifest_checksum_for_cache_key();
    test_prepare_assetpack_trims_cache_key_before_ack();
    test_prepare_assetpack_fractional_size_not_ready();
    test_prepare_assetpack_ready_without_critical_asset_list();
    test_prepare_assetpack_prefix_only_cache_key_not_ready();
    test_prepare_assetpack_stale_checksum_cache_key_not_ready();
    test_prepare_assetpack_missing_declared_critical_key_not_ready();
    test_prepare_assetpack_malformed_critical_assets_not_ready();
    test_prepare_assetpack_rejects_unbounded_count_and_total_size();
    test_prepare_assetpack_requires_manifest_checksum_before_ack();
    test_prepare_reject_clears_stale_lesson_scene();
    test_version_profile_gate();
    test_fresh_prepare_contract_reject_clears_stale_lesson_scene();
    test_unknown_session_and_staleness();
    test_dedup_reack();
    test_delayed_duplicate_prepare_replays_assetpack_after_start_ack();
    test_prepare_new_assignment_version_same_session_resets_stream();
    test_fresh_prepare_clears_stale_active_lesson_before_start();
    test_prepare_after_stop_same_session_resets_stream();
    test_prepare_during_running_same_session_resets_stream();
    test_prepare_after_terminal_error_same_session_resets_stream();
    test_start_stop_error_lifecycle();
    test_start_clears_stale_lesson_scene_before_first_step();
    test_pause_resume_drop_outside_running_and_unhandled_status_frames();
    test_pause_resume_is_acknowledged_and_child_visible();
    test_stop_reason_controls_terminal_cue();
    test_lifecycle_display_variants();
    test_step_rejects();
    test_invalid_step_clears_stale_lesson_scene();
    test_contract_reject_clears_stale_lesson_scene();
    test_step_full_render_http();
    test_step_ignores_story_metadata_while_rendering_layers();
    test_step_fetches_canonical_layer_urls_in_order();
    test_step_http_fetch_sets_short_timeout_before_open();
    test_step_reuses_cached_layer_bytes_for_repeated_urls();
    test_step_interactive_opens_listen();
    test_passive_step_cancels_prior_interactive_listen();
    test_passive_imperative_prompt_falls_back_to_narration_caption();
    test_passive_polite_imperative_prompt_falls_back_to_narration_caption();
    test_passive_try_saying_prompt_falls_back_to_narration_caption();
    test_passive_can_you_say_prompt_falls_back_to_narration_caption();
    test_passive_embedded_you_can_say_prompt_falls_back_to_narration_caption();
    test_passive_can_you_tell_me_prompt_falls_back_to_narration_caption();
    test_passive_can_you_find_prompt_falls_back_to_narration_caption();
    test_passive_step_invalidates_queued_interactive_listen_prepare();
    test_step_no_display_does_not_open_listen();
    test_step_no_display_object_does_not_open_listen();
    test_visual_only_interactive_step_does_not_open_listen_without_caption();
    test_step_blank_visible_content_does_not_open_listen();
    test_step_degraded_and_caption_fallback();
    test_step_missing_optional_object_overlay_uses_prompt_fallback();
    test_caption_truncation_preserves_utf8_boundary();
    test_invalid_utf8_interactive_prompt_does_not_open_listen();
    test_prompt_caption_collapses_internal_ascii_whitespace();
    test_prompt_caption_collapses_ascii_form_feed_and_vertical_tab();
    test_invalid_story_ask_falls_back_to_prompt_for_interactive_turn();
    test_invalid_story_ask_suffix_falls_back_to_prompt_for_interactive_turn();
    test_invalid_fallback_label_uses_valid_alt_caption();
    test_interactive_prompt_caption_trims_ascii_whitespace();
    test_step_http_error_paths();
    test_step_http_chunked_paths();
    test_decode_failure_branches();
    test_oom_guards();
    test_local_file_fetch();
    test_local_file_fetch_error_branches();
    test_sd_pack_step_draws_all_local_layers_and_waits_for_child();
    test_asset_available_fallback();
    test_flashed_poster_without_draw_is_not_drawn();
    test_remaining_reachable_branches();
    test_protocol_null_send_skip();
    test_safe_motion_presets_and_auto_rest();
    test_motion_unknown_raw_failures_and_stale_rest_are_nonfatal_degrades();
    test_queued_old_timer_callback_cannot_rest_a_new_pose_early();
    test_step_reads_only_body_motion_present_and_motion_degrades_ack();
    test_motion_runtime_control_defaults_disabled_and_resets_per_manifest();
    test_teaching_word_telemetry_reuse_and_duplicate_ack_parity();
    test_ack_replay_window_handles_delayed_and_expired_duplicates();
    test_layer_install_timeout_degrades_without_committing_layer_state();
    std::cout << "lesson host test OK (" << g_checks << " checks)\n";
    return 0;
}
