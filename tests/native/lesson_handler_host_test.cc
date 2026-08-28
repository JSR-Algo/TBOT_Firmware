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
#include "esp_log.h"
#include "esp_timer.h"
#include "lesson_handler.h"
#include "lesson_cinematic_renderer.h"
#include "lesson_flattened_cinematic_renderer.h"
#include "lesson_layered_cinematic_renderer.h"

namespace tbot {
bool LessonCourseDeliveryAppliedForTest(const char* session_id, const char* delivery_id);
void ResetLessonCourseDeliveryMemoryForTest();
void SetLessonCourseDeliveryStorageForTest(const char* serialized);
std::size_t LessonCourseDeliveryEntryCountForTest();
bool LessonCourseDeliveryStorageValidForTest();
std::string LessonCourseDeliveryStorageForTest();
void SetLessonCourseDeliveryWriteFailureForTest(bool fail);
void FailNextLessonCourseDeliveryWriteForTest(int write_number);
}
#include "lesson_asset_storage_coordinator.h"
#include "lesson_motion_presets.h"
#include "system_info.h"

#include <cJSON.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>  // access() for the /sdcard writability probe

#ifdef fread
#undef fread
#endif
#include <stdio.h>
extern "C" size_t fread(void* ptr, size_t size, size_t count, FILE* stream);

#ifdef fopen
#undef fopen
#endif
extern "C" FILE* fopen(const char* path, const char* mode);

int& HostLessonAssetOpenCount() {
    static int value = 0;
    return value;
}

extern "C" FILE* HostLessonFopen(const char* path, const char* mode) {
    ++HostLessonAssetOpenCount();
    return ::fopen(path, mode);
}

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
bool FrameBodyTelemetryBool(size_t i, const char* key, bool dflt) {
    cJSON* f = cJSON_Parse(Sent()[i].c_str());
    cJSON* b = cJSON_GetObjectItem(f, "body");
    cJSON* telemetry = b ? cJSON_GetObjectItem(b, "telemetry") : nullptr;
    cJSON* v = telemetry ? cJSON_GetObjectItem(telemetry, key) : nullptr;
    bool out = v && cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
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
bool FrameEmbodiedBool(size_t i, const char* key, bool fallback) {
    cJSON* frame = cJSON_Parse(Sent()[i].c_str());
    cJSON* body = cJSON_GetObjectItem(frame, "body");
    cJSON* embodied = body ? cJSON_GetObjectItem(body, "embodiedAction") : nullptr;
    cJSON* value = embodied ? cJSON_GetObjectItem(embodied, key) : nullptr;
    const bool out = cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
    cJSON_Delete(frame);
    return out;
}
bool FrameHasExactEmbodiedAckSchema(size_t i) {
    cJSON* frame = cJSON_Parse(Sent()[i].c_str());
    cJSON* body = frame ? cJSON_GetObjectItem(frame, "body") : nullptr;
    cJSON* embodied = body ? cJSON_GetObjectItem(body, "embodiedAction") : nullptr;
    const bool exact = frame && body && embodied && cJSON_GetArraySize(frame) == 6 &&
        cJSON_GetArraySize(body) == 2 && cJSON_GetArraySize(embodied) == 4 &&
        cJSON_GetObjectItem(frame, "type") && cJSON_GetObjectItem(frame, "assignmentId") &&
        cJSON_GetObjectItem(frame, "sessionId") && cJSON_GetObjectItem(frame, "stepId") &&
        cJSON_GetObjectItem(frame, "sequence") &&
        cJSON_GetObjectItem(embodied, "actionId") &&
        cJSON_GetObjectItem(embodied, "actionGeneration") &&
        cJSON_GetObjectItem(embodied, "outcome") &&
        cJSON_GetObjectItem(embodied, "returnedToRest");
    cJSON_Delete(frame);
    return exact;
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

bool HandleTransportJson(const std::string& json) {
    if (JsonHasForbiddenDecodedNull(json.data(), json.size())) return false;
    Handle(json);
    return true;
}

void ResetObservable() {
    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    App().HostReset();
    HostEspResetLogs();
    HostLessonAssetOpenCount() = 0;
}

bool LogContains(const std::string& needle) {
    return std::any_of(HostEspLogs().begin(), HostEspLogs().end(),
                       [&needle](const std::string& log) {
                           return log.find(needle) != std::string::npos;
                       });
}

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

std::string ReplaceOnce(std::string value, const std::string& from,
                        const std::string& to);
std::string ReplaceAll(std::string value, const std::string& from,
                       const std::string& to);
std::string ReplaceNth(std::string value, const std::string& from,
                       const std::string& to, int occurrence);

std::string PrepareFrame(int seq, const std::string& extra_body = "") {
    return std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\"" + extra_body + "}}";
}
std::string PrepareFrameFor(const std::string& assignment_id, const std::string& session_id,
                            int seq, const std::string& extra_body = "") {
    return std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + assignment_id +
           "\",\"sessionId\":\"" + session_id + "\",\"sequence\":" +
           std::to_string(seq) + ",\"body\":{\"profile\":\"" + kLessonProfileEspTft +
           "\"" + extra_body + "}}";
}
std::string StartFrame(int seq) {
    return std::string("{\"type\":\"lesson_start\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{}}";
}

std::string V2PrepareFrame(int seq, const std::string& extra_body = "") {
    return std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"sequence\":" + std::to_string(seq) +
           ",\"body\":{\"profile\":\"espTft\",\"runtimeControls\":{"
           "\"openingEntranceEnabled\":true,\"visualStateEventsEnabled\":true,"
           "\"motionPresetsEnabled\":true,\"physicalMotionOwner\":\"server\"}" +
           extra_body + "}}";
}

std::string V2StartFrame(int seq, const std::string& opening_entrance) {
    return std::string("{\"type\":\"lesson_start\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"sequence\":" + std::to_string(seq) +
           ",\"body\":{\"openingEntrance\":" + opening_entrance + "}}";
}

std::string V3Frame(const char* type, int seq, const std::string& body) {
    return std::string("{\"type\":\"") + type +
           "\",\"protocolVersion\":\"teebot-lesson-renderer.v3\",\"assignmentId\":\"" +
           AID() + "\",\"sessionId\":\"" + SID() + "\",\"sequence\":" +
           std::to_string(seq) + ",\"body\":" + body + "}";
}

std::string V3PrepareFrame(int seq, std::uint64_t command_sequence_id = 41) {
    return V3Frame("lesson_prepare", seq,
        "{\"profile\":\"espTft\",\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"directMp4Cinematic\",\"templateVersion\":1,"
        "\"phaseId\":\"opening\",\"durationMs\":300,\"fps\":10,\"frameCount\":3,"
        "\"layers\":["
        "{\"layer\":\"background\",\"slot\":\"backgroundScene\","
        "\"sdPath\":\"sd://tbot/lesson-assets/background.mp4\",\"rect\":{\"x\":0,\"y\":0,\"width\":480,\"height\":320},\"chromaKey\":null},"
        "{\"layer\":\"teachingObject\",\"slot\":\"teachingObject\","
        "\"sdPath\":\"sd://tbot/lesson-assets/object.mp4\",\"rect\":{\"x\":10,\"y\":10,\"width\":2,\"height\":2},"
        "\"chromaKey\":{\"keyColor\":\"#00ff00\",\"tolerance\":20,\"featherPx\":1}},"
        "{\"layer\":\"robotOverlay\",\"slot\":\"robotOverlay\","
        "\"sdPath\":\"sd://tbot/lesson-assets/robot.mp4\",\"rect\":{\"x\":20,\"y\":20,\"width\":2,\"height\":2},"
        "\"chromaKey\":{\"keyColor\":\"#00ff00\",\"tolerance\":20,\"featherPx\":1}}]}}"
    );
}

std::string V4Frame(const char* type, int seq, const std::string& body) {
    return std::string("{\"type\":\"") + type +
           "\",\"protocolVersion\":\"teebot-lesson-renderer.v4\",\"assignmentId\":\"" +
           AID() + "\",\"sessionId\":\"" + SID() + "\",\"sequence\":" +
           std::to_string(seq) + ",\"body\":" + body + "}";
}

std::string V5Frame(const char* type, int seq, const std::string& body) {
    return std::string("{\"type\":\"") + type +
           "\",\"protocolVersion\":\"teebot-lesson-renderer.v5\",\"assignmentId\":\"" +
           AID() + "\",\"sessionId\":\"" + SID() + "\",\"sequence\":" +
           std::to_string(seq) + ",\"body\":" + body + "}";
}

std::string V5PrepareFrame(int seq, std::uint64_t command_sequence_id = 91) {
    const std::string hash(64, 'a');
    return V5Frame("lesson_prepare", seq,
        "{\"profile\":\"espTft\",\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"layeredCinematic\",\"templateVersion\":1,"
        "\"phaseId\":\"flyIn\",\"durationMs\":300,\"fps\":10,\"frameCount\":3,"
        "\"playbackMode\":\"once\",\"layers\":["
        "{\"layer\":\"background\",\"slot\":\"backgroundScene\","
        "\"mediaKind\":\"image\",\"mediaType\":\"image/jpeg\","
        "\"sdPath\":\"sd://tbot/lesson-assets/background.jpg\",\"sha256\":\"" + hash +
        "\",\"bytes\":1234,\"width\":480,\"height\":320,"
        "\"rect\":{\"x\":0,\"y\":0,\"width\":480,\"height\":320},\"fit\":\"cover\"},"
        "{\"layer\":\"teachingObject\",\"slot\":\"teachingObject\","
        "\"mediaKind\":\"image\",\"mediaType\":\"image/png\","
        "\"sdPath\":\"sd://tbot/lesson-assets/object.png\",\"sha256\":\"" + hash +
        "\",\"bytes\":234,\"width\":2,\"height\":2,"
        "\"rect\":{\"x\":10,\"y\":10,\"width\":2,\"height\":2},\"fit\":\"contain\"},"
        "{\"layer\":\"robotOverlay\",\"slot\":\"robotOverlay\","
        "\"mediaKind\":\"video\",\"mediaType\":\"video/mp4\","
        "\"sdPath\":\"sd://tbot/lesson-assets/robot.mp4\",\"sha256\":\"" + hash +
        "\",\"bytes\":3456,\"width\":2,\"height\":2,"
        "\"rect\":{\"x\":20,\"y\":20,\"width\":2,\"height\":2},"
        "\"codec\":\"mjpeg\",\"hasAudio\":false,"
        "\"chromaKey\":{\"keyColor\":\"#00ff00\",\"tolerance\":20,\"featherPx\":1}}]}}"
    );
}

std::string V5CourseModeCompatibilityJson() {
    return "{\"schemaVersion\":1,"
        "\"contractChecksum\":\"332fb68e340abb94c0178dd83b06ed0939d6e2d63c17d48bcb09dab8cc6bb3be\","
        "\"layoutContract\":\"layeredCinematic\","
        "\"lessonId\":\"course-mode-v5-farm-candidate\",\"lessonVersion\":2,"
        "\"manifestChecksum\":\"22e94ced4b2dae1ced13f3e34de1f72e8a3ce177e1ba3a7c599a4c3d002aea0d\"}";
}

constexpr const char* kV5CourseModeManifestChecksum =
    "22e94ced4b2dae1ced13f3e34de1f72e8a3ce177e1ba3a7c599a4c3d002aea0d";
constexpr const char* kV5CourseModePackName = "course-mode-v5-dynamic-pack";
constexpr const char* kV5CourseModeAssetIds[3] = {
    "75000000-0000-4000-8000-000000000011",
    "75000000-0000-4000-8000-000000000022",
    "75000000-0000-4000-8000-000000000031"};
constexpr std::size_t kV5CourseModeAssetSizes[3] = {43599, 15086, 223033};
constexpr const char* kV5DynamicCourseModeAssetIds[3] = {
    "85000000-0000-4000-8000-000000000011",
    "85000000-0000-4000-8000-000000000022",
    "85000000-0000-4000-8000-000000000031"};

std::string V5CourseModePackRoot() {
    return std::string("sd://sdcard/tbot/lesson-assets/") + kV5CourseModePackName;
}

std::string V5CourseModeAssetPath(int index) {
    return V5CourseModePackRoot() + "/" + kV5CourseModeAssetIds[index];
}

void StageV5CourseModeAssetPack() {
    const std::string host_root = std::string("/tmp/") + kV5CourseModePackName;
    require(system(("mkdir -p " + host_root).c_str()) == 0,
            "Course Mode v5 asset-pack directory is staged");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", "/tmp", 1);
    for (const auto* ids : {kV5CourseModeAssetIds, kV5DynamicCourseModeAssetIds}) {
        for (int index = 0; index < 3; ++index) {
            const std::string path = host_root + "/" + ids[index];
            FILE* file = fopen(path.c_str(), "wb");
            require(file != nullptr, "Course Mode v5 sparse asset opens");
            require(fseek(file, static_cast<long>(kV5CourseModeAssetSizes[index] - 1), SEEK_SET) == 0 &&
                        fputc(0, file) != EOF,
                    "Course Mode v5 sparse asset reaches exact declared size");
            fclose(file);
        }
    }
}

void RemoveV5CourseModeAssetPack() {
    const std::string host_root = std::string("/tmp/") + kV5CourseModePackName;
    for (const auto* ids : {kV5CourseModeAssetIds, kV5DynamicCourseModeAssetIds}) {
        for (int index = 0; index < 3; ++index) {
            remove((host_root + "/" + ids[index]).c_str());
        }
    }
    rmdir(host_root.c_str());
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

std::string V5CourseModePrepareFrame(int seq, std::uint64_t command_sequence_id = 491,
                                     const std::string& phase_id = "teach",
                                     bool include_asset_pack = true) {
    std::string body = "{\"profile\":\"espTft\",";
    if (include_asset_pack) {
        body += std::string("\"manifestRef\":{\"manifestChecksum\":\"") +
        kV5CourseModeManifestChecksum +
        "\"},\"assetPack\":{\"cacheKey\":\"course-mode-v5-" +
        kV5CourseModeManifestChecksum + "\",\"localRoot\":\"" +
        V5CourseModePackRoot() + "\",\"ready\":true,\"assets\":["
        "{\"key\":\"" + kV5CourseModeAssetIds[0] +
        "\",\"state\":\"READY\",\"checksumOk\":true,\"sha256\":\"d4abb6087dc3122e0a00feb5e6a86b03dc7db550eb59d25e92f54d0fd09e4fc0\",\"mediaType\":\"image/jpeg\",\"size\":43599},"
        "{\"key\":\"" + kV5CourseModeAssetIds[1] +
        "\",\"state\":\"READY\",\"checksumOk\":true,\"sha256\":\"c466239ff8ba202998e3827b6871906d7fbac6232aeaea3a59b7c69bec7d8777\",\"mediaType\":\"image/png\",\"size\":15086},"
        "{\"key\":\"" + kV5CourseModeAssetIds[2] +
        "\",\"state\":\"READY\",\"checksumOk\":true,\"sha256\":\"f2d496b5e750e895f7e086aec827d7b99d0bb322d73ea660a2e84ff484b602c4\",\"mediaType\":\"video/mp4\",\"size\":223033}]},"
        ;
    }
    body +=
        "\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"layeredCinematic\",\"templateVersion\":1,"
        "\"phaseId\":\"" + phase_id +
        "\",\"durationMs\":3000,\"fps\":10,\"frameCount\":30,"
        "\"playbackMode\":\"once\",\"courseModeCompatibility\":" +
        V5CourseModeCompatibilityJson() + ",\"layers\":["
        "{\"layer\":\"background\",\"slot\":\"backgroundScene\","
        "\"assetVersionId\":\"75000000-0000-4000-8000-000000000011\","
        "\"mediaKind\":\"image\",\"mediaType\":\"image/jpeg\","
        "\"sdPath\":\"" + V5CourseModeAssetPath(0) + "\","
        "\"sha256\":\"d4abb6087dc3122e0a00feb5e6a86b03dc7db550eb59d25e92f54d0fd09e4fc0\","
        "\"bytes\":43599,\"width\":480,\"height\":320,"
        "\"rect\":{\"x\":0,\"y\":0,\"width\":480,\"height\":320},\"fit\":\"cover\"},"
        "{\"layer\":\"teachingObject\",\"slot\":\"teachingObject\","
        "\"assetVersionId\":\"75000000-0000-4000-8000-000000000022\","
        "\"mediaKind\":\"image\",\"mediaType\":\"image/png\","
        "\"sdPath\":\"" + V5CourseModeAssetPath(1) + "\","
        "\"sha256\":\"c466239ff8ba202998e3827b6871906d7fbac6232aeaea3a59b7c69bec7d8777\","
        "\"bytes\":15086,\"width\":95,\"height\":95,"
        "\"rect\":{\"x\":20,\"y\":168,\"width\":95,\"height\":95},\"fit\":\"contain\"},"
        "{\"layer\":\"robotOverlay\",\"slot\":\"robotOverlay\","
        "\"assetVersionId\":\"75000000-0000-4000-8000-000000000031\","
        "\"mediaKind\":\"video\",\"mediaType\":\"video/mp4\","
        "\"sdPath\":\"" + V5CourseModeAssetPath(2) + "\","
        "\"sha256\":\"f2d496b5e750e895f7e086aec827d7b99d0bb322d73ea660a2e84ff484b602c4\","
        "\"bytes\":223033,\"width\":240,\"height\":240,"
        "\"rect\":{\"x\":118,\"y\":160,\"width\":150,\"height\":150},"
        "\"codec\":\"mjpeg\",\"hasAudio\":false,"
        "\"chromaKey\":{\"keyColor\":\"#00ff00\",\"tolerance\":20,\"featherPx\":1}}]}}";
    return V5Frame("lesson_prepare", seq, body);
}

std::string V5CourseModeActivityFallbackPrepareFrame(
    int seq, std::uint64_t command_sequence_id = 191) {
    std::string frame = V5CourseModePrepareFrame(seq, command_sequence_id, "listen");
    frame = ReplaceOnce(
        frame, "\"playbackMode\":\"once\",\"courseModeCompatibility\":",
        "\"playbackMode\":\"once\",\"activityIds\":[\"w19-weather-recall\"],"
        "\"courseModeCompatibility\":");
    frame = ReplaceOnce(
        frame, V5CourseModeCompatibilityJson(),
        "{\"schemaVersion\":1,"
        "\"contractChecksum\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"layoutContract\":\"layeredCinematic\","
        "\"lessonId\":\"english-6month-week-19\",\"lessonVersion\":1,"
        "\"manifestChecksum\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}");
    frame = ReplaceAll(
        frame, kV5CourseModeManifestChecksum,
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    for (int index = 0; index < 3; ++index) {
        frame = ReplaceAll(frame, kV5CourseModeAssetIds[index],
                           kV5DynamicCourseModeAssetIds[index]);
    }
    frame = ReplaceAll(
        frame, "d4abb6087dc3122e0a00feb5e6a86b03dc7db550eb59d25e92f54d0fd09e4fc0",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    frame = ReplaceAll(
        frame, "f2d496b5e750e895f7e086aec827d7b99d0bb322d73ea660a2e84ff484b602c4",
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    frame = ReplaceAll(
        frame, "\"rect\":{\"x\":118,\"y\":160,\"width\":150,\"height\":150}",
        "\"rect\":{\"x\":180,\"y\":120,\"width\":120,\"height\":120}");
    frame = ReplaceAll(frame, "\"keyColor\":\"#00ff00\"", "\"keyColor\":\"#00aa00\"");
    const std::string object_start = "{\"layer\":\"teachingObject\"";
    const std::string robot_start = "{\"layer\":\"robotOverlay\"";
    const auto begin = frame.find(object_start);
    const auto end = frame.find(robot_start, begin);
    require(begin != std::string::npos && end != std::string::npos,
            "Course Mode fallback payload contains removable object layer");
    frame.erase(begin, end - begin);
    return frame;
}

std::string CourseActivityFrame(int seq, const char* delivery_id,
                                const char* activity_id = "w19-weather-recall",
                                const char* visual_state = "listen",
                                bool replay_entrance = false) {
    return std::string("{\"type\":\"lesson_course_activity\",\"assignmentId\":\"") +
        AID() + "\",\"sessionId\":\"" + SID() + "\",\"stepId\":\"a1\"," +
        "\"sequence\":" + std::to_string(seq) + ",\"body\":{" +
        "\"contractVersion\":\"courseCompanion.v2.contract.v1\"," +
        "\"deliveryId\":\"" + delivery_id + "\",\"activityId\":\"" + activity_id +
        "\",\"visualState\":\"" + visual_state +
        "\",\"embodiedIntent\":\"PRESENT_CENTER\"," +
        "\"retainStaticLayers\":true,\"replayEntrance\":" +
        (replay_entrance ? "true" : "false") + "}}";
}

std::string V4PrepareFrame(int seq, std::uint64_t command_sequence_id = 71,
                           const std::string& asset_extra = "") {
    return V4Frame("lesson_prepare", seq,
        "{\"profile\":\"espTft\",\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"flattenedMjpegCinematic\",\"templateVersion\":1,"
        "\"phaseId\":\"opening\",\"durationMs\":300,\"fps\":10,\"frameCount\":3,"
        "\"asset\":{\"derivativeId\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\","
        "\"phaseId\":\"opening\",\"sdPath\":\"sd://tbot/lesson-assets/flattenedCinematic.opening\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"bytes\":1234,\"mediaType\":\"video/mp4\",\"width\":480,\"height\":320" +
        asset_extra + "}}}" );
}

std::string V4V2PrepareFrame(int seq, std::uint64_t command_sequence_id = 81,
                             const std::string& command_extra = "",
                             const std::string& asset_extra = "",
                             const std::string& cue_id = "barn-correct",
                             const std::string& effect = "correct",
                             const std::string& playback_mode = "once",
                             int duration_ms = 600, int frame_count = 6) {
    return V4Frame("lesson_prepare", seq,
        "{\"profile\":\"espTft\",\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"flattenedMjpegCinematic\",\"templateVersion\":2,"
        "\"cueId\":\"" + cue_id + "\",\"effect\":\"" + effect +
        "\",\"stepKey\":\"barn\",\"playbackMode\":\"" + playback_mode +
        "\",\"durationMs\":" + std::to_string(duration_ms) +
        ",\"fps\":10,\"frameCount\":" + std::to_string(frame_count) + ","
        "\"asset\":{\"derivativeId\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\","
        "\"cueId\":\"" + cue_id + "\","
        "\"sdPath\":\"sd://tbot/lesson-assets/flattenedCinematic." + cue_id + "\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"bytes\":1234,\"mediaType\":\"video/mp4\",\"width\":480,\"height\":320" +
        asset_extra + "}" + command_extra + "}}" );
}

std::string V4V2BarnCuePrepareFrame(int seq, std::uint64_t command_sequence_id = 171,
                                    const std::string& body_extra = "") {
    return V4Frame("lesson_prepare", seq,
        "{\"profile\":\"espTft\",\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"flattenedMjpegCinematic\",\"templateVersion\":2,"
        "\"cueId\":\"barn-opening\",\"effect\":\"opening\",\"stepKey\":\"barn\","
        "\"playbackMode\":\"once\",\"durationMs\":9500,\"fps\":10,\"frameCount\":95,"
        "\"asset\":{\"derivativeId\":\"9d699633809f46d1a75ae772c25c74335da329416b8e48b56b6a089b48b6ef31\","
        "\"cueId\":\"barn-opening\",\"sdPath\":\"sd://tbot/lesson-assets/flattenedCinematic.barn-opening\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"bytes\":123456,\"mediaType\":\"video/mp4\",\"width\":480,\"height\":320}}" +
        body_extra + "}");
}

std::string CourseModeCompatibilityJson() {
    return "{\"schemaVersion\":1,"
        "\"contractChecksum\":\"cf12b1a5f71f0a80a8ee22bb2cdc775ada5b803e26d154e5d29c76b14c9fb264\","
        "\"layoutContract\":\"renderer-v4.course-mode-layout.v1\","
        "\"lessonId\":\"course-mode-pilot-cat-ball\",\"lessonVersion\":1,"
        "\"manifestChecksum\":\"205784b3f97cb081ce9c226d8fd83fdd400401e706c000e1b09ba4e7ebdf36ce\"}";
}

std::string V4CourseModePrepareFrame(int seq, std::uint64_t command_sequence_id = 281) {
    const std::string compatibility = CourseModeCompatibilityJson();
    return V4Frame("lesson_prepare", seq,
        "{\"profile\":\"espTft\",\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"flattenedMjpegCinematic\",\"templateVersion\":2,"
        "\"cueId\":\"cat-discover\",\"effect\":\"teach\","
        "\"stepKey\":\"cat-discover\",\"playbackMode\":\"once\","
        "\"durationMs\":2000,\"fps\":10,\"frameCount\":20,"
        "\"asset\":{"
        "\"derivativeId\":\"6c5d8ee1c2695a12dfa8202df5d0820b360aeca5a15662583efb97e812c99f66\","
        "\"cueId\":\"cat-discover\","
        "\"sdPath\":\"sd://tbot/lesson-assets/flattenedCinematic.cat-discover\","
        "\"sha256\":\"ebeaf4e8159b17da82d615f359272e26e7e81a1f005183335feba5b702f98d72\","
        "\"bytes\":134626,\"mediaType\":\"video/mp4\",\"width\":480,\"height\":320,"
        "\"courseModeCompatibility\":" + compatibility + "},"
        "\"courseModeCompatibility\":" + compatibility + "}}" );
}

std::string V4TrgbPrepareFrame(int seq, std::uint64_t command_sequence_id = 271,
                               const std::string& asset_extra = "",
                               const std::string& body_extra = "") {
    return V4Frame("lesson_prepare", seq,
        "{\"profile\":\"espTft\",\"cinematicPhase\":{"
        "\"command\":\"prepare\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) +
        ",\"templateId\":\"flattenedMjpegCinematic\",\"templateVersion\":2,"
        "\"cueId\":\"barn-opening\",\"effect\":\"opening\",\"stepKey\":\"barn\","
        "\"playbackMode\":\"once\",\"durationMs\":9500,\"fps\":10,\"frameCount\":95,"
        "\"asset\":{\"derivativeId\":\"9d699633809f46d1a75ae772c25c74335da329416b8e48b56b6a089b48b6ef31\","
        "\"cueId\":\"barn-opening\",\"sdPath\":\"sd://tbot/lesson-assets/barn-opening.trgb\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"bytes\":29186048,\"mediaType\":\"application/vnd.tbot.rgb565-indexed\","
        "\"containerVersion\":1,\"storedWidth\":320,\"storedHeight\":480,"
        "\"orientation\":\"panelNativeClockwise\",\"frameBytes\":307200" +
        asset_extra + "}}" + body_extra + "}");
}

std::string V4V2BarnCueCommandFrame(const char* type, const char* command, int seq,
                                    std::uint64_t command_sequence_id,
                                    const std::string& extra_body = "") {
    const std::string command_body = std::string("{\"command\":\"") + command +
        "\",\"cueId\":\"barn-opening\",\"commandSequenceId\":" +
        std::to_string(command_sequence_id) + extra_body + "}";
    if (std::string(type) == "lesson_cinematic_control") {
        return V4Frame(type, seq, command_body);
    }
    return V4Frame(type, seq,
        std::string("{\"cinematicPhase\":") + command_body + "}");
}

std::string WithSession(std::string frame, const std::string& session_id) {
    const std::string current = std::string("\"sessionId\":\"") + SID() + "\"";
    const std::string replacement = std::string("\"sessionId\":\"") + session_id + "\"";
    const auto position = frame.find(current);
    require(position != std::string::npos, "fixture contains current session identity");
    frame.replace(position, current.size(), replacement);
    return frame;
}

const char* ValidV2OpeningEntrance() {
    return "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"oncePerLessonSession\","
           "\"layoutPreset\":\"centerRoad\",\"backgroundAssetKey\":\"scene.farm\","
           "\"robotAssetKey\":\"robotOverlay.teach\",\"fallback\":\"staticGreet\"}";
}

std::string ValidV2OpeningEntranceForLayout(const char* layout) {
    return std::string("{\"preset\":\"flyLandWalkGreet\",\"policy\":\"oncePerLessonSession\","
           "\"layoutPreset\":\"") + layout + "\",\"backgroundAssetKey\":\"scene.farm\","
           "\"robotAssetKey\":\"robotOverlay.teach\",\"fallback\":\"staticGreet\"}";
}

std::string V2VisualFrameWithGeneration(int seq, const char* state,
                                        const std::string& generation_json) {
    return std::string("{\"type\":\"lesson_visual_state\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"stepId\":\"s2\",\"sequence\":" + std::to_string(seq) +
           ",\"body\":{\"state\":\"" + state + "\",\"overlayKey\":\"thinking\","
           "\"motionPreset\":\"encourage\",\"visualGeneration\":" + generation_json + "}}";
}

std::string V2VisualFrameWithStepId(int seq, const char* step_id, std::uint64_t generation) {
    return std::string("{\"type\":\"lesson_visual_state\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"stepId\":\"" + step_id + "\",\"sequence\":" + std::to_string(seq) +
           ",\"body\":{\"state\":\"thinking\",\"overlayKey\":\"thinking\","
           "\"motionPreset\":\"encourage\",\"visualGeneration\":" +
           std::to_string(generation) + "}}";
}

std::string V2VisualFrame(int seq, const char* state, std::uint64_t generation) {
    return V2VisualFrameWithGeneration(seq, state, std::to_string(generation));
}

std::string V2StepFrame(int seq, const std::string& step_id) {
    return std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"stepId\":\"" + step_id + "\",\"lessonVersion\":3,\"lessonId\":\"L1\"," +
           "\"sequence\":" + std::to_string(seq) +
           ",\"body\":{\"profile\":\"espTft\",\"stepType\":\"model\"," +
           "\"completionClass\":\"passive\",\"prompt\":\"Ready\",\"scene\":{" +
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}}," +
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}}," +
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},\"expression\":\"teaching\"}}}}";
}

std::string V2PauseFrame(int seq) {
    return std::string("{\"type\":\"lesson_pause\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"sequence\":" + std::to_string(seq) + ",\"body\":{}}";
}
std::string V2ResumeFrame(int seq) {
    return std::string("{\"type\":\"lesson_resume\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"sequence\":" + std::to_string(seq) + ",\"body\":{}}";
}
std::string StopFrame(int seq, const std::string& body = "") {
    return std::string("{\"type\":\"lesson_stop\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{" + body + "}}";
}
std::string StopFrameFor(const std::string& assignment_id, const std::string& session_id,
                         int seq, const std::string& body = "") {
    return std::string("{\"type\":\"lesson_stop\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + assignment_id +
           "\",\"sessionId\":\"" + session_id + "\",\"sequence\":" +
           std::to_string(seq) + ",\"body\":{" + body + "}}";
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
std::string EmbodiedActionFrame(int seq, const std::string& step_id,
                                const std::string& action_id, int generation,
                                const char* intent = "PRESENT_LEFT",
                                const char* focus = "focus.left.choice") {
    return std::string("{\"type\":\"lesson_embodied_action\",\"assignmentId\":\"") +
           AID() + "\",\"sessionId\":\"" + SID() + "\",\"stepId\":\"" + step_id +
           "\",\"sequence\":" + std::to_string(seq) +
           ",\"body\":{\"actionId\":\"" + action_id +
           "\",\"actionGeneration\":" + std::to_string(generation) +
           ",\"intent\":\"" + intent + "\",\"visualFocusRegion\":\"" + focus +
           "\",\"listenWindowPolicy\":\"complete_before_listening\"}}";
}
std::string EmbodiedCancelFrame(int seq, const std::string& step_id,
                                const std::string& action_id, int generation) {
    return std::string("{\"type\":\"lesson_embodied_cancel\",\"assignmentId\":\"") +
           AID() + "\",\"sessionId\":\"" + SID() + "\",\"stepId\":\"" + step_id +
           "\",\"sequence\":" + std::to_string(seq) +
           ",\"body\":{\"actionId\":\"" + action_id +
           "\",\"actionGeneration\":" + std::to_string(generation) + "}}";
}
std::string ErrorFrame(int seq) {
    return std::string("{\"type\":\"lesson_error\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"sequence\":" + std::to_string(seq) + ",\"body\":{\"code\":\"STEP_TIMEOUT\"}}";
}
std::string ReadyAssetPackExtra(const std::string& cache_key,
                                const std::string& manifest_checksum,
                                const std::string& file_name) {
    const std::string host_path = "/tmp/" + file_name;
    FILE* file = fopen(host_path.c_str(), "wb");
    require(file != nullptr, "ready asset fixture opened");
    require(fwrite("data", 1, 4, file) == 4, "ready asset fixture written");
    fclose(file);
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", "/tmp", 1);
    return ",\"manifestRef\":{\"manifestChecksum\":\"" + manifest_checksum +
           "\"},\"assetPack\":{\"cacheKey\":\"" + cache_key +
           "\",\"assets\":[{\"key\":\"poster\",\"state\":\"READY\","
           "\"checksumOk\":true,\"localPath\":\"sd://sdcard/tbot/lesson-assets/" +
           file_name + "\",\"size\":4}]}";
}
void RemoveReadyAssetPackFixture(const std::string& file_name) {
    remove(("/tmp/" + file_name).c_str());
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}
// Full three-layer lesson_step. Caller controls scene srcs + extra body fields.
std::string StepFrame(int seq, const std::string& step_id,
                      const std::string& poster_src, const std::string& object_src,
                      const std::string& overlay_src, const std::string& extra_body = "",
                      const std::string& extra_scene = "",
                      const std::string& robot_state = "talking") {
    std::string scene =
        "\"scene\":{"
        "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"" + poster_src + "\"}},"
        "\"teachingObject\":{\"asset\":{\"src\":\"" + object_src + "\"}},"
        "\"robotOverlay\":{\"asset\":{\"src\":\"" + overlay_src +
        "\"},\"expression\":\"teaching\",\"robotState\":\"" + robot_state + "\"}"
        + extra_scene + "}";
    return std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\"," + "\"stepId\":\"" + step_id + "\",\"lessonVersion\":3,\"lessonId\":\"L1\"," +
           "\"sequence\":" + std::to_string(seq) + ",\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\"" + extra_body + "," + scene + "}}";
}

std::string TvideoProjection(const std::string& phase_override = "",
                             const std::string& arrived_override = "") {
    const std::string phases = phase_override.empty()
        ? "[{\"name\":\"hidden\",\"durationMs\":100},"
          "{\"name\":\"flyIn\",\"durationMs\":1200},"
          "{\"name\":\"landFar\",\"durationMs\":700},"
          "{\"name\":\"settle\",\"durationMs\":350},"
          "{\"name\":\"walkToward\",\"durationMs\":1800},"
          "{\"name\":\"arriveNear\",\"durationMs\":250},"
          "{\"name\":\"greetIdle\",\"durationMs\":650},"
          "{\"name\":\"revealTeachingContent\",\"durationMs\":100}]"
        : phase_override;
    const std::string arrived = arrived_override.empty()
        ? "{\"versionId\":\"pose-v1\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
          "\"bytes\":4096,\"mediaType\":\"image/png\"}"
        : arrived_override;
    return "{\"templateId\":\"tvideoFlyWalk\",\"templateVersion\":1,"
           "\"layoutPreset\":\"centerRoad\",\"geometryVersion\":1,"
           "\"phases\":" + phases + ",\"revealPhase\":\"revealTeachingContent\","
           "\"fallbackPolicy\":\"snapToArriveNearAndReveal\","
           "\"background\":{\"versionId\":\"bg-v1\",\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
           "\"bytes\":8192,\"mediaType\":\"image/jpeg\"},\"arrivedPose\":" + arrived + "}";
}

std::string ReplaceOnce(std::string value, const std::string& from, const std::string& to) {
    const size_t position = value.find(from);
    require(position != std::string::npos, "projection test replacement target exists");
    value.replace(position, from.size(), to);
    return value;
}

std::string ReplaceAll(std::string value, const std::string& from, const std::string& to) {
    require(!from.empty(), "projection test replacement target is nonempty");
    size_t position = 0;
    bool replaced = false;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
        replaced = true;
    }
    require(replaced, "projection test replacement target exists");
    return value;
}

std::string ReplaceNth(std::string value, const std::string& from,
                       const std::string& to, int occurrence) {
    require(occurrence > 0, "projection replacement occurrence is positive");
    size_t position = 0;
    for (int index = 0; index < occurrence; ++index) {
        position = value.find(from, position);
        require(position != std::string::npos, "projection nth replacement target exists");
        if (index + 1 < occurrence) position += from.size();
    }
    value.replace(position, from.size(), to);
    return value;
}

std::string TvideoStepFrame(int seq, const std::string& projection,
                            const std::string& overlay_metadata =
                                "\"key\":\"pose-v1\","
                                "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                                "") {
    return std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"stepId\":\"tvideo\",\"lessonVersion\":3,\"lessonId\":\"L1\","
           "\"sequence\":" + std::to_string(seq) + ",\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"stepType\":\"greeting\",\"templateProjection\":" + projection +
           ",\"scene\":{\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\"}},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.jpg\"}},"
           "\"robotOverlay\":{\"asset\":{" + overlay_metadata +
           "\"src\":\"http://x/r.png\"},\"expression\":\"teaching\"}}}}";
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

void test_embodied_action_capability_and_async_terminal_ack() {
    ResetObservable();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;

    cJSON* features = cJSON_CreateObject();
    AddLessonRendererFeatures(features);
    cJSON* capability = cJSON_GetObjectItem(features, "lessonCourseMode");
    require(cJSON_IsObject(capability), "course mode capability is advertised after initialization");
    require(cJSON_GetObjectItem(capability, "version")->valueint == 2,
            "course mode capability pins version 2");
    require(cJSON_IsTrue(cJSON_GetObjectItem(capability, "embodiedActions")),
            "course mode capability advertises embodied actions");
    cJSON_Delete(features);

    const int sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "cat-meaning-left-right-01", "http://x/p.jpg",
                     "http://x/o.jpg", "http://x/r.jpg",
                     ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    const size_t before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "cat-meaning-left-right-01",
                               "session:present-left:1", 1));
    require(Sent().size() == before, "embodied action waits asynchronously for hold and rest");
    require(display.last_emotion == "neutral", "embodied action applies its supportive face");
    require(App().robot_uart_.calls.size() == 3, "embodied action dispatches one safe servo preset");
    HostEspFireTimer();
    require(Sent().size() == before, "timer callback only enqueues onto lesson worker");
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == before, "hold completion restores before settle ACK");
    HostEspFireTimer();
    require(Sent().size() == before, "settle timer also emits no Protocol frame directly");
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == before + 1, "embodied action emits one terminal ACK after settle");
    require(FrameBodyNum(before, "acks") == sequence + 1,
            "embodied terminal ACK echoes the server sequence");
    require(FrameBodyStr(before, "embodiedAction", "actionId") == "session:present-left:1",
            "embodied terminal ACK echoes action identity");
    require(FrameBodyStr(before, "embodiedAction", "outcome") == "applied",
            "embodied terminal ACK uses an allowed firmware outcome");
    require(FrameHasExactEmbodiedAckSchema(before),
            "embodied terminal ACK has the frozen closed schema");
    require(FrameEmbodiedBool(before, "returnedToRest", false),
            "embodied terminal ACK confirms rest only after settle");
    Board::GetInstance().display_ = nullptr;
}

void test_embodied_action_reduced_motion_and_partial_servo_degrade() {
    ResetObservable();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    int sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "reduced-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    tbot::SetLessonCourseModeCapabilityForTest(true, true);
    const size_t before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "reduced-step", "reduced:1", 1));
    require(App().robot_uart_.calls.empty(), "reduced motion never dispatches a servo command");
    require(display.last_emotion == "neutral", "reduced motion still applies the named face");
    HostEspFireTimer();
    require(Sent().size() == before, "reduced settle callback only enqueues");
    App().DrainLessonEmbodiedQueue();
    require(FrameBodyStr(before, "embodiedAction", "outcome") == "degraded",
            "reduced motion ACK is degraded");
    require(FrameEmbodiedBool(before, "returnedToRest", false),
            "reduced motion is already at rest when terminal ACK is emitted");
    tbot::SetLessonCourseModeCapabilityForTest(true, false);
    Board::GetInstance().display_ = nullptr;

    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "partial-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    App().robot_uart_.left_ok = false;
    const size_t partial_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "partial-step", "partial:1", 1));
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(FrameBodyStr(partial_before, "embodiedAction", "outcome") == "degraded",
            "one-servo failure degrades without failing the lesson");
    Board::GetInstance().display_ = nullptr;
}

void test_embodied_action_cancel_duplicate_and_supersession_are_safe() {
    ResetObservable();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    int sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "lifecycle-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    const size_t before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "lifecycle-step", "lifecycle:1", 1));
    const auto stale_callback = HostEspQueueTimerCallback();
    Handle(EmbodiedActionFrame(sequence + 2, "lifecycle-step", "lifecycle:2", 2,
                               "PRESENT_RIGHT", "focus.right.choice"));
    require(App().robot_uart_.calls.size() == 9,
            "newer action restores the old pose before applying replacement");
    HostEspInvokeQueuedCallback(stale_callback);
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == before, "superseded timer output is suppressed without ACK");

    const auto replaced_hold_callback = HostEspQueueTimerCallback();
    Handle(EmbodiedCancelFrame(sequence + 3, "lifecycle-step", "lifecycle:2", 2));
    require(Sent().size() == before, "matching cancel waits for the settle interval");
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == before + 1 &&
                FrameBodyStr(before, "embodiedAction", "outcome") == "applied",
            "matching cancel terminates with an allowed applied ACK after rest");
    require(FrameBodyNum(before, "acks") == sequence + 3,
            "matching cancel ACK echoes the cancel frame sequence");
    HostEspInvokeQueuedCallback(replaced_hold_callback);
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == before + 1,
            "cancel suppresses the replaced hold callback after terminal ACK");

    const size_t after_cancel = Sent().size();
    Handle(EmbodiedCancelFrame(sequence + 4, "lifecycle-step", "unknown", 99));
    require(Sent().size() == after_cancel,
            "unknown cancel is an idempotent no-op without motion or ACK");
    Handle(EmbodiedActionFrame(sequence + 5, "lifecycle-step", "lifecycle:1", 3));
    require(Sent().size() == after_cancel + 1 &&
                FrameBodyStr(after_cancel, "embodiedAction", "outcome") == "rejected",
            "consumed action identity is rejected instead of replaying motion");
    const size_t after_rejected = Sent().size();
    std::string unsafe_generation = EmbodiedActionFrame(
        sequence + 6, "lifecycle-step", "unsafe-generation", 4);
    unsafe_generation = ReplaceOnce(
        unsafe_generation, "\"actionGeneration\":4",
        "\"actionGeneration\":9007199254740992");
    Handle(unsafe_generation);
    require(Sent().size() == after_rejected,
            "unsafe non-exact generation is rejected without casting it into an ACK");
    Board::GetInstance().display_ = nullptr;
}

void test_embodied_action_fail_closed_capability_focus_and_timer_failure() {
    tbot::SetLessonCourseModeCapabilityForTest(false, false);
    cJSON* features = cJSON_CreateObject();
    AddLessonRendererFeatures(features);
    require(cJSON_GetObjectItem(features, "lessonCourseMode") == nullptr,
            "course mode capability is absent until the complete path is ready");
    cJSON_Delete(features);
    tbot::SetLessonCourseModeCapabilityForTest(true, false);

    ResetObservable();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    const int sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "focus-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg",
                     ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    HostEspTimerStartOk() = false;
    const size_t before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "focus-step", "timer-failure:1", 1));
    require(std::find(display.lesson_focus_calls.begin(), display.lesson_focus_calls.end(),
                      "focus.left.choice") != display.lesson_focus_calls.end(),
            "production display applies the exact authored focus anchor without text inference");
    require(Sent().size() == before + 1 &&
                FrameBodyStr(before, "embodiedAction", "outcome") == "rejected" &&
                !FrameEmbodiedBool(before, "returnedToRest", true),
            "timer failure rejects without falsely confirming physical rest");
    HostEspTimerStartOk() = true;
    Board::GetInstance().display_ = nullptr;
}

void CompleteEmbodiedAction() {
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
}

void test_embodied_software_journeys_16_through_20() {
    // Journey 16: authored left/right focus stays aligned with the safe physical preset.
    ResetObservable();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    int sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "alignment-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "alignment-step", "journey-16-left", 1));
    require(!display.lesson_focus_calls.empty() &&
                display.lesson_focus_calls.back() == "focus.left.choice" &&
                App().robot_uart_.calls ==
                    std::vector<std::string>({"head_percent:25", "left_percent:45", "right_percent:0"}),
            "journey 16 keeps left focus and motion aligned");
    Handle(EmbodiedActionFrame(sequence + 2, "alignment-step", "journey-16-right", 2,
                               "PRESENT_RIGHT", "focus.right.choice"));
    require(display.lesson_focus_calls.back() == "focus.right.choice" &&
                App().robot_uart_.calls.size() == 9 &&
                App().robot_uart_.calls[6] == "head_percent:75" &&
                App().robot_uart_.calls[7] == "left_percent:0" &&
                App().robot_uart_.calls[8] == "right_percent:45",
            "journey 16 keeps right focus and motion aligned after safe supersession");
    CompleteEmbodiedAction();
    require(display.lesson_focus_calls.back().empty(),
            "terminal completion clears the production focus cue");

    // Journey 17: a lost terminal ACK keeps the action consumed and never replays motion.
    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "lost-ack-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    const std::string lost_ack_action =
        EmbodiedActionFrame(sequence + 1, "lost-ack-step", "journey-17", 1);
    Handle(lost_ack_action);
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    HostEspFireTimer();
    App().protocol_.reset();
    App().DrainLessonEmbodiedQueue();
    const auto calls_after_lost_ack = App().robot_uart_.calls;
    App().protocol_ = std::make_unique<Protocol>();
    Handle(lost_ack_action);
    require(App().robot_uart_.calls == calls_after_lost_ack && Sent().empty(),
            "journey 17 does not replay physical motion when a terminal ACK is lost");
    Handle(EmbodiedCancelFrame(sequence + 2, "lost-ack-step", "journey-17", 1));
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == 1 && FrameBodyNum(0, "acks") == sequence + 2,
            "journey 17 remains recoverable through an exact cancel after ACK loss");

    // Journey 18: opening the child's assessment window cancels motion before listening.
    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "barge-source", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "barge-source", "journey-18", 1));
    const auto barge_stale_callback = HostEspQueueTimerCallback();
    const size_t calls_before_barge = App().robot_uart_.calls.size();
    Handle(StepFrame(sequence + 2, "barge-listen", "http://x/p2.jpg", "http://x/o2.jpg",
                     "http://x/r2.jpg", ",\"completionClass\":\"interactive\",\"prompt\":\"What do you see?\""));
    require(App().robot_uart_.calls.size() == calls_before_barge + 3 &&
                App().prepare_listen_calls == 0,
            "journey 18 keeps assessed listening closed while restore settles");
    const size_t frames_after_barge = Sent().size();
    HostEspInvokeQueuedCallback(barge_stale_callback);
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == frames_after_barge && App().prepare_listen_calls == 0,
            "journey 18 suppresses the cancelled hold callback while restore settles");
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == frames_after_barge + 1 &&
                FrameEmbodiedBool(frames_after_barge, "returnedToRest", false) &&
                App().prepare_listen_calls == 1,
            "journey 18 opens assessed listening once after terminal rest ACK and settle");

    // A lifecycle transition invalidates both the pending assessed-listen request and
    // the restore timer nonce; neither can reopen the mic after pause.
    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "barge-pause-source", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "barge-pause-source", "journey-18-pause", 1));
    Handle(StepFrame(sequence + 2, "barge-pause-listen", "http://x/p2.jpg", "http://x/o2.jpg",
                     "http://x/r2.jpg", ",\"completionClass\":\"interactive\",\"prompt\":\"Your turn\""));
    const auto stale_restore_callback = HostEspQueueTimerCallback();
    Handle(PauseFrame(sequence + 3));
    HostEspInvokeQueuedCallback(stale_restore_callback);
    App().DrainLessonEmbodiedQueue();
    require(App().prepare_listen_calls == 0,
            "journey 18 pause invalidates pending assessed listening and restore nonce");

    // A partial rest command degrades the action but never opens assessed listening.
    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "barge-failed-source", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "barge-failed-source", "journey-18-failed", 1));
    App().robot_uart_.right_ok = false;
    const size_t failed_restore_before = Sent().size();
    Handle(StepFrame(sequence + 2, "barge-failed-listen", "http://x/p2.jpg", "http://x/o2.jpg",
                     "http://x/r2.jpg", ",\"completionClass\":\"interactive\",\"prompt\":\"Your turn\""));
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(Sent().size() == failed_restore_before + 2 &&
                !FrameEmbodiedBool(Sent().size() - 1, "returnedToRest", true) &&
                App().prepare_listen_calls == 0,
            "journey 18 partial restore ACK keeps assessed listening closed");

    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "barge-timer-source", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "barge-timer-source", "journey-18-timer", 1));
    HostEspTimerStartOk() = false;
    const size_t failed_timer_before = Sent().size();
    Handle(StepFrame(sequence + 2, "barge-timer-listen", "http://x/p2.jpg", "http://x/o2.jpg",
                     "http://x/r2.jpg", ",\"completionClass\":\"interactive\",\"prompt\":\"Your turn\""));
    require(Sent().size() == failed_timer_before + 2 &&
                FrameBodyStr(Sent().size() - 1, "embodiedAction", "outcome") == "rejected" &&
                !FrameEmbodiedBool(Sent().size() - 1, "returnedToRest", true) &&
                App().prepare_listen_calls == 0,
            "journey 18 restore timer failure rejects and keeps assessed listening closed");
    HostEspTimerStartOk() = true;

    // Journey 19: emotional sharing uses the calm supportive face and bounded pose.
    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "calm-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "calm-step", "journey-19", 1,
                               "COMFORT_CALM", "focus.center.primary"));
    require(display.last_emotion == "relaxed" &&
                display.lesson_focus_calls.back() == "focus.center.primary" &&
                App().robot_uart_.calls ==
                    std::vector<std::string>({"head_percent:50", "left_percent:10", "right_percent:10"}),
            "journey 19 applies the calm supportive face and bounded pose");
    CompleteEmbodiedAction();

    // Journey 20: reduced motion completes with face/focus only and a settled degraded ACK.
    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "reduced-journey", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    tbot::SetLessonCourseModeCapabilityForTest(true, true);
    const size_t reduced_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "reduced-journey", "journey-20", 1,
                               "LISTEN_STILL", "focus.center.primary"));
    require(App().robot_uart_.calls.empty() && display.last_emotion == "relaxed",
            "journey 20 reduced motion uses face and focus without servo commands");
    require(display.lesson_focus_calls.back() == "focus.center.primary",
            "journey 20 reduced motion still applies the authored production focus cue");
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(FrameBodyStr(reduced_before, "embodiedAction", "outcome") == "degraded" &&
                FrameEmbodiedBool(reduced_before, "returnedToRest", false),
            "journey 20 completes with a settled degraded ACK");
    tbot::SetLessonCourseModeCapabilityForTest(true, false);
    Board::GetInstance().display_ = nullptr;
}

void test_embodied_lifecycle_failure_and_teardown_paths() {
    ResetObservable();
    FreshSession();
    Handle(EmbodiedActionFrame(1, "not-running", "outside-session", 1));
    require(Sent().empty() && App().robot_uart_.calls.empty(),
            "embodied action outside a running lesson is ignored");

    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    int sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "assessment-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"interactive\",\"prompt\":\"Your turn\""));
    const size_t assessment_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "assessment-step", "assessment-open", 1));
    require(Sent().size() == assessment_before + 1 &&
                FrameBodyStr(assessment_before, "embodiedAction", "outcome") == "rejected" &&
                App().robot_uart_.calls.empty(),
            "assessment-open action is rejected without physical motion");

    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "no-face-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Board::GetInstance().display_ = nullptr;
    const size_t no_face_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "no-face-step", "no-face", 1));
    CompleteEmbodiedAction();
    require(FrameBodyStr(no_face_before, "embodiedAction", "outcome") == "degraded",
            "missing face surface degrades an otherwise responsive servo action");

    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "hold-failure", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    const size_t hold_failure_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "hold-failure", "hold-failure", 1));
    App().robot_uart_.left_ok = false;
    HostEspTimerStartOk() = false;
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(FrameBodyStr(hold_failure_before, "embodiedAction", "outcome") == "rejected" &&
                !FrameEmbodiedBool(hold_failure_before, "returnedToRest", true),
            "restore plus settle-timer failure rejects without a false rest claim");
    HostEspTimerStartOk() = true;

    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "cancel-failure", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    const size_t cancel_failure_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "cancel-failure", "cancel-failure", 1));
    App().robot_uart_.right_ok = false;
    HostEspTimerStartOk() = false;
    Handle(EmbodiedCancelFrame(sequence + 2, "cancel-failure", "cancel-failure", 1));
    require(FrameBodyStr(cancel_failure_before, "embodiedAction", "outcome") == "rejected" &&
                FrameBodyNum(cancel_failure_before, "acks") == sequence + 2,
            "cancel restoration/timer failure emits one allowed rejected terminal ACK");
    HostEspTimerStartOk() = true;

    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "teardown-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "teardown-step", "pause-action", 1));
    const size_t pause_calls = App().robot_uart_.calls.size();
    Handle(PauseFrame(sequence + 2));
    require(App().robot_uart_.calls.size() == pause_calls + 3,
            "pause teardown restores the active pose");
    Handle(ResumeFrame(sequence + 3));
    Handle(EmbodiedActionFrame(sequence + 4, "teardown-step", "stop-action", 2));
    const size_t stop_calls = App().robot_uart_.calls.size();
    Handle(StopFrame(sequence + 5));
    require(App().robot_uart_.calls.size() == stop_calls + 3 && !App().lesson_runtime_active,
            "stop teardown restores rest and closes lesson authority");
    Board::GetInstance().display_ = nullptr;
}

void test_embodied_ledger_mastery_cap_and_no_return_settle() {
    ResetObservable();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    int sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "ledger-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    Handle(EmbodiedActionFrame(sequence + 1, "ledger-step", "ledger-action", 1));
    CompleteEmbodiedAction();

    Handle(PrepareFrame(sequence + 2,
                        ",\"runtimeControls\":{\"motionPresetsEnabled\":true}"));
    Handle(StartFrame(sequence + 3));
    Handle(StepFrame(sequence + 4, "ledger-step-2", "http://x/p2.jpg", "http://x/o2.jpg",
                     "http://x/r2.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    const size_t stale_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 5, "ledger-step-2", "ledger-stale", 1));
    require(Sent().size() == stale_before + 1 &&
                FrameBodyStr(stale_before, "embodiedAction", "outcome") == "rejected" &&
                App().robot_uart_.calls.size() == 6,
            "same-session restart restores the action-generation ledger without replaying motion");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "mastery-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    for (int generation = 1; generation <= 2; ++generation) {
        Handle(EmbodiedActionFrame(sequence + generation, "mastery-step",
                                   "mastery-" + std::to_string(generation), generation,
                                   "CELEBRATE_MASTERY", "focus.center.primary"));
        CompleteEmbodiedAction();
    }
    const size_t mastery_calls = App().robot_uart_.calls.size();
    const size_t capped_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 3, "mastery-step", "mastery-3", 3,
                               "CELEBRATE_MASTERY", "focus.center.primary"));
    require(App().robot_uart_.calls.size() == mastery_calls,
            "third high-energy mastery celebration is reduced instead of moving servos");
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(FrameBodyStr(capped_before, "embodiedAction", "outcome") == "degraded" &&
                FrameEmbodiedBool(capped_before, "returnedToRest", false),
            "mastery energy cap completes safely as an already-rested degraded action");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "rest-warm-step", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    const size_t rest_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "rest-warm-step", "rest-warm", 1,
                               "REST_WARM", "focus.center.primary"));
    require(App().robot_uart_.calls ==
                std::vector<std::string>({"head_percent:50", "left_percent:0", "right_percent:0"}),
            "REST_WARM resolves directly to the bounded rest pose");
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(FrameBodyStr(rest_before, "embodiedAction", "outcome") == "applied" &&
                FrameEmbodiedBool(rest_before, "returnedToRest", false),
            "no-return REST_WARM completes after settle without a redundant restore command");

    ResetObservable();
    Board::GetInstance().display_ = &display;
    sequence = OpenMotionEnabledSession();
    Handle(StepFrame(sequence, "listen-still-partial", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"completionClass\":\"passive\",\"prompt\":\"Ready\""));
    App().robot_uart_.left_ok = false;
    const size_t partial_rest_before = Sent().size();
    Handle(EmbodiedActionFrame(sequence + 1, "listen-still-partial", "listen-still-partial", 1,
                               "LISTEN_STILL", "focus.center.primary"));
    HostEspFireTimer();
    App().DrainLessonEmbodiedQueue();
    require(FrameBodyStr(partial_rest_before, "embodiedAction", "outcome") == "degraded" &&
                !FrameEmbodiedBool(partial_rest_before, "returnedToRest", true),
            "partial no-return rest preset never claims confirmed rest");
    Board::GetInstance().display_ = nullptr;
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

void test_renderer_v2_capability_shape_and_exact_tokens() {
    require(std::string(kLessonRendererV1) == "teebot-lesson-renderer.v1",
            "renderer v1 token remains exact");
    require(std::string(kLessonRendererV2) == "teebot-lesson-renderer.v2",
            "renderer v2 token is exact");
    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "lesson", true);
    cJSON_AddStringToObject(features, "renderer", kLessonRendererName);
    AddLessonRendererFeatures(features);
    char* encoded = cJSON_PrintUnformatted(features);
    require(encoded != nullptr, "renderer capability serializes");
    require(std::string(encoded) ==
                "{\"lesson\":true,\"renderer\":[\"teebot-lesson-renderer.v1\","
                "\"teebot-lesson-renderer.v2\"],\"lessonRendererV2\":{"
                "\"openingEntrance\":true,\"visualStateEvents\":true,"
                "\"physicalMotionOwner\":\"server\",\"singleSpriteEntrance\":true},"
                "\"lessonCourseMode\":{\"version\":2,\"embodiedActions\":true,"
                "\"reducedMotion\":false,\"faces\":[\"neutral\",\"happy\","
                "\"thinking\",\"relaxed\"]}}",
            "hello advertises both renderer tokens and structured v2 ownership");
    cJSON_free(encoded);
    cJSON_Delete(features);
}

void test_renderer_v3_capability_is_fail_closed_until_initialized() {
    tbot::SetLessonCinematicRendererCapabilityReady(false);
    cJSON* features = cJSON_CreateObject();
    AddLessonRendererFeatures(features);
    char* encoded = cJSON_PrintUnformatted(features);
    require(encoded != nullptr && std::string(encoded).find("teebot-lesson-renderer.v3") ==
                                      std::string::npos,
            "renderer v3 is absent before display/PSRAM/JPEG initialization succeeds");
    cJSON_free(encoded);
    cJSON_Delete(features);

    tbot::SetLessonCinematicRendererCapabilityReady(true);
    features = cJSON_CreateObject();
    AddLessonRendererFeatures(features);
    encoded = cJSON_PrintUnformatted(features);
    require(encoded != nullptr && std::string(encoded) ==
                "{\"renderer\":[\"teebot-lesson-renderer.v1\",\"teebot-lesson-renderer.v2\","
                "\"teebot-lesson-renderer.v3\"],\"lessonRendererV2\":{"
                "\"openingEntrance\":true,\"visualStateEvents\":true,"
                "\"physicalMotionOwner\":\"server\",\"singleSpriteEntrance\":true},"
                "\"lessonRendererV3\":{\"directMp4Cinematic\":true,\"sdAssetPack\":true},"
                "\"lessonCourseMode\":{\"version\":2,\"embodiedActions\":true,"
                "\"reducedMotion\":false,\"faces\":[\"neutral\",\"happy\","
                "\"thinking\",\"relaxed\"]}}",
            "initialized renderer advertises exact Task-7 v3 capability booleans");
    cJSON_free(encoded);
    cJSON_Delete(features);
    tbot::SetLessonCinematicRendererCapabilityReady(false);
}

struct V3RendererFake {
    int allocations = 0;
    int frees = 0;
    int opens = 0;
    int closes = 0;
    int presents = 0;
    std::vector<std::string> opened_paths;
    bool fail_allocate = false;
    bool fail_open = false;
    bool fail_decode = false;
    bool fail_present = false;
    tbot::LessonCinematicError operation_error = tbot::LessonCinematicError::kNone;
    std::uint64_t monotonic_ms = 0;
    std::uint64_t monotonic_step_ms = 0;
    bool trgb_open = false;
    int jpeg_decodes = 0;
    int png_decodes = 0;
    int video_decodes = 0;
    std::vector<std::size_t> video_indices;
    std::uint16_t video_width = 2;
    std::uint16_t video_height = 2;
    std::uint32_t video_frame_count = 3;
    std::uint32_t video_duration_ms = 300;
    std::string expected_claim_session;
    std::string expected_claim_delivery;
    bool observed_claim_before_decode = false;
};

void* V3Allocate(void* context, std::size_t size) {
    if (static_cast<V3RendererFake*>(context)->fail_allocate) return nullptr;
    void* allocation = std::malloc(size);
    if (allocation != nullptr) ++static_cast<V3RendererFake*>(context)->allocations;
    return allocation;
}
void V3Free(void* context, void* pointer) {
    if (pointer != nullptr) ++static_cast<V3RendererFake*>(context)->frees;
    std::free(pointer);
}
bool V3Open(void* context, const char* path, tbot::LessonCinematicStreamMetadata* metadata,
            void** handle) {
    auto* fake = static_cast<V3RendererFake*>(context);
    if (fake->fail_open) return false;
    fake->opened_paths.emplace_back(path);
    fake->trgb_open = std::string(path).find(".trgb") != std::string::npos;
    const bool background = std::string(path).find("background") != std::string::npos ||
        std::string(path).find("flattenedCinematic") != std::string::npos;
    const std::string opened(path);
    const bool course_mode_robot =
        opened.find("75000000-0000-4000-8000-000000000031") != std::string::npos ||
        opened.find("85000000-0000-4000-8000-000000000031") != std::string::npos;
    fake->video_width = opened.find("robot-teach") != std::string::npos || course_mode_robot ? 240 : 2;
    fake->video_height = opened.find("robot-teach") != std::string::npos || course_mode_robot ? 240 : 2;
    std::uint32_t duration_ms = 300;
    if (opened.find("-opening") != std::string::npos) duration_ms = 9500;
    else if (opened.find("robot-teach") != std::string::npos || course_mode_robot) duration_ms = 3000;
    else if (opened.find("cat-discover") != std::string::npos) duration_ms = 2000;
    else if (opened.find("-greet") != std::string::npos) duration_ms = 1200;
    else if (opened.find("-teach") != std::string::npos) duration_ms = 2600;
    else if (opened.find("-listen") != std::string::npos ||
             opened.find("-thinking") != std::string::npos) duration_ms = 1300;
    else if (opened.find("-correct") != std::string::npos) duration_ms = 600;
    else if (opened.find("-retry-level-1") != std::string::npos) duration_ms = 1200;
    else if (opened.find("-retry-level-2") != std::string::npos) duration_ms = 1400;
    else if (opened.find("-retry-level-3") != std::string::npos) duration_ms = 1600;
    else if (opened.find("-celebrate") != std::string::npos) duration_ms = 3000;
    else if (opened.find("-word-transition") != std::string::npos) duration_ms = 1100;
    fake->video_duration_ms = duration_ms;
    fake->video_frame_count = duration_ms / 100;
    *metadata = {static_cast<std::uint16_t>(fake->trgb_open ? 320 : background ? 480 : 2),
                 static_cast<std::uint16_t>(fake->trgb_open ? 480 : background ? 320 : fake->video_height), 10,
                 fake->video_frame_count, duration_ms,
                 static_cast<std::uint32_t>(fake->trgb_open ? 307200 : 64)};
    if (!fake->trgb_open && !background) {
        metadata->width = fake->video_width;
    }
    *handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(++fake->opens));
    return true;
}
void V3Close(void* context, void*) { ++static_cast<V3RendererFake*>(context)->closes; }
bool V3Decode(void* context, void*, std::size_t index, std::uint8_t* destination,
              std::size_t capacity, std::uint16_t* width, std::uint16_t* height,
              std::size_t* stride) {
    auto* fake = static_cast<V3RendererFake*>(context);
    if (!fake->expected_claim_delivery.empty()) {
        fake->observed_claim_before_decode = tbot::LessonCourseDeliveryAppliedForTest(
            fake->expected_claim_session.c_str(), fake->expected_claim_delivery.c_str());
    }
    if (fake->fail_decode) return false;
    ++fake->video_decodes;
    fake->video_indices.push_back(index);
    const bool trgb = fake->trgb_open;
    const bool background = capacity >= 480u * 320u * 2u;
    *width = trgb ? 320 : background ? 480 : fake->video_width;
    *height = trgb ? 480 : background ? 320 : fake->video_height;
    *stride = static_cast<std::size_t>(*width) * 2;
    std::memset(destination, 0, *stride * *height);
    return true;
}
bool V5DecodeJpeg(void* context, const char*, std::uint16_t* destination,
                  std::size_t capacity, std::uint16_t* width,
                  std::uint16_t* height, std::size_t* stride) {
    auto* fake = static_cast<V3RendererFake*>(context);
    if (fake->fail_decode || capacity < 480u * 320u) return false;
    ++fake->jpeg_decodes;
    *width = 480;
    *height = 320;
    *stride = 480;
    std::fill(destination, destination + 480u * 320u, 0x1234);
    return true;
}
bool V5DecodePng(void* context, const char*, std::uint8_t* destination,
                 std::size_t capacity, std::uint16_t* width,
                 std::uint16_t* height, std::size_t* stride) {
    auto* fake = static_cast<V3RendererFake*>(context);
    if (fake->fail_decode || capacity < 16) return false;
    ++fake->png_decodes;
    *width = 2;
    *height = 2;
    *stride = 8;
    std::fill(destination, destination + 16, 0xff);
    return true;
}
bool V4Begin(void*) { return true; }
bool V4Queue(void*, const std::uint16_t*, std::uint16_t width, std::uint16_t height) {
    return width == 320 && height == 480;
}
bool V4Wait(void*, std::uint32_t) { return true; }
void V4End(void*) {}
bool V4StreamBytes(void* context, void*, std::uint64_t* bytes) {
    *bytes = static_cast<V3RendererFake*>(context)->trgb_open ? 29186048 : 0;
    return true;
}
bool V3Present(void* context, const std::uint16_t*, std::uint16_t, std::uint16_t,
               std::size_t);

void test_renderer_v4_trgb_exact_asset_contract() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        tbot::LessonFlattenedCinematicRendererOps{
            {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present},
            V4Begin, V4Queue, V4Wait, V4End, nullptr, V4StreamBytes});
    tbot::SetActiveLessonFlattenedCinematicRenderer(&renderer);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);

    Handle(V4TrgbPrepareFrame(1));
    require(FrameType(0) == "lesson_ack" && fake.opens == 1 && fake.allocations == 2,
            "handler accepts the exact TRGB contract and routes it to the renderer");
    Handle(V4V2BarnCueCommandFrame("lesson_cinematic_control", "cancel", 2, 272,
                                   ",\"reason\":\"testCleanup\""));

    const std::vector<std::pair<std::string, std::string>> invalid = {
        {"\"containerVersion\":1,", ""},
        {"\"containerVersion\":1", "\"containerVersion\":2"},
        {"\"storedWidth\":320", "\"storedWidth\":480"},
        {"\"storedHeight\":480", "\"storedHeight\":320"},
        {"\"orientation\":\"panelNativeClockwise\"", "\"orientation\":\"landscape\""},
        {"\"frameBytes\":307200", "\"frameBytes\":1"},
        {"\"durationMs\":9500", "\"durationMs\":9501"},
    };
    int sequence = 3;
    for (const auto& replacement : invalid) {
        ResetObservable();
        FreshSession();
        const int opens_before = fake.opens;
        const int frame_sequence = sequence++;
        Handle(ReplaceOnce(V4TrgbPrepareFrame(frame_sequence, 300 + frame_sequence),
                           replacement.first, replacement.second));
        require(FrameType(0) == "lesson_error" && fake.opens == opens_before,
                "handler rejects missing/wrong TRGB metadata before renderer work");
    }
    ResetObservable();
    FreshSession();
    const int opens_before_extra = fake.opens;
    Handle(V4TrgbPrepareFrame(20, 420, ",\"width\":480"));
    require(FrameType(0) == "lesson_error" && fake.opens == opens_before_extra,
            "TRGB exact-key validation rejects legacy dimensions");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}
bool V3Present(void* context, const std::uint16_t*, std::uint16_t, std::uint16_t,
               std::size_t) {
    auto* fake = static_cast<V3RendererFake*>(context);
    ++fake->presents;
    return !fake->fail_present;
}
tbot::LessonCinematicError V3LastError(void* context) {
    return static_cast<V3RendererFake*>(context)->operation_error;
}
std::uint64_t V3MonotonicMs(void* context) {
    auto* fake = static_cast<V3RendererFake*>(context);
    const std::uint64_t now = fake->monotonic_ms;
    fake->monotonic_ms += fake->monotonic_step_ms;
    return now;
}

void test_cinematic_rejects_unsupported_frames_and_accepts_all_late_phases() {
    ResetObservable();
    FreshSession();
    Handle(V3Frame("lesson_visual_state", 1, "{}"));
    require(Sent().size() == 1 && FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_COMMAND_UNSUPPORTED" &&
                FrameBodyStr(0, "context", "reason") == "command",
            "cinematic renderer rejects unsupported frame types with the stable command error");

    ResetObservable();
    FreshSession();
    Handle(V4Frame("lesson_step", 1, "{}"));
    require(Sent().size() == 1 && FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_COMMAND_UNSUPPORTED" &&
                FrameBodyStr(0, "context", "reason") == "command",
            "renderer-v4 rejects legacy lesson_step with the stable command error");

    V3RendererFake fake;
    tbot::LessonCinematicRenderer renderer({&fake, V3Allocate, V3Free, V3Open, V3Close,
                                             V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&renderer);
    int sequence = 1;
    std::uint64_t command_sequence = 101;
    for (const char* phase : {"thinking", "correct", "retry", "celebrate"}) {
        ResetObservable();
        FreshSession();
        const std::string frame = ReplaceOnce(
            V3PrepareFrame(sequence++, command_sequence++),
            "\"phaseId\":\"opening\"", std::string("\"phaseId\":\"") + phase + "\"");
        Handle(frame);
        require(Sent().size() == 1 && FrameType(0) == "lesson_ack" &&
                    FrameBodyStr(0, "cinematicPhase", "phaseId") == phase &&
                    FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady",
                "late cinematic phase alternatives are accepted and echoed in the typed ACK");
    }
    Handle(V3Frame("lesson_cinematic_control", sequence,
        std::string("{\"command\":\"cancel\",\"phaseId\":\"celebrate\","
                    "\"commandSequenceId\":") + std::to_string(command_sequence) + "}"));
    require(FrameType(Sent().size() - 1) == "lesson_ack",
            "late-phase cinematic fixture releases its active handler session");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_renderer_v3_routes_lesson_step_after_cinematic_start() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    NetworkInterface network;
    Board::GetInstance().display_ = &display;
    Board::GetInstance().network_ = &network;
    ResetHostHttp();
    HostHttp().body = JpegBody();
    HostHttp().use_content_length = true;
    HostJpegDecodeMode() = 0;

    V3RendererFake fake;
    tbot::LessonCinematicRenderer renderer({&fake, V3Allocate, V3Free, V3Open, V3Close,
                                             V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&renderer);
    Handle(V3PrepareFrame(1));
    Handle(V3Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":42}}"));

    std::string step = StepFrame(
        3, "s1", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
        ",\"prompt\":\"Welcome\",\"stepType\":\"greeting\","
        "\"completionClass\":\"passive\"");
    const std::string legacy_version = kLessonProtocolVersion;
    const std::string renderer_v3 = tbot::kLessonRendererV3;
    const std::size_t version = step.find(legacy_version);
    require(version != std::string::npos, "step fixture contains its protocol version");
    step.replace(version, legacy_version.size(), renderer_v3);
    Handle(step);

    require(Sent().size() == 3 && FrameType(2) == "lesson_ack" &&
                FrameStepId(2) == "s1" && FrameBodyNum(2, "acks") == 3,
            "renderer-v3 lesson_step reaches the standard step ACK path after cinematic start");
    require(FrameBodyBool(2, "rendered", false) && !FrameBodyBool(2, "degraded", true),
            "renderer-v3 lesson_step renders all authored layers without degradation");
    Handle(V3Frame("lesson_stop", 4, "{\"reason\":\"completed\"}"));
    require(FrameType(3) == "lesson_ack" && FrameBodyNum(3, "acks") == 4,
            "renderer-v3 terminal lesson_stop reaches the standard completion ACK path");
    require(!renderer.prepared() && !LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "renderer-v3 terminal lesson_stop releases its cinematic session and asset lease");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_cinematic_renderer_failures_use_stable_error_mapping() {
    enum class FailureMode { kOpen, kAllocate, kDecode, kTimeout, kPresent };
    struct FailureCase {
        FailureMode mode;
        tbot::LessonCinematicError operation_error;
        const char* expected_code;
    };
    const FailureCase cases[] = {
        {FailureMode::kOpen, tbot::LessonCinematicError::kParserFailed,
         "CINEMATIC_PARSER_FAILED"},
        {FailureMode::kDecode, tbot::LessonCinematicError::kFileRead,
         "CINEMATIC_FILE_READ_FAILED"},
        {FailureMode::kOpen, tbot::LessonCinematicError::kSessionMismatch,
         "CINEMATIC_SESSION_MISMATCH"},
        {FailureMode::kAllocate, tbot::LessonCinematicError::kNone,
         "CINEMATIC_INSUFFICIENT_PSRAM"},
        {FailureMode::kDecode, tbot::LessonCinematicError::kNone,
         "CINEMATIC_DECODE_FAILED"},
        {FailureMode::kTimeout, tbot::LessonCinematicError::kNone,
         "CINEMATIC_DECODE_TIMEOUT"},
        {FailureMode::kPresent, tbot::LessonCinematicError::kNone,
         "CINEMATIC_PRESENT_FAILED"},
        {FailureMode::kOpen, static_cast<tbot::LessonCinematicError>(0xff),
         "CINEMATIC_METADATA_MISMATCH"},
    };

    int sequence = 1;
    std::uint64_t command_sequence = 201;
    for (const auto& failure : cases) {
        ResetObservable();
        FreshSession();
        V3RendererFake fake;
        fake.operation_error = failure.operation_error;
        fake.fail_open = failure.mode == FailureMode::kOpen;
        fake.fail_allocate = failure.mode == FailureMode::kAllocate;
        fake.fail_decode = failure.mode == FailureMode::kDecode;
        fake.fail_present = failure.mode == FailureMode::kPresent;
        fake.monotonic_step_ms = failure.mode == FailureMode::kTimeout ? 501 : 0;
        tbot::LessonFlattenedCinematicRenderer renderer(
            {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present,
             V3LastError, failure.mode == FailureMode::kTimeout ? V3MonotonicMs : nullptr});
        tbot::SetActiveLessonFlattenedCinematicRenderer(&renderer);
        tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);

        Handle(V4PrepareFrame(sequence++, command_sequence++));
        require(Sent().size() == 1 && FrameType(0) == "lesson_error" &&
                    FrameBodyStr(0, nullptr, "code") == failure.expected_code &&
                    FrameBodyStr(0, "context", "reason") == "cinematicPhase",
                "cinematic renderer failure maps to its stable outbound error code");
        require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession() &&
                    fake.allocations == fake.frees && fake.opens == fake.closes,
                "rejected v4 prepare releases its newly acquired lease and renderer resources");
        tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
    }
}

void test_cinematic_prepare_reservation_refusal_and_v3_rejection_cleanup() {
    ResetObservable();
    FreshSession();
    V3RendererFake v4_fake;
    tbot::LessonFlattenedCinematicRenderer v4_renderer(
        {&v4_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonFlattenedCinematicRenderer(&v4_renderer);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "v4 reservation-refusal fixture holds mutation lease");
        Handle(V4PrepareFrame(1));
        require(Sent().size() == 1 && FrameType(0) == "lesson_error" &&
                    FrameBodyStr(0, nullptr, "code") == "CINEMATIC_SD_PATH_MISSING" &&
                    v4_fake.allocations == 0 && v4_fake.opens == 0,
                "v4 reservation refusal emits the stable path error before renderer work");
    }
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);

    ResetObservable();
    FreshSession();
    V3RendererFake v3_fake;
    tbot::LessonCinematicRenderer v3_renderer(
        {&v3_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&v3_renderer);
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "v3 reservation-refusal fixture holds mutation lease");
        Handle(V3PrepareFrame(1));
        require(Sent().size() == 1 && FrameType(0) == "lesson_error" &&
                    FrameBodyStr(0, nullptr, "code") == "CINEMATIC_SD_PATH_MISSING" &&
                    v3_fake.allocations == 0 && v3_fake.opens == 0,
                "v3 reservation refusal emits the stable path error before renderer work");
    }

    ResetObservable();
    FreshSession();
    v3_fake.fail_open = true;
    Handle(V3PrepareFrame(2, 302));
    require(Sent().size() == 1 && FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_SD_PATH_MISSING" &&
                !LessonAssetStorageCoordinator::GetInstance().HasLessonSession() &&
                v3_fake.allocations == v3_fake.frees,
            "rejected v3 renderer prepare releases its new reservation and allocations");

    ResetObservable();
    FreshSession();
    v3_fake.fail_open = false;
    std::string invalid_chroma = ReplaceOnce(
        V3PrepareFrame(3, 303), "\"keyColor\":\"#00ff00\"",
        "\"keyColor\":\"#zzzzzz\"");
    Handle(invalid_chroma);
    require(Sent().size() == 1 && FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                v3_fake.opens == 0,
            "v3 chroma sscanf failure rejects metadata before renderer work");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void ActivateV4Renderer(tbot::LessonFlattenedCinematicRenderer* renderer) {
    tbot::SetActiveLessonFlattenedCinematicRenderer(renderer);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(renderer != nullptr &&
                                                              renderer->initialized());
}

void test_renderer_v5_capability_exact_layers_and_lifecycle() {
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    cJSON* features = cJSON_CreateObject();
    AddLessonRendererFeatures(features);
    char* encoded = cJSON_PrintUnformatted(features);
    require(encoded != nullptr && std::string(encoded).find("teebot-lesson-renderer.v5") ==
                                      std::string::npos,
            "renderer v5 is absent until the layered renderer is initialized");
    cJSON_free(encoded);
    cJSON_Delete(features);

    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    features = cJSON_CreateObject();
    AddLessonRendererFeatures(features);
    encoded = cJSON_PrintUnformatted(features);
    require(encoded != nullptr &&
                std::string(encoded).find("teebot-lesson-renderer.v5") != std::string::npos &&
                std::string(encoded).find("\"lessonRendererV5\":{\"layeredCinematic\":true,"
                                          "\"sdAssetPack\":true}") != std::string::npos,
            "initialized renderer advertises the exact v5 capability shape");
    cJSON_free(encoded);
    cJSON_Delete(features);

    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V5PrepareFrame(1));
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady" &&
                FrameBodyStr(0, "cinematicPhase", "phaseId") == "flyIn",
            "v5 prepare returns the typed frame-zero ACK for the requested effect");
    require(fake.jpeg_decodes == 1 && fake.png_decodes == 1 &&
                fake.video_decodes == 1 && fake.opens == 1 && fake.presents == 1,
            "v5 prepare decodes both static images once and only frame zero from Robot video");

    Handle(V5Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"flyIn\","
        "\"commandSequenceId\":92}}"));
    require(FrameType(1) == "lesson_ack" &&
                FrameBodyStr(1, "cinematicPhase", "event") == "phaseReady",
            "v5 start routes to the layered renderer");
    Handle(V5Frame("lesson_cinematic_control", 3,
        "{\"command\":\"pause\",\"phaseId\":\"flyIn\",\"commandSequenceId\":93}"));
    require(FrameType(2) == "lesson_ack", "v5 pause routes to the layered renderer");
    Handle(V5Frame("lesson_cinematic_control", 4,
        "{\"command\":\"resume\",\"phaseId\":\"flyIn\",\"commandSequenceId\":94,"
        "\"clockRebaseSequenceId\":94}"));
    require(FrameType(3) == "lesson_ack", "v5 resume routes to the layered renderer");
    Handle(V5Frame("lesson_stop", 5,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"flyIn\","
        "\"commandSequenceId\":95}}"));
    require(FrameType(4) == "lesson_ack" && FrameSeq(4) == 5 &&
                fake.closes == 1 && fake.frees == 4,
            "v5 stop releases the Robot stream and all four bounded buffers");
    require(App().lesson_terminal_audio_quiet,
            "v5 stop quarantines late TTS before releasing lesson mode");
    require(!display.lesson_mode_calls.empty() && !display.lesson_mode_calls.back() &&
                !display.background_calls.empty() && !display.background_calls.back(),
            "v5 stop releases the cinematic display surface and layers");

    ResetObservable();
    FreshSession();
    const int opens_before = fake.opens;
    Handle(ReplaceOnce(V5PrepareFrame(6, 96),
                       "\"mediaKind\":\"video\",\"mediaType\":\"video/mp4\"",
                       "\"mediaKind\":\"image\",\"mediaType\":\"video/mp4\""));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.opens == opens_before,
            "v5 rejects a still-image Robot layer before renderer IO");
    Board::GetInstance().display_ = nullptr;

    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
}

void test_renderer_v5_course_mode_activity_fallback_without_object() {
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    Handle(V5CourseModeActivityFallbackPrepareFrame(1));

    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady",
            "activity-aware Course Mode fallback prepares without a teaching object");
    require(fake.jpeg_decodes == 1 && fake.png_decodes == 0 && fake.video_decodes == 1,
            "fallback keeps background and Robot static state without object decoding");
    Handle(V5Frame("lesson_stop", 2,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"listen\","
        "\"commandSequenceId\":192}}"));
    require(FrameType(1) == "lesson_ack" &&
                !LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "activity-aware fallback stop releases its lesson asset reservation");
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
}

void test_renderer_v5_first_course_prepare_reports_static_degradation() {
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    fake.fail_open = true;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);

    Handle(V5CourseModeActivityFallbackPrepareFrame(1));

    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "degradedReason") ==
                    "animationStartFailed" && renderer.prepared(),
            "first Course Mode Robot open failure ACKs retained static degradation");
    Handle(V5Frame("lesson_stop", 2,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"listen\","
        "\"commandSequenceId\":192}}"));
    require(FrameBodyStr(1, "cinematicPhase", "degradedReason").empty(),
            "successful stop ACK does not inherit prepare degradation");
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
}

void test_course_activity_durable_outcome_replay_and_write_failure() {
    tbot::SetLessonCourseDeliveryStorageForTest("");
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    Handle(V5CourseModeActivityFallbackPrepareFrame(1));

    fake.fail_decode = true;
    Handle(CourseActivityFrame(2, "degraded-lost-ack", "activity-degraded", "teach", true));
    require(FrameBodyBool(1, "degraded", false), "first degraded activity ACK is recorded");
    const int degraded_decodes = fake.video_decodes;
    fake.fail_decode = false;
    Handle(CourseActivityFrame(3, "degraded-lost-ack", "activity-degraded", "teach", true));
    require(fake.video_decodes == degraded_decodes && FrameBodyBool(2, "degraded", false) &&
                FrameBodyStr(2, nullptr, "degradedReason") == "animationStartFailed",
            "duplicate replays exact stored degraded outcome without reapplying");

    tbot::SetLessonCourseDeliveryWriteFailureForTest(true);
    const int before_write_failure = fake.video_decodes;
    Handle(CourseActivityFrame(4, "write-failure", "activity-write", "teach", true));
    require(FrameType(3) == "lesson_error" &&
                FrameBodyStr(3, nullptr, "code") == "COURSE_ACTIVITY_DEDUPE_UNAVAILABLE" &&
                fake.video_decodes == before_write_failure,
            "durable reservation write failure emits retryable error before side effect");
    tbot::SetLessonCourseDeliveryWriteFailureForTest(false);

    const std::string pending_storage = std::string(SID()) + "\tpending-after-reboot\tpending\n";
    tbot::SetLessonCourseDeliveryStorageForTest(pending_storage.c_str());
    const int before_pending = fake.video_decodes;
    Handle(CourseActivityFrame(5, "pending-after-reboot", "activity-pending", "teach", true));
    require(FrameType(4) == "lesson_error" &&
                FrameBodyStr(4, nullptr, "code") == "COURSE_ACTIVITY_DEDUPE_PENDING" &&
                fake.video_decodes == before_pending,
            "unresolved reboot reservation fails closed without replay or rendered success");
    tbot::SetLessonCourseDeliveryStorageForTest("");

    fake.fail_present = true;
    Handle(CourseActivityFrame(6, "present-retry", "activity-present", "listen", false));
    require(FrameType(5) == "lesson_error", "non-applied static present failure is rejected");
    fake.fail_present = false;
    Handle(CourseActivityFrame(7, "present-retry", "activity-present", "listen", false));
    require(FrameType(6) == "lesson_ack" && FrameBodyBool(6, "rendered", false),
            "non-applied present failure removes reservation and permits retry");

    Handle(V5Frame("lesson_stop", 8,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"listen\","
        "\"commandSequenceId\":205}}"));
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
    tbot::SetLessonCourseDeliveryStorageForTest("");
}

void test_course_activity_reconciles_outcome_and_removal_write_failures() {
    tbot::SetLessonCourseDeliveryStorageForTest("");
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    Handle(V5CourseModeActivityFallbackPrepareFrame(1));

    tbot::FailNextLessonCourseDeliveryWriteForTest(2);
    Handle(CourseActivityFrame(2, "outcome-reconcile", "activity-outcome", "teach", true));
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "COURSE_ACTIVITY_DEDUPE_UNAVAILABLE",
            "second outcome write failure is explicit after one applied side effect");
    const int applied_decodes = fake.video_decodes;
    Handle(CourseActivityFrame(3, "outcome-reconcile", "activity-outcome", "teach", true));
    require(fake.video_decodes == applied_decodes && FrameType(2) == "lesson_ack" &&
                !FrameBodyBool(2, "degraded", true),
            "retry durably resolves RAM outcome and ACKs without a second effect");

    fake.fail_present = true;
    tbot::FailNextLessonCourseDeliveryWriteForTest(2);
    Handle(CourseActivityFrame(4, "removal-reconcile", "activity-removal", "listen", false));
    require(FrameType(3) == "lesson_error" &&
                FrameBodyStr(3, nullptr, "code") == "COURSE_ACTIVITY_DEDUPE_UNAVAILABLE",
            "failed durable reservation removal does not advertise a retryable render path");
    const int removal_decodes = fake.video_decodes;
    fake.fail_present = false;
    Handle(CourseActivityFrame(5, "removal-reconcile", "activity-removal", "listen", false));
    require(fake.video_decodes == removal_decodes && FrameType(4) == "lesson_error" &&
                FrameBodyStr(4, nullptr, "code") == "COURSE_ACTIVITY_RENDER_FAILED",
            "retry first reconciles durable removal without applying the effect");
    Handle(CourseActivityFrame(6, "removal-reconcile", "activity-removal", "listen", false));
    require(FrameType(5) == "lesson_ack", "activity retries only after removal is durable");

    Handle(V5Frame("lesson_stop", 7,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"listen\","
        "\"commandSequenceId\":206}}"));
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
    tbot::SetLessonCourseDeliveryStorageForTest("");
}

void test_course_activity_failed_thirteenth_claim_restores_evicted_oldest() {
    FreshSession();
    std::string full;
    for (int index = 0; index < 12; ++index) {
        full += std::string(SID()) + "\twindow-" + std::to_string(index) + "\tapplied\n";
    }
    tbot::SetLessonCourseDeliveryStorageForTest(full.c_str());
    ResetObservable();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    Handle(V5CourseModeActivityFallbackPrepareFrame(1));
    fake.fail_present = true;
    Handle(CourseActivityFrame(2, "window-12", "activity-window", "listen", false));
    fake.fail_present = false;
    require(tbot::LessonCourseDeliveryEntryCountForTest() == 12 &&
                tbot::LessonCourseDeliveryAppliedForTest(SID(), "window-0"),
            "failed thirteenth effect restores the evicted oldest durable record");
    Handle(V5Frame("lesson_stop", 3,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"listen\","
        "\"commandSequenceId\":207}}"));
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
    tbot::SetLessonCourseDeliveryStorageForTest("");
}

void test_course_activity_retains_w19_static_layers_and_dedupes_delivery() {
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    Handle(V5CourseModeActivityFallbackPrepareFrame(1));
    const int initial_video_decodes = fake.video_decodes;

    Handle(CourseActivityFrame(2, "delivery-w19-1"));
    require(fake.jpeg_decodes == 1 && fake.png_decodes == 0 &&
                fake.video_decodes == initial_video_decodes,
            "W19 activity transition retains its two-layer static composition without entrance");
    require(FrameType(1) == "lesson_ack" && FrameBodyBool(1, "rendered", false) &&
                !FrameBodyBool(1, "degraded", true),
            "Course activity emits applied visual status");

    fake.expected_claim_session = SID();
    fake.expected_claim_delivery = "delivery-w19-claim-order";
    Handle(CourseActivityFrame(3, "delivery-w19-claim-order",
                               "w19-weather-order", "teach", true));
    require(fake.observed_claim_before_decode,
            "durable delivery claim is visible before the Robot side effect starts");
    fake.expected_claim_delivery.clear();

    const int after_claim_order = fake.video_decodes;
    Handle(CourseActivityFrame(4, "delivery-w19-1", "w19-weather-recall", "teach", true));
    require(fake.video_decodes == after_claim_order,
            "duplicate session and delivery identity applies no visual effect");

    Handle(CourseActivityFrame(5, "delivery-w19-2", "w19-weather-transfer", "teach", true));
    require(fake.video_decodes == after_claim_order + 1,
            "new delivery explicitly replays entrance exactly once");

    const std::string legacy_activity = ReplaceOnce(
        CourseActivityFrame(6, "legacy-delivery", "w19-weather-legacy", "teach", true),
        "\"deliveryId\":\"legacy-delivery\",", "");
    Handle(legacy_activity);
    const int after_legacy = fake.video_decodes;
    Handle(legacy_activity);
    require(fake.video_decodes == after_legacy,
            "legacy activity without deliveryId remains sequence-idempotent");

    fake.fail_decode = true;
    Handle(CourseActivityFrame(7, "delivery-w19-3", "w19-weather-close", "celebrate", true));
    require(FrameBodyBool(Sent().size() - 1, "rendered", false) &&
                FrameBodyBool(Sent().size() - 1, "degraded", false) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "animationStartFailed",
            "Robot animation failure keeps static composition and emits degraded evidence");

    Handle(V5Frame("lesson_stop", 8,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"celebrate\","
        "\"commandSequenceId\":197}}"));

    fake.fail_decode = false;
    Handle(V5CourseModeActivityFallbackPrepareFrame(1, 198));
    const int reconnect_video_decodes = fake.video_decodes;
    Handle(CourseActivityFrame(2, "delivery-w19-2", "w19-weather-transfer", "teach", true));
    require(fake.video_decodes == reconnect_video_decodes,
            "delivery ledger suppresses an already applied effect after session reconnect");
    Handle(V5Frame("lesson_stop", 3,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"listen\","
        "\"commandSequenceId\":199}}"));

    FreshSession();
    Handle(V5CourseModeActivityFallbackPrepareFrame(1, 200));
    const int different_session_decodes = fake.video_decodes;
    Handle(CourseActivityFrame(2, "delivery-w19-2", "w19-weather-new-session", "teach", true));
    require(fake.video_decodes == different_session_decodes + 1,
            "the same delivery identity in a different session remains a distinct effect");
    Handle(V5Frame("lesson_stop", 3,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"teach\","
        "\"commandSequenceId\":201}}"));
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
}

void test_course_activity_delivery_persistence_reload_bounds_and_corruption() {
    std::string persisted;
    for (int index = 0; index < 15; ++index) {
        persisted += "session-" + std::to_string(index) + "\tdelivery-" +
                     std::to_string(index) + "\n";
    }
    tbot::SetLessonCourseDeliveryStorageForTest(persisted.c_str());
    tbot::ResetLessonCourseDeliveryMemoryForTest();
    require(tbot::LessonCourseDeliveryStorageValidForTest() &&
                tbot::LessonCourseDeliveryEntryCountForTest() == 12,
            "persisted delivery reload evicts oldest entries to the bounded window");
    require(!tbot::LessonCourseDeliveryAppliedForTest("session-0", "delivery-0") &&
                tbot::LessonCourseDeliveryAppliedForTest("session-14", "delivery-14"),
            "reload preserves newest delivery identities and evicts oldest identities");

    tbot::SetLessonCourseDeliveryStorageForTest("session-truncated\tdelivery-truncated");
    tbot::ResetLessonCourseDeliveryMemoryForTest();
    require(!tbot::LessonCourseDeliveryStorageValidForTest() &&
                tbot::LessonCourseDeliveryEntryCountForTest() == 0,
            "truncated persisted ledger fails closed without accepting a partial identity");
    tbot::SetLessonCourseDeliveryStorageForTest("");
    tbot::ResetLessonCourseDeliveryMemoryForTest();
}

void test_course_activity_handler_persistence_round_trip_after_reboot() {
    tbot::SetLessonCourseDeliveryStorageForTest("");
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    Handle(V5CourseModeActivityFallbackPrepareFrame(1));

    for (int index = 0; index < 15; ++index) {
        const std::string delivery = "roundtrip-delivery-" + std::to_string(index);
        const std::string activity = "roundtrip-activity-" + std::to_string(index);
        Handle(CourseActivityFrame(index + 2, delivery.c_str(), activity.c_str()));
    }
    const std::string persisted = tbot::LessonCourseDeliveryStorageForTest();
    require(!persisted.empty(), "handler claims are serialized into durable host storage");

    tbot::ResetLessonCourseDeliveryMemoryForTest();
    require(!tbot::LessonCourseDeliveryAppliedForTest(SID(), "roundtrip-delivery-0") &&
                tbot::LessonCourseDeliveryAppliedForTest(SID(), "roundtrip-delivery-14") &&
                tbot::LessonCourseDeliveryEntryCountForTest() == 12,
            "reboot reloads serializer-produced bytes with the newest bounded window");

    const int before_duplicate = fake.video_decodes;
    Handle(CourseActivityFrame(17, "roundtrip-delivery-14",
                               "roundtrip-activity-14", "teach", true));
    require(fake.video_decodes == before_duplicate,
            "reloaded durable claim suppresses the duplicate handler side effect");
    Handle(CourseActivityFrame(18, "roundtrip-delivery-new",
                               "roundtrip-activity-new", "teach", true));
    require(fake.video_decodes == before_duplicate + 1,
            "a distinct delivery still applies after reboot reload");

    Handle(V5Frame("lesson_stop", 19,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"teach\","
        "\"commandSequenceId\":202}}"));

    FreshSession();
    Handle(V5CourseModeActivityFallbackPrepareFrame(1, 203));
    const int different_session_before = fake.video_decodes;
    Handle(CourseActivityFrame(2, "roundtrip-delivery-14",
                               "roundtrip-other-session", "teach", true));
    require(fake.video_decodes == different_session_before + 1,
            "reloaded delivery identity remains distinct in a different session");
    Handle(V5Frame("lesson_stop", 3,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"teach\","
        "\"commandSequenceId\":204}}"));
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
    tbot::SetLessonCourseDeliveryStorageForTest("");
}

void test_renderer_v5_course_mode_activity_fallback_rejects_manifest_identity_mix() {
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);
    Handle(ReplaceOnce(
        V5CourseModeActivityFallbackPrepareFrame(1),
        "\"manifestChecksum\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"",
        "\"manifestChecksum\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\""));

    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.jpeg_decodes == 0 && fake.video_decodes == 0,
            "Course Mode fallback rejects a marker from a different manifest before IO");
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
}

void test_renderer_v5_dynamic_course_mode_requires_ready_matching_asset_pack() {
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);

    Handle(ReplaceOnce(
        V5CourseModeActivityFallbackPrepareFrame(1),
        "\"state\":\"READY\"", "\"state\":\"PRELOADING\""));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.jpeg_decodes == 0 && fake.video_decodes == 0,
            "dynamic Course Mode rejects a pack without READY checksum attestation");

    ResetObservable();
    FreshSession();
    Handle(ReplaceOnce(
        V5CourseModeActivityFallbackPrepareFrame(2),
        kV5DynamicCourseModeAssetIds[2],
        "85000000-0000-4000-8000-000000000099"));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.jpeg_decodes == 0 && fake.video_decodes == 0,
            "dynamic Course Mode rejects layer and asset-pack identity mismatch");

    ResetObservable();
    FreshSession();
    Handle(ReplaceNth(
        V5CourseModeActivityFallbackPrepareFrame(3),
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"",
        "\"sha256\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\"",
        2));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.jpeg_decodes == 0 && fake.video_decodes == 0,
            "dynamic Course Mode rejects a layer checksum that differs from its READY pack asset");

    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    RemoveV5CourseModeAssetPack();
}

void test_renderer_v5_course_mode_exact_identity_and_fail_closed_metadata() {
    ResetObservable();
    FreshSession();
    StageV5CourseModeAssetPack();
    Board::GetInstance().display_ = nullptr;
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);

    Handle(V5CourseModePrepareFrame(1));
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "phaseId") == "teach" &&
                fake.jpeg_decodes == 1 && fake.png_decodes == 1 &&
                fake.video_indices == std::vector<std::size_t>({0}),
            "exact reviewed v5 Course Mode identity prepares and decodes only Robot frame zero");
    Handle(V5Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"teach\","
        "\"commandSequenceId\":492}}"));
    require(FrameType(1) == "lesson_ack",
            "exact reviewed v5 Course Mode start control is accepted");
    const std::uint64_t start_ms = static_cast<std::uint64_t>(HostEspNowUs() / 1000);
    fake.monotonic_ms = start_ms + 200;
    tbot::TickActiveLessonLayeredCinematicRenderer(start_ms + 200);
    fake.monotonic_ms = start_ms + 300;
    tbot::TickActiveLessonLayeredCinematicRenderer(start_ms + 300);
    require(fake.video_indices.size() >= 2 && fake.video_indices.front() == 0 &&
                std::any_of(fake.video_indices.begin() + 1, fake.video_indices.end(),
                            [](std::size_t index) { return index > 0; }),
            "real Robot MJPEG playback advances beyond the first frame");
    Handle(V5Frame("lesson_stop", 4,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"teach\","
        "\"commandSequenceId\":493}}"));

    const std::vector<std::pair<std::string, std::string>> invalid_replacements = {
        {"\"lessonId\":\"course-mode-v5-farm-candidate\"",
         "\"lessonId\":\"course-mode-v5-farm-candidate-drift\""},
        {"\"layoutContract\":\"layeredCinematic\"",
         "\"layoutContract\":\"flattenedMjpegCinematic\""},
        {"\"lessonVersion\":2,\"manifestChecksum\":\"22e94ced4b2dae1ced13f3e34de1f72e8a3ce177e1ba3a7c599a4c3d002aea0d\"",
         "\"lessonVersion\":2,\"manifestChecksum\":\"205784b3f97cb081ce9c226d8fd83fdd400401e706c000e1b09ba4e7ebdf36ce\""},
        {"\"assetVersionId\":\"75000000-0000-4000-8000-000000000022\"",
         "\"assetVersionId\":\"75000000-0000-4000-8000-000000000021\""},
        {"c466239ff8ba202998e3827b6871906d7fbac6232aeaea3a59b7c69bec7d8777",
         "eac30a7ddf3f14df79f27c3eb39f2114f3a780d5670bb11ef62446f5fa5dcbb9"},
        {"\"bytes\":15086", "\"bytes\":200618"},
        {"\"width\":240,\"height\":240",
         "\"width\":239,\"height\":240"},
        {"\"rect\":{\"x\":20,\"y\":168,\"width\":95,\"height\":95}",
         "\"rect\":{\"x\":20,\"y\":168,\"width\":96,\"height\":95}"},
        {V5CourseModeAssetPath(2),
         V5CourseModePackRoot() + "/alternate-robot-teach.mp4"},
        {V5CourseModeAssetPath(2),
         "file:///sdcard/tbot/lesson-assets/" + std::string(kV5CourseModePackName) +
             "/" + kV5CourseModeAssetIds[2]},
        {V5CourseModeAssetPath(2),
         V5CourseModePackRoot() + "/../" + kV5CourseModeAssetIds[2]},
        {V5CourseModeAssetPath(2), "https://cdn.invalid/robot-teach.mp4"},
        {"\"keyColor\":\"#00ff00\"", "\"keyColor\":\"#ff00ff\""},
        {"\"hasAudio\":false", "\"hasAudio\":true"},
        {"\"mediaKind\":\"video\",\"mediaType\":\"video/mp4\"",
         "\"mediaKind\":\"image\",\"mediaType\":\"image/png\""},
        {"\"fps\":10,\"frameCount\":30", "\"fps\":15,\"frameCount\":45"},
    };
    int sequence = 6;
    std::uint64_t command_sequence = 502;
    std::vector<std::string> admitted_drifts;
    for (const auto& replacement : invalid_replacements) {
        ResetObservable();
        FreshSession();
        const int opens_before = fake.opens;
        Handle(ReplaceOnce(V5CourseModePrepareFrame(sequence++, command_sequence++),
                           replacement.first, replacement.second));
        const bool rejected_before_io = FrameType(0) == "lesson_error" &&
            FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
            fake.opens == opens_before;
        if (!rejected_before_io) {
            std::cerr << "Course Mode v5 drift case was not rejected before IO: "
                      << replacement.second << "\n";
            admitted_drifts.push_back(replacement.second);
        }
    }
    require(admitted_drifts.empty(),
            "v5 Course Mode identity/media/path drift fails closed before renderer IO");

    ResetObservable();
    const int opens_before_missing_current_pack = fake.opens;
    Handle(V5CourseModePrepareFrame(sequence++, command_sequence++, "teach", false));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.opens == opens_before_missing_current_pack,
            "v5 Course Mode requires READY/checksum asset records from the same prepare frame");

    ResetObservable();
    FreshSession();
    fake.fail_open = true;
    Handle(V5CourseModePrepareFrame(sequence++, command_sequence++));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_SD_PATH_MISSING",
            "missing verified Robot file propagates the stable file-open error");
    fake.fail_open = false;

    ResetObservable();
    FreshSession();
    fake.fail_decode = true;
    fake.operation_error = tbot::LessonCinematicError::kDecodeTimeout;
    Handle(V5CourseModePrepareFrame(sequence++, command_sequence++));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_DECODE_TIMEOUT",
            "Robot decoder timeout propagates without advancing the Course Mode phase");
    fake.fail_decode = false;
    fake.operation_error = tbot::LessonCinematicError::kNone;

    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    Board::GetInstance().display_ = nullptr;
    RemoveV5CourseModeAssetPack();
}

void test_renderer_v4_capability_and_exact_single_asset_routing() {
    tbot::SetLessonCinematicRendererCapabilityReady(false);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);
    cJSON* features = cJSON_CreateObject();
    AddLessonRendererFeatures(features);
    char* encoded = cJSON_PrintUnformatted(features);
    require(encoded != nullptr && std::string(encoded) ==
                "{\"renderer\":[\"teebot-lesson-renderer.v1\",\"teebot-lesson-renderer.v2\","
                "\"teebot-lesson-renderer.v4\"],\"lessonRendererV2\":{"
                "\"openingEntrance\":true,\"visualStateEvents\":true,"
                "\"physicalMotionOwner\":\"server\",\"singleSpriteEntrance\":true},"
                "\"lessonRendererV4\":{\"flattenedMjpegCinematic\":true,\"sdAssetPack\":true},"
                "\"lessonCourseMode\":{\"version\":2,\"embodiedActions\":true,"
                "\"reducedMotion\":false,\"faces\":[\"neutral\",\"happy\","
                "\"thinking\",\"relaxed\"]}}",
            "hello advertises exact v4 feature only when v4 production capability is ready");
    cJSON_free(encoded);
    cJSON_Delete(features);

    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(false);
    Handle(V4PrepareFrame(1));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_CAPABILITY_UNSUPPORTED" &&
                fake.allocations == 0 && fake.opens == 0,
            "active renderer remains unavailable until replacement peak capacity is proven");

    ResetObservable();
    FreshSession();
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);
    Handle(V4PrepareFrame(1));
    require(fake.opened_paths == std::vector<std::string>({
                "/sdcard/tbot/lesson-assets/flattenedCinematic.opening"}),
            "v4 routes one normalized SD asset to the flattened renderer");
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady",
            "v4 prepare returns the exact typed frame-zero ACK");
    Handle(WithSession(V4Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":72}}"), "foreign-session"));
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "CINEMATIC_SESSION_MISMATCH" &&
                fake.presents == 1,
            "foreign v4 start is rejected before touching renderer state");
    Handle(V4Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":72,\"extra\":true}}"));
    require(FrameType(2) == "lesson_error" && fake.presents == 1,
            "v4 rejects extra control keys before changing renderer state");
    Handle(V4Frame("lesson_cinematic_control", 3,
        "{\"command\":\"start\",\"phaseId\":\"opening\",\"commandSequenceId\":72}"));
    require(FrameType(3) == "lesson_error" &&
                FrameBodyStr(3, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.presents == 1,
            "v4 template-v1 rejects control-frame start without consuming the command");
    Handle(V4Frame("lesson_start", 3,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":72}}"));
    require(FrameType(4) == "lesson_ack" &&
                FrameBodyStr(4, "cinematicPhase", "event") == "phaseReady",
            "valid owner v4 start still applies after foreign and rejected control starts");
    Handle(V4Frame("lesson_cinematic_control", 4,
        "{\"command\":\"pause\",\"phaseId\":\"opening\",\"commandSequenceId\":73}"));
    require(FrameType(5) == "lesson_ack", "valid v4 pause applies");
    Handle(V4Frame("lesson_cinematic_control", 5,
        "{\"command\":\"resume\",\"phaseId\":\"opening\",\"commandSequenceId\":74,"
        "\"clockRebaseSequenceId\":\"74\"}"));
    require(FrameType(6) == "lesson_error", "string resume rebase identity is rejected");
    Handle(V4Frame("lesson_cinematic_control", 6,
        "{\"command\":\"resume\",\"phaseId\":\"opening\",\"commandSequenceId\":74,"
        "\"clockRebaseSequenceId\":999}"));
    require(FrameType(7) == "lesson_error", "mismatched resume rebase identity is rejected");
    Handle(V4Frame("lesson_cinematic_control", 7,
        "{\"command\":\"resume\",\"phaseId\":\"opening\",\"commandSequenceId\":74,"
        "\"clockRebaseSequenceId\":74}"));
    require(FrameType(8) == "lesson_ack", "matching positive resume rebase identity applies");
    Handle(V4Frame("lesson_cinematic_control", 8,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":75,"
        "\"reason\":\"\"}"));
    require(FrameType(9) == "lesson_error", "empty cancel reason is rejected");
    Handle(V4Frame("lesson_cinematic_control", 9,
        std::string("{\"command\":\"cancel\",\"phaseId\":\"opening\","
                    "\"commandSequenceId\":75,\"reason\":\"") +
        std::string(65, 'x') + "\"}"));
    require(FrameType(10) == "lesson_error", "cancel reason longer than 64 bytes is rejected");
    Handle(V4Frame("lesson_cinematic_control", 10,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":75,"
        "\"reason\":\"assignmentReplaced\"}"));
    require(FrameType(11) == "lesson_ack", "bounded nonempty cancel reason applies");

    ResetObservable();
    FreshSession();
    Handle(V4PrepareFrame(1, 72, ",\"extra\":true"));
    require(FrameType(0) == "lesson_error" && fake.opens == 1,
            "v4 rejects extra asset keys before file open");
    require(FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH",
            "malformed v4 command returns the typed metadata error");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(false);
}

void test_renderer_v4_accepts_template_v2_cue_identity_prepare_and_controls() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonFlattenedCinematicRenderer(&renderer);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);

    Handle(V4V2BarnCuePrepareFrame(1));
    require(fake.opened_paths == std::vector<std::string>({
                "/sdcard/tbot/lesson-assets/flattenedCinematic.barn-opening"}),
            "v4 template v2 barn cue routes its single SD asset by cueId");
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady" &&
                FrameBodyStr(0, "cinematicPhase", "cueId") == "barn-opening",
            "v4 template v2 prepare ACK echoes cueId instead of phaseId");

    Handle(V4V2BarnCueCommandFrame("lesson_cinematic_control", "start", 2, 172));
    require(FrameType(1) == "lesson_ack" &&
                FrameBodyStr(1, "cinematicPhase", "event") == "phaseReady" &&
                FrameBodyStr(1, "cinematicPhase", "cueId") == "barn-opening",
            "v4 template v2 production control-start applies by cueId");
    Handle(V4V2BarnCueCommandFrame("lesson_cinematic_control", "pause", 3, 173));
    require(FrameType(2) == "lesson_ack", "v4 template v2 pause applies by cueId");
    Handle(V4V2BarnCueCommandFrame("lesson_cinematic_control", "resume", 4, 174,
                                   ",\"clockRebaseSequenceId\":174"));
    require(FrameType(3) == "lesson_ack", "v4 template v2 resume applies by cueId");
    Handle(V4V2BarnCueCommandFrame("lesson_cinematic_control", "cancel", 5, 175,
                                   ",\"reason\":\"testCleanup\""));
    require(FrameType(4) == "lesson_ack", "v4 template v2 cancel applies by cueId");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "v4 template v2 terminal cue control releases the asset lease");

    ResetObservable();
    FreshSession();
    const std::string ready_file_name = "tbot-v4-combined-prepare-ack.bin";
    const std::string manifest_checksum = "abcdef1234567890";
    const std::string cache_key = "tvideo-v7-" + manifest_checksum;
    Handle(V4V2BarnCuePrepareFrame(
        1, 181, ReadyAssetPackExtra(cache_key, manifest_checksum, ready_file_name)));
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady" &&
                FrameHasAssetPack(0) && FrameAssetPackReady(0) &&
                FrameBodyStr(0, "assetPack", "cacheKey") == cache_key,
            "v4 cinematic prepare ACK carries frame-zero readiness and SD-pack attestation together");
    Handle(V4V2BarnCueCommandFrame("lesson_cinematic_control", "cancel", 2, 182,
                                   ",\"reason\":\"testCleanup\""));
    RemoveReadyAssetPackFixture(ready_file_name);

    const int opens_before_stale_duration = fake.opens;
    ResetObservable();
    FreshSession();
    Handle(ReplaceOnce(V4V2BarnCuePrepareFrame(6, 181),
                       "\"durationMs\":9500", "\"durationMs\":9400"));
    require(FrameType(0) == "lesson_error" && fake.opens == opens_before_stale_duration,
            "v4 template v2 rejects stale effect duration before renderer open");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_renderer_v4_course_mode_compatibility_is_exact_and_narrow() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);

    Handle(V4CourseModePrepareFrame(1));
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady" &&
                FrameBodyStr(0, "cinematicPhase", "cueId") == "cat-discover",
            "exact frozen Course Mode marker and cue identity admit 2000ms once playback");

    ResetObservable();
    FreshSession();
    const int opens_before_bad_marker = fake.opens;
    Handle(ReplaceOnce(V4CourseModePrepareFrame(2),
                       "renderer-v4.course-mode-layout.v1",
                       "renderer-v4.course-mode-layout.v2"));
    require(FrameType(0) == "lesson_error" && fake.opens == opens_before_bad_marker,
            "Course Mode compatibility rejects any marker identity drift before renderer IO");

    ResetObservable();
    FreshSession();
    const int opens_before_stale_generic = fake.opens;
    Handle(V4V2PrepareFrame(3, 283, "", "", "cat-discover", "teach", "once", 2000, 20));
    require(FrameType(0) == "lesson_error" && fake.opens == opens_before_stale_generic,
            "generic renderer-v4 path still rejects stale 2000ms once teach cues");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_renderer_v4_accepts_external_flattened_cue_metadata_matrix() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonFlattenedCinematicRenderer(&renderer);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);

    const int opens_before_invalid_slug = fake.opens;
    Handle(ReplaceOnce(V4V2BarnCuePrepareFrame(1, 201),
                       "\"stepKey\":\"barn\"", "\"stepKey\":\"barn_name\""));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                fake.opens == opens_before_invalid_slug,
            "external flattened cue rejects a non-slug stepKey before renderer work");

    struct CueMetadataCase {
        const char* effect;
        const char* playback_mode;
        int duration_ms;
    };
    const std::vector<CueMetadataCase> cues = {
        {"greet", "loop", 1200},
        {"teach", "once", 2600},
        {"listen", "loop", 1300},
        {"thinking", "loop", 1300},
        {"correct", "once", 600},
        {"retry-level-1", "once", 1200},
        {"retry-level-2", "once", 1400},
        {"retry-level-3", "once", 1600},
        {"celebrate", "once", 3000},
        {"word-transition", "once", 1100},
    };
    int sequence = 2;
    std::uint64_t command_sequence_id = 202;
    for (const CueMetadataCase& cue : cues) {
        ResetObservable();
        FreshSession();
        std::string frame = V4V2BarnCuePrepareFrame(sequence++, command_sequence_id++);
        frame = ReplaceOnce(frame, "\"effect\":\"opening\"",
                            std::string("\"effect\":\"") + cue.effect + "\"");
        frame = ReplaceOnce(frame, "\"playbackMode\":\"once\"",
                            std::string("\"playbackMode\":\"") + cue.playback_mode + "\"");
        frame = ReplaceOnce(frame, "\"durationMs\":9500",
                            "\"durationMs\":" + std::to_string(cue.duration_ms));
        frame = ReplaceOnce(frame, "\"frameCount\":95",
                            "\"frameCount\":" + std::to_string(cue.duration_ms / 100));

        const int opens_before = fake.opens;
        Handle(frame);
        require(FrameType(0) == "lesson_error" &&
                    FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH" &&
                    fake.opens == opens_before + 1,
                "known external flattened cue metadata reaches renderer file validation");
    }
    require(fake.opens == static_cast<int>(cues.size()),
            "every known external flattened cue effect passes handler metadata validation");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_renderer_v4_numeric_narrowing_rejects_before_renderer_work() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);

    const std::vector<std::pair<std::string, std::string>> invalid_numbers = {
        {"\"durationMs\":300", "\"durationMs\":4294967596"},
        {"\"durationMs\":300", "\"durationMs\":300.5"},
        {"\"frameCount\":3", "\"frameCount\":4294967299"},
        {"\"bytes\":1234", "\"bytes\":9007199254740992"},
        {"\"commandSequenceId\":71", "\"commandSequenceId\":9007199254740992"},
    };
    int sequence = 1;
    for (const auto& replacement : invalid_numbers) {
        Handle(ReplaceOnce(V4PrepareFrame(sequence++), replacement.first, replacement.second));
        require(FrameType(Sent().size() - 1) == "lesson_error",
                "unsafe v4 numeric narrowing is rejected with a typed error");
        require(fake.allocations == 0 && fake.opens == 0 && fake.presents == 0,
                "unsafe v4 numeric narrowing is rejected before renderer work");
    }
    Handle(ReplaceOnce(
        ReplaceOnce(V4PrepareFrame(sequence++), "\"durationMs\":300",
                    "\"durationMs\":4294967300"),
        "\"frameCount\":3", "\"frameCount\":42949673"));
    require(FrameType(Sent().size() - 1) == "lesson_error" &&
                fake.allocations == 0 && fake.opens == 0,
            "internally consistent duration above uint32 range is rejected before renderer work");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "unsafe v4 numeric narrowing does not acquire an asset lease");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_renderer_v4_template_v2_exact_cue_schema_and_ack_identity() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);

    Handle(V4V2PrepareFrame(1));
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "event") == "frameZeroReady" &&
                FrameBodyStr(0, "cinematicPhase", "cueId") == "barn-correct" &&
                FrameBodyStr(0, "cinematicPhase", "phaseId").empty(),
            "v2 prepare ACK echoes cueId without relabeling it as phaseId");
    cJSON* ack = cJSON_Parse(Sent().back().c_str());
    cJSON* cinematic = cJSON_GetObjectItem(cJSON_GetObjectItem(ack, "body"), "cinematicPhase");
    require(cJSON_GetArraySize(cinematic) == 6 &&
                cJSON_GetObjectItem(cinematic, "cueId") != nullptr &&
                cJSON_GetObjectItem(cinematic, "phaseId") == nullptr,
            "v2 frame-zero ACK has the exact six-field cue schema");
    cJSON_Delete(ack);

    Handle(V4Frame("lesson_cinematic_control", 2,
        "{\"command\":\"start\",\"cueId\":\"barn-correct\",\"commandSequenceId\":82}"));
    require(FrameType(1) == "lesson_ack" &&
                FrameBodyStr(1, "cinematicPhase", "event") == "phaseReady" &&
                FrameBodyStr(1, "cinematicPhase", "cueId") == "barn-correct",
            "v2 control-frame start uses cue identity end to end");
    ack = cJSON_Parse(Sent().back().c_str());
    cinematic = cJSON_GetObjectItem(cJSON_GetObjectItem(ack, "body"), "cinematicPhase");
    require(cJSON_GetArraySize(cinematic) == 6 &&
                std::string(cJSON_GetObjectItem(cinematic, "event")->valuestring) ==
                    "phaseReady" &&
                std::string(cJSON_GetObjectItem(cinematic, "command")->valuestring) == "start" &&
                std::string(cJSON_GetObjectItem(cinematic, "cueId")->valuestring) ==
                    "barn-correct" &&
                cJSON_GetObjectItem(cinematic, "phaseId") == nullptr &&
                cJSON_GetObjectItem(cinematic, "commandSequenceId")->valueint == 82 &&
                cJSON_IsTrue(cJSON_GetObjectItem(cinematic, "accepted")) &&
                cJSON_IsTrue(cJSON_GetObjectItem(cinematic, "phaseReady")),
            "v2 control-start ACK has the exact six-field cue identity schema");
    cJSON_Delete(ack);
    Handle(V4Frame("lesson_cinematic_control", 3,
        "{\"command\":\"pause\",\"cueId\":\"barn-correct\",\"commandSequenceId\":83}"));
    require(FrameType(2) == "lesson_ack", "v2 pause accepts exact cue control schema");
    Handle(V4Frame("lesson_cinematic_control", 4,
        "{\"command\":\"resume\",\"cueId\":\"barn-correct\",\"commandSequenceId\":84,"
        "\"clockRebaseSequenceId\":84}"));
    require(FrameType(3) == "lesson_ack", "v2 resume accepts exact cue control schema");
    Handle(V4Frame("lesson_cinematic_control", 5,
        "{\"command\":\"cancel\",\"cueId\":\"barn-correct\",\"commandSequenceId\":85,"
        "\"reason\":\"testCleanup\"}"));
    require(FrameType(4) == "lesson_ack", "v2 cancel accepts exact cue control schema");

    ResetObservable();
    FreshSession();
    Handle(V4V2PrepareFrame(1, 86));
    Handle(V4Frame("lesson_cinematic_control", 2,
        "{\"command\":\"start\",\"cueId\":\"barn-correct\",\"commandSequenceId\":87,"
        "\"effect\":\"correct\"}"));
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH",
            "v2 control-start rejects leaked prepare metadata instead of widening schema");

    ResetObservable();
    FreshSession();
    Handle(V4V2PrepareFrame(1, 1));
    require(FrameType(0) == "lesson_ack" &&
                FrameBodyStr(0, "cinematicPhase", "cueId") == "barn-correct",
            "fresh v2 session safely resets its command sequence to one");
    Handle(V4Frame("lesson_cinematic_control", 2,
        "{\"command\":\"cancel\",\"cueId\":\"barn-correct\",\"commandSequenceId\":2,"
        "\"reason\":\"testCleanup\"}"));
    require(FrameType(1) == "lesson_ack", "fresh low-sequence v2 session remains controllable");

    ResetObservable();
    FreshSession();
    Handle(V4V2PrepareFrame(1, 11, "", "", "barn-listen", "listen", "loop", 1300, 13));
    require(FrameType(0) == "lesson_ack", "v2 loop cue parses through the exact handler schema");
    Handle(V4Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"barn-listen\","
        "\"commandSequenceId\":12}}"));
    require(renderer.Tick(1300).type == tbot::LessonCinematicResponseType::kCommandApplied,
            "handler maps v2 loop playback instead of completing at the first seam");
    Handle(V4Frame("lesson_cinematic_control", 3,
        "{\"command\":\"cancel\",\"cueId\":\"barn-listen\",\"commandSequenceId\":13,"
        "\"reason\":\"testCleanup\"}"));
    require(FrameType(2) == "lesson_ack", "parsed v2 loop cue cleans up normally");

    ResetObservable();
    FreshSession();
    const int baseline_opens = fake.opens;
    const std::vector<std::string> malformed = {
        V4V2PrepareFrame(1, 91, ",\"phaseId\":\"correct\""),
        V4V2PrepareFrame(2, 92, ",\"extra\":true"),
        ReplaceOnce(V4V2PrepareFrame(3, 93), "\"cueId\":\"barn-correct\",", ""),
        ReplaceOnce(V4V2PrepareFrame(4, 94), "\"cueId\":\"barn-correct\"",
                    "\"cueId\":\"Barn Correct\""),
        ReplaceOnce(V4V2PrepareFrame(5, 95), "\"effect\":\"correct\"",
                    "\"effect\":\"unknown\""),
        ReplaceOnce(V4V2PrepareFrame(6, 96), "\"playbackMode\":\"once\"",
                    "\"playbackMode\":\"loop\""),
        V4V2PrepareFrame(7, 97, "", ",\"phaseId\":\"correct\""),
    };
    for (const auto& frame : malformed) {
        Handle(frame);
        require(FrameType(Sent().size() - 1) == "lesson_error" &&
                    FrameBodyStr(Sent().size() - 1, nullptr, "code") ==
                        "CINEMATIC_METADATA_MISMATCH",
                "malformed or inexact v2 schema is rejected with typed metadata error");
    }
    require(fake.opens == baseline_opens,
            "malformed v2 commands are rejected before renderer file work");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_tvideo_farm_cross_repository_fixture_runs_prepare_start_through_handler() {
    const auto object = [](cJSON* parent, const char* key) -> cJSON* {
        cJSON* value = cJSON_GetObjectItem(parent, key);
        return cJSON_IsObject(value) ? value : nullptr;
    };
    const auto string = [](cJSON* parent, const char* key) -> const char* {
        cJSON* value = cJSON_GetObjectItem(parent, key);
        return cJSON_IsString(value) ? value->valuestring : nullptr;
    };
    const char* fixture_path = std::getenv("TBOT_TVIDEO_FARM_COMMAND_FIXTURE");
    require(fixture_path != nullptr && fixture_path[0] != '\0',
            "farm command fixture path is configured");
    std::ifstream input(fixture_path, std::ios::binary);
    require(input.good(), "farm command fixture opens");
    const std::string encoded((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    cJSON* fixture = cJSON_ParseWithLength(encoded.data(), encoded.size());
    require(fixture != nullptr, "farm command fixture parses");
    require(std::string(string(fixture, "schemaVersion")) == "tvideo-farm-command.v2" &&
                cJSON_IsTrue(cJSON_GetObjectItem(fixture, "softwareOnly")) &&
                std::string(string(fixture, "hardwareStatus")) == "PENDING_ATTENDED_HARDWARE",
            "farm fixture is explicitly software-only");
    cJSON* source = object(fixture, "source");
    cJSON* frames = cJSON_GetObjectItem(fixture, "frames");
    require(source != nullptr && frames != nullptr && cJSON_IsArray(frames) &&
                cJSON_GetArraySize(frames) == 38 &&
                cJSON_GetNumberValue(cJSON_GetObjectItem(source, "cueCount")) == 19,
            "farm fixture contains the exact 19 ordered prepare-start pairs");

    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);
    std::vector<std::string> cue_order;
    for (int index = 0; index < 38; index += 2) {
        cJSON* prepare = cJSON_GetArrayItem(frames, index);
        cJSON* start = cJSON_GetArrayItem(frames, index + 1);
        cJSON* prepare_body = object(prepare, "body");
        cJSON* command = object(prepare_body, "cinematicPhase");
        cJSON* start_body = object(start, "body");
        cJSON* asset = object(command, "asset");
        const char* cue_id = string(command, "cueId");
        require(cue_id != nullptr && std::string(string(asset, "cueId")) == cue_id &&
                    std::string(string(start_body, "cueId")) == cue_id &&
                    cJSON_GetArraySize(start_body) == 3 &&
                    cJSON_GetObjectItem(start_body, "effect") == nullptr &&
                    cJSON_GetObjectItem(start_body, "asset") == nullptr,
                "farm pair preserves cue identity and strict metadata-free start schema");
        cue_order.emplace_back(cue_id);

        char* prepare_json = cJSON_PrintUnformatted(prepare);
        char* start_json = cJSON_PrintUnformatted(start);
        require(prepare_json != nullptr && start_json != nullptr,
                "farm pair serializes for the real handler");
        const std::size_t before = Sent().size();
        Handle(prepare_json);
        if (!(Sent().size() == before + 1 && FrameType(before) == "lesson_ack" &&
              FrameBodyStr(before, "cinematicPhase", "event") == "frameZeroReady")) {
            std::cerr << "farm prepare rejected cue=" << cue_id
                      << " response=" << (Sent().size() > before ? Sent().back() : "<none>")
                      << "\n";
        }
        require(Sent().size() == before + 1 && FrameType(before) == "lesson_ack" &&
                    FrameBodyStr(before, "cinematicPhase", "event") == "frameZeroReady" &&
                    FrameBodyStr(before, "cinematicPhase", "cueId") == cue_id,
                "farm prepare reaches the real renderer boundary and ACKs frame zero");
        Handle(start_json);
        if (!(Sent().size() == before + 2 && FrameType(before + 1) == "lesson_ack" &&
              FrameBodyStr(before + 1, "cinematicPhase", "event") == "phaseReady")) {
            std::cerr << "farm start rejected cue=" << cue_id
                      << " response=" << (Sent().size() > before + 1 ? Sent().back() : "<none>")
                      << "\n";
        }
        require(Sent().size() == before + 2 && FrameType(before + 1) == "lesson_ack" &&
                    FrameBodyStr(before + 1, "cinematicPhase", "event") == "phaseReady" &&
                    FrameBodyStr(before + 1, "cinematicPhase", "cueId") == cue_id,
                "farm start reaches the real renderer boundary with exact cue identity");
        cJSON_free(prepare_json);
        cJSON_free(start_json);
    }
    require(cue_order.front() == "barn-opening" &&
                cue_order[10] == "barn-to-hay-word-transition" &&
                cue_order.back() == "hay-celebrate" && fake.opens == 19,
            "farm fixture preserves exact cue order and opens one stream per prepared cue");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
    cJSON_Delete(fixture);
}

void test_cinematic_cross_renderer_handoff_releases_old_resources() {
    ResetObservable();
    FreshSession();
    V3RendererFake v3_fake;
    V3RendererFake v4_fake;
    tbot::LessonCinematicRenderer v3_renderer(
        {&v3_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::LessonFlattenedCinematicRenderer v4_renderer(
        {&v4_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&v3_renderer);
    ActivateV4Renderer(&v4_renderer);

    Handle(V3PrepareFrame(1));
    Handle(V4PrepareFrame(2));
    require(FrameType(1) == "lesson_ack" && !v3_renderer.prepared() && v4_renderer.prepared(),
            "v3 to v4 handoff commits only the new renderer");
    require(v3_fake.opens == v3_fake.closes && v3_fake.allocations == v3_fake.frees,
            "v3 to v4 handoff releases every old file and allocation");
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(!static_cast<bool>(mutation),
                "v3 to v4 handoff keeps storage mutation blocked while v4 is active");
    }
    Handle(V4Frame("lesson_cinematic_control", 3,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":72,"
        "\"reason\":\"testCleanup\"}"));
    require(FrameType(2) == "lesson_ack" && v4_fake.opens == v4_fake.closes &&
                v4_fake.allocations == v4_fake.frees,
            "v4 cancel releases every handoff target file and allocation");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "v4 cancel releases the handoff asset lease");
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "next storage operation is allowed after v3 to v4");
    }

    ResetObservable();
    FreshSession();
    Handle(V4PrepareFrame(1, 81));
    Handle(V3PrepareFrame(2, 82));
    require(FrameType(1) == "lesson_ack" && !v4_renderer.prepared() && v3_renderer.prepared(),
            "v4 to v3 handoff commits only the new renderer");
    require(v4_fake.opens == v4_fake.closes && v4_fake.allocations == v4_fake.frees,
            "v4 to v3 handoff releases every old file and allocation");
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(!static_cast<bool>(mutation),
                "v4 to v3 handoff keeps storage mutation blocked while v3 is active");
    }
    Handle(V3Frame("lesson_stop", 3,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":83}}"));
    require(FrameType(2) == "lesson_ack" && v3_fake.opens == v3_fake.closes &&
                v3_fake.allocations == v3_fake.frees,
            "v3 stop releases every handoff target file and allocation");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "v3 stop releases the handoff asset lease");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_layered_cinematic_coverage_boundaries() {
    V3RendererFake v5_fake;
    V3RendererFake v4_fake;
    tbot::LessonLayeredCinematicRenderer v5_renderer(
        {&v5_fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::LessonFlattenedCinematicRenderer v4_renderer(
        {&v4_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonLayeredCinematicRenderer(&v5_renderer);
    ActivateV4Renderer(&v4_renderer);

    for (const char* phase : {"teach", "listen", "thinking", "celebrate"}) {
        ResetObservable();
        FreshSession();
        Handle(ReplaceOnce(V5PrepareFrame(1), "\"phaseId\":\"flyIn\"",
                           std::string("\"phaseId\":\"") + phase + "\""));
        require(FrameType(0) == "lesson_ack",
                "every frozen middle v5 phase identity reaches layered prepare");
        Handle(V5Frame("lesson_stop", 2,
            std::string("{\"cinematicPhase\":{\"command\":\"stop\",\"phaseId\":\"") +
            phase + "\",\"commandSequenceId\":92}}"));
    }

    ResetObservable();
    FreshSession();
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "v5 reservation-refusal fixture holds mutation lease");
        Handle(V5PrepareFrame(1));
        require(FrameType(0) == "lesson_error" &&
                    FrameBodyStr(0, nullptr, "code") == "CINEMATIC_SD_PATH_MISSING",
                "v5 reservation refusal maps to the stable path error before renderer work");
    }

    ResetObservable();
    FreshSession();
    v5_fake.fail_open = true;
    Handle(V5PrepareFrame(1));
    require(FrameType(0) == "lesson_error" &&
                !LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "rejected v5 prepare releases its newly acquired storage reservation");
    v5_fake.fail_open = false;

    ResetObservable();
    FreshSession();
    Handle(V5PrepareFrame(1));
    Handle(V4PrepareFrame(2));
    require(FrameType(1) == "lesson_ack" && !v5_renderer.prepared() && v4_renderer.prepared(),
            "v5 to v4 handoff releases the layered renderer before committing v4");
    Handle(V4Frame("lesson_cinematic_control", 3,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":72,"
        "\"reason\":\"testCleanup\"}"));

    ResetObservable();
    FreshSession();
    tbot::LessonCinematicRenderer v3_renderer(
        {&v4_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&v3_renderer);
    Handle(V3PrepareFrame(1));
    require(App().AbandonLessonStorageSession() && !v3_renderer.prepared(),
            "transport abandonment discards an active v3 renderer");

    ResetObservable();
    FreshSession();
    Handle(V4PrepareFrame(1));
    require(App().AbandonLessonStorageSession() && !v4_renderer.prepared(),
            "transport abandonment discards an active v4 renderer");

    tbot::SetActiveLessonCinematicRenderer(nullptr);
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
}

void test_cinematic_cross_renderer_handoff_fails_closed_without_old_renderer() {
    ResetObservable();
    FreshSession();
    V3RendererFake v3_fake;
    V3RendererFake v4_fake;
    tbot::LessonCinematicRenderer v3_renderer(
        {&v3_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::LessonFlattenedCinematicRenderer v4_renderer(
        {&v4_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&v3_renderer);
    ActivateV4Renderer(&v4_renderer);

    Handle(V3PrepareFrame(1));
    tbot::SetActiveLessonCinematicRenderer(nullptr);
    Handle(V4PrepareFrame(2));
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "CINEMATIC_SESSION_RELEASE_FAILED" &&
                !v4_renderer.prepared(),
            "v3 to v4 handoff fails closed when the old renderer disappears");
    v3_renderer.DiscardSession();
    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();

    ResetObservable();
    FreshSession();
    tbot::SetActiveLessonCinematicRenderer(&v3_renderer);
    ActivateV4Renderer(&v4_renderer);
    Handle(V4PrepareFrame(1));
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
    Handle(V3PrepareFrame(2));
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "CINEMATIC_SESSION_RELEASE_FAILED" &&
                !v3_renderer.prepared(),
            "v4 to v3 handoff fails closed when the old renderer disappears");
    v4_renderer.DiscardSession();
    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_renderer_v4_lesson_stop_routes_to_flattened_renderer() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);
    Handle(V4V2PrepareFrame(1));
    Handle(V4Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"barn-correct\","
        "\"commandSequenceId\":82}}"));
    Handle(V4Frame("lesson_stop", 3,
        "{\"cinematicPhase\":{\"command\":\"stop\",\"cueId\":\"barn-correct\","
        "\"commandSequenceId\":83}}"));
    require(FrameType(2) == "lesson_ack" && !renderer.prepared() &&
                !LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "v4 lesson_stop reaches the flattened renderer and releases the session");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_cinematic_terminal_waits_for_asset_lease_release() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);
    Handle(V4PrepareFrame(1));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(AID(), SID());
    require(owner.acquired && owner.idempotent, "v4 terminal test owns its asset session");
    auto retained = LessonAssetStorageCoordinator::GetInstance().TryRetainLessonSession(
        AID(), SID(), owner.generation);
    require(static_cast<bool>(retained), "v4 terminal test holds a blocking read lease");

    const std::string cancel = V4Frame("lesson_cinematic_control", 2,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":72,"
        "\"reason\":\"testCleanup\"}");
    Handle(cancel);
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "CINEMATIC_SESSION_RELEASE_FAILED",
            "terminal cleanup reports a typed failure while a read lease remains");
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "failed terminal cleanup preserves the asset session for retry");

    retained = {};
    Handle(cancel);
    require(FrameType(2) == "lesson_ack" &&
                !LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "duplicate terminal command completes after the blocking lease releases");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_renderer_v4_fresh_prepare_resets_session_sequence_stream() {
    ResetObservable();
    FreshSession();
    V3RendererFake v3_fake;
    V3RendererFake v4_fake;
    tbot::LessonCinematicRenderer v3_renderer(
        {&v3_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::LessonFlattenedCinematicRenderer v4_renderer(
        {&v4_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&v3_renderer);
    ActivateV4Renderer(&v4_renderer);
    Handle(V3PrepareFrame(9, 61));
    Handle(V3Frame("lesson_start", 10,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":62}}"));
    require(FrameSeq(0) == 1 && FrameSeq(1) == 2,
            "previous cinematic session owns a nonzero firmware sequence stream");

    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    FreshSession();
    Handle(V4PrepareFrame(1, 71));
    require(FrameType(2) == "lesson_ack" && FrameSeq(2) == 1,
            "fresh v4 prepare resets F-to-S sequence before frame-zero ACK");
    Handle(V4Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":72}}"));
    require(FrameType(3) == "lesson_ack" && FrameSeq(3) == 2,
            "fresh v4 session continues its own sequence stream");
    Handle(V4Frame("lesson_cinematic_control", 3,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":73,"
        "\"reason\":\"testCleanup\"}"));
    tbot::SetActiveLessonCinematicRenderer(nullptr);
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_renderer_v4_failed_same_session_reprepare_keeps_session_playable() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    ActivateV4Renderer(&renderer);
    Handle(V4PrepareFrame(1, 80));
    require(FrameType(0) == "lesson_ack" && renderer.prepared(),
            "baseline v4 session is prepared");
    fake.fail_open = true;
    Handle(V4PrepareFrame(2, 81));
    require(FrameType(1) == "lesson_error" && renderer.prepared(),
            "failed idempotent reprepare preserves prepared renderer and handler lease");
    fake.fail_open = false;
    Handle(V4Frame("lesson_start", 3,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":82}}"));
    require(FrameType(2) == "lesson_ack" &&
                FrameBodyStr(2, "cinematicPhase", "event") == "phaseReady",
            "old stream remains playable after same-session reprepare failure");
    Handle(V4Frame("lesson_cinematic_control", 4,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":83,"
        "\"reason\":\"testCleanup\"}"));
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_cinematic_controls_cannot_cross_renderer_session_identity() {
    ResetObservable();
    FreshSession();
    V3RendererFake v3_fake;
    V3RendererFake v4_fake;
    tbot::LessonCinematicRenderer v3_renderer(
        {&v3_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::LessonFlattenedCinematicRenderer v4_renderer(
        {&v4_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&v3_renderer);
    ActivateV4Renderer(&v4_renderer);
    Handle(V4PrepareFrame(1, 90));
    Handle(V3Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":91}}"));
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "CINEMATIC_SESSION_MISMATCH",
            "v3 control cannot operate a v4-prepared session with the same IDs");
    Handle(V4Frame("lesson_start", 3,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":91}}"));
    require(FrameType(2) == "lesson_ack",
            "correct renderer control remains usable after cross-renderer rejection");
    Handle(V4Frame("lesson_cinematic_control", 4,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":92,"
        "\"reason\":\"testCleanup\"}"));
    tbot::SetActiveLessonCinematicRenderer(nullptr);
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
}

void test_renderer_v3_exact_typed_ack_lifecycle_and_idempotency() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonCinematicRenderer renderer({&fake, V3Allocate, V3Free, V3Open, V3Close,
                                             V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&renderer);
    Handle(V3PrepareFrame(1));
    require(fake.opened_paths == std::vector<std::string>({
                "/sdcard/tbot/lesson-assets/background.mp4",
                "/sdcard/tbot/lesson-assets/object.mp4",
                "/sdcard/tbot/lesson-assets/robot.mp4"}),
            "v3 prepare normalizes sd:// asset aliases before renderer file open");
    require(FrameType(0) == "lesson_ack", "v3 prepare returns lesson_ack");
    cJSON* ack = cJSON_Parse(Sent().back().c_str());
    cJSON* cinematic = cJSON_GetObjectItem(cJSON_GetObjectItem(ack, "body"), "cinematicPhase");
    require(cJSON_GetArraySize(cinematic) == 6 &&
                std::string(cJSON_GetObjectItem(cinematic, "event")->valuestring) == "frameZeroReady" &&
                std::string(cJSON_GetObjectItem(cinematic, "command")->valuestring) == "prepare" &&
                std::string(cJSON_GetObjectItem(cinematic, "phaseId")->valuestring) == "opening" &&
                cJSON_GetObjectItem(cinematic, "commandSequenceId")->valueint == 41 &&
                cJSON_IsTrue(cJSON_GetObjectItem(cinematic, "accepted")) &&
                cJSON_IsTrue(cJSON_GetObjectItem(cinematic, "frameZeroReady")),
            "v3 prepare ACK exactly matches Task-7 frameZeroReady DTO");
    cJSON_Delete(ack);
    const int presents = fake.presents;
    Handle(V3PrepareFrame(2));
    require(fake.presents == presents, "duplicate prepare command ID replays ACK without work");

    Handle(WithSession(V3Frame("lesson_start", 3,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":42}}"), "foreign-session"));
    require(FrameType(Sent().size() - 1) == "lesson_error" &&
                FrameBodyStr(Sent().size() - 1, nullptr, "code") ==
                    "CINEMATIC_SESSION_MISMATCH",
            "foreign v3 start is rejected without advancing renderer state");

    Handle(V3Frame("lesson_cinematic_control", 3,
        "{\"command\":\"start\",\"phaseId\":\"opening\",\"commandSequenceId\":42}"));
    require(FrameType(Sent().size() - 1) == "lesson_error" &&
                FrameBodyStr(Sent().size() - 1, nullptr, "code") ==
                    "CINEMATIC_METADATA_MISMATCH" &&
                fake.presents == presents,
            "v3 rejects control-frame start without consuming the command");

    Handle(V3Frame("lesson_start", 3,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\",\"commandSequenceId\":42}}"));
    ack = cJSON_Parse(Sent().back().c_str());
    cinematic = cJSON_GetObjectItem(cJSON_GetObjectItem(ack, "body"), "cinematicPhase");
    require(cJSON_GetArraySize(cinematic) == 6 &&
                std::string(cJSON_GetObjectItem(cinematic, "event")->valuestring) == "phaseReady" &&
                cJSON_IsTrue(cJSON_GetObjectItem(cinematic, "phaseReady")),
            "v3 start ACK exactly matches Task-7 phaseReady DTO");
    cJSON_Delete(ack);

    for (const char* command : {"pause", "resume", "cancel"}) {
        const int command_id = command[0] == 'p' ? 43 : command[0] == 'r' ? 44 : 45;
        Handle(V3Frame("lesson_cinematic_control", command_id,
            std::string("{\"command\":\"") + command +
            "\",\"phaseId\":\"opening\",\"commandSequenceId\":" +
            std::to_string(command_id) + "}"));
        ack = cJSON_Parse(Sent().back().c_str());
        cinematic = cJSON_GetObjectItem(cJSON_GetObjectItem(ack, "body"), "cinematicPhase");
        require(cJSON_GetArraySize(cinematic) == 5 &&
                    std::string(cJSON_GetObjectItem(cinematic, "event")->valuestring) == "commandApplied" &&
                    std::string(cJSON_GetObjectItem(cinematic, "command")->valuestring) == command &&
                    cJSON_IsTrue(cJSON_GetObjectItem(cinematic, "accepted")),
                "v3 control ACK exactly matches Task-7 commandApplied DTO");
        cJSON_Delete(ack);
    }
    require(fake.closes == 3, "cancel closes all streams before handler releases lesson lease");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_renderer_v3_controls_reject_v4_only_fields() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonCinematicRenderer renderer({&fake, V3Allocate, V3Free, V3Open, V3Close,
                                             V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&renderer);
    Handle(V3PrepareFrame(1));
    Handle(V3Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":42}}"));
    Handle(V3Frame("lesson_cinematic_control", 3,
        "{\"command\":\"pause\",\"phaseId\":\"opening\",\"commandSequenceId\":43}"));

    Handle(V3Frame("lesson_cinematic_control", 4,
        "{\"command\":\"resume\",\"phaseId\":\"opening\",\"commandSequenceId\":44,"
        "\"clockRebaseSequenceId\":44}"));
    require(FrameType(3) == "lesson_error",
            "v3 resume rejects the v4-only clock rebase field");
    Handle(V3Frame("lesson_cinematic_control", 5,
        "{\"command\":\"resume\",\"phaseId\":\"opening\",\"commandSequenceId\":44}"));
    require(FrameType(4) == "lesson_ack",
            "v3 resume retains its original three-field control contract");

    Handle(V3Frame("lesson_cinematic_control", 6,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":45,"
        "\"reason\":\"testCleanup\"}"));
    require(FrameType(5) == "lesson_error",
            "v3 cancel rejects the v4-only reason field");
    Handle(V3Frame("lesson_cinematic_control", 7,
        "{\"command\":\"cancel\",\"phaseId\":\"opening\",\"commandSequenceId\":45}"));
    require(FrameType(6) == "lesson_ack",
            "v3 cancel retains its original three-field control contract");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_renderer_v3_rejections_use_task7_consumed_lesson_error() {
    ResetObservable();
    FreshSession();
    tbot::SetActiveLessonCinematicRenderer(nullptr);
    Handle(V3PrepareFrame(1));
    require(FrameType(0) == "lesson_error",
            "rejected cinematic prepare uses lesson_error instead of an unmatchable ACK");
    require(FrameBodyStr(0, nullptr, "code") == "CINEMATIC_CAPABILITY_UNSUPPORTED",
            "cinematic lesson_error preserves the typed failure code");

    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonCinematicRenderer renderer({&fake, V3Allocate, V3Free, V3Open, V3Close,
                                             V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&renderer);
    std::string invalid_layers = V3PrepareFrame(1);
    const std::string expected = "\"layer\":\"background\",\"slot\":\"backgroundScene\"";
    const auto position = invalid_layers.find(expected);
    require(position != std::string::npos, "v3 fixture contains background identity");
    invalid_layers.replace(position, expected.size(),
                           "\"layer\":\"teachingObject\",\"slot\":\"backgroundScene\"");
    Handle(invalid_layers);
    require(FrameType(0) == "lesson_error" && fake.opens == 0,
            "wrong layer identity/order fails before renderer prepare");
    require(FrameBodyStr(0, nullptr, "code") == "CINEMATIC_METADATA_MISMATCH",
            "wrong layer identity reports typed metadata failure");

    ResetObservable();
    FreshSession();
    Handle(V3Frame("lesson_start", 1,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":42}}"));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_SESSION_MISMATCH",
            "unprepared cinematic start reports the scoped session error");

    ResetObservable();
    FreshSession();
    Handle(V3Frame("lesson_cinematic_control", 1,
        "{\"command\":\"pause\",\"phaseId\":\"opening\",\"commandSequenceId\":43}"));
    require(FrameType(0) == "lesson_error" &&
                FrameBodyStr(0, nullptr, "code") == "CINEMATIC_SESSION_MISMATCH",
            "unprepared cinematic control reports the scoped session error");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_renderer_v3_duplicate_sequence_requires_exact_original_command() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonCinematicRenderer renderer({&fake, V3Allocate, V3Free, V3Open, V3Close,
                                             V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&renderer);
    Handle(V3PrepareFrame(1));
    require(FrameType(0) == "lesson_ack", "baseline v3 prepare is accepted");

    std::string changed_phase = V3PrepareFrame(2);
    const auto phase = changed_phase.find("\"phaseId\":\"opening\"");
    changed_phase.replace(phase, strlen("\"phaseId\":\"opening\""),
                          "\"phaseId\":\"teach\"");
    Handle(changed_phase);
    require(FrameType(1) == "lesson_error" &&
                FrameBodyStr(1, nullptr, "code") == "CINEMATIC_STALE_COMMAND",
            "same sequence with changed phase is rejected, not replayed under current payload");

    std::string changed_layers = V3PrepareFrame(3);
    const auto rect = changed_layers.find("\"x\":10,\"y\":10");
    changed_layers.replace(rect, strlen("\"x\":10,\"y\":10"), "\"x\":11,\"y\":10");
    Handle(changed_layers);
    require(FrameType(2) == "lesson_error" &&
                FrameBodyStr(2, nullptr, "code") == "CINEMATIC_STALE_COMMAND",
            "same prepare sequence with changed layers is rejected");

    Handle(V3Frame("lesson_start", 4,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":41}}"));
    require(FrameType(3) == "lesson_error" &&
                FrameBodyStr(3, nullptr, "code") == "CINEMATIC_STALE_COMMAND",
            "same sequence reused by a different command is rejected");
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_renderer_v3_json_safe_sequence_boundaries_and_ack_oom() {
    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonCinematicRenderer renderer({&fake, V3Allocate, V3Free, V3Open, V3Close,
                                             V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&renderer);
    constexpr std::uint64_t kMaxJsonSafeInteger = 9007199254740991ULL;
    Handle(V3PrepareFrame(1, kMaxJsonSafeInteger));
    require(FrameType(0) == "lesson_ack",
            "maximum JSON-safe commandSequenceId is accepted");

    renderer.Cancel(kMaxJsonSafeInteger + 1, "opening");
    ResetObservable();
    FreshSession();
    Handle(V3PrepareFrame(1, kMaxJsonSafeInteger + 1));
    require(FrameType(0) == "lesson_error" && fake.opens == 3,
            "commandSequenceId above JSON-safe range is rejected before renderer work");

    tbot::SetActiveLessonCinematicRenderer(nullptr);
    V3RendererFake oom_fake;
    tbot::LessonCinematicRenderer oom_renderer(
        {&oom_fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present});
    tbot::SetActiveLessonCinematicRenderer(&oom_renderer);
    ResetObservable();
    FreshSession();
    for (int fail_after = 0; fail_after <= 9; ++fail_after) {
        tbot::SetLessonCinematicJsonFailAfterForTest(fail_after);
        Handle(V3PrepareFrame(fail_after + 1, 51 + fail_after));
        require(Sent().empty(),
                "frameZeroReady ACK cJSON OOM at every construction step sends no partial ACK");
    }
    ResetObservable();
    tbot::SetLessonCinematicJsonFailAfterForTest(0);
    Handle(V3Frame("lesson_start", 11,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"opening\","
        "\"commandSequenceId\":61}}"));
    require(Sent().empty(), "phaseReady ACK cJSON OOM drops instead of sending a partial ACK");
    ResetObservable();
    tbot::SetLessonCinematicJsonFailAfterForTest(0);
    Handle(V3Frame("lesson_cinematic_control", 12,
        "{\"command\":\"pause\",\"phaseId\":\"opening\",\"commandSequenceId\":62}"));
    require(Sent().empty(), "commandApplied ACK cJSON OOM drops instead of sending a partial ACK");
    tbot::SetLessonCinematicJsonFailAfterForTest(-1);
    tbot::SetActiveLessonCinematicRenderer(nullptr);
}

void test_generic_lesson_json_failures_drop_partial_frames_and_clean_up() {
    ResetObservable();
    FreshSession();
    tbot::SetLessonJsonFailAfterForTest(0);
    Handle(PrepareFrame(1));
    require(Sent().empty(), "v1 BuildFrame root allocation failure sends no partial ACK");

    ResetObservable();
    FreshSession();
    tbot::SetLessonJsonFailAfterForTest(8);
    Handle(PrepareFrame(1));
    require(Sent().empty(), "v1 BuildFrame body attach failure sends no partial ACK");

    ResetObservable();
    FreshSession();
    tbot::SetLessonJsonFailAfterForTest(9);
    Handle(V2PrepareFrame(1));
    require(Sent().empty(), "v2 BuildFrame serialization failure sends no partial ACK");

    for (int fail_after = 0; fail_after <= 6; ++fail_after) {
        tbot::SetLessonJsonFailAfterForTest(-1);
        ResetObservable();
        FreshSession();
        Board::GetInstance().display_ = nullptr;
        Handle(V2PrepareFrame(1));
        const size_t frames_before_error = Sent().size();
        tbot::SetLessonJsonFailAfterForTest(fail_after);
        Handle(V2StartFrame(2,
            "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"everyStep\"}"));
        require(Sent().size() == frames_before_error,
                "MakeErrorBody cJSON failure sends no partial error frame");
    }

    for (int fail_after = 0; fail_after <= 18; ++fail_after) {
        tbot::SetLessonJsonFailAfterForTest(-1);
        ResetObservable();
        FreshSession();
        Board::GetInstance().display_ = nullptr;
        SetLessonTransportEpoch(71 + fail_after);
        Handle(V2PrepareFrame(1));
        Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
        std::string ack;
        require(AcceptLessonVisualCompletion(App().lesson_visual_queue.back(), &ack),
                "visual failpoint fixture completes opening entrance first");
        App().lesson_visual_queue.clear();
        std::string visual = V2VisualFrame(3, "thinking", 17);
        visual = ReplaceOnce(visual, "\"stepId\":\"s2\"",
                             "\"lessonId\":\"L1\",\"lessonVersion\":3,\"stepId\":\"s2\"");
        Handle(visual);
        const LessonQueueItem completion = App().lesson_visual_queue.back();
        ack.clear();
        tbot::SetLessonJsonFailAfterForTest(fail_after);
        require(!AcceptLessonVisualCompletion(completion, &ack) && ack.empty(),
                "visual completion cJSON failure sends no partial ACK");

        tbot::SetLessonJsonFailAfterForTest(-1);
        require(AcceptLessonVisualCompletion(completion, &ack),
                "visual completion session remains recoverable after cJSON failure");
        cJSON* recovered = cJSON_Parse(ack.c_str());
        require(recovered != nullptr &&
                    cJSON_GetObjectItem(recovered, "sequence")->valueint == 3,
                "failed visual ACK construction does not consume the outbound sequence");
        cJSON_Delete(recovered);
    }
    tbot::SetLessonJsonFailAfterForTest(-1);
}

void test_buildframe_failure_does_not_consume_outbound_sequence() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    tbot::SetLessonJsonFailAfterForTest(0);
    Handle(PrepareFrame(1));
    require(Sent().empty(), "failed v1 prepare ACK sends no frame");
    tbot::SetLessonJsonFailAfterForTest(-1);
    Handle(StartFrame(2));
    require(Sent().size() == 1 && FrameSeq(0) == 1,
            "v1 ACK recovery reuses the unsent outbound sequence");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    SetLessonTransportEpoch(101);
    tbot::SetLessonJsonFailAfterForTest(0);
    Handle(V2PrepareFrame(1));
    require(Sent().empty(), "failed v2 prepare ACK sends no frame");
    tbot::SetLessonJsonFailAfterForTest(-1);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    std::string completion_ack;
    require(AcceptLessonVisualCompletion(App().lesson_visual_queue.back(), &completion_ack),
            "v2 session recovers after the unsent prepare ACK");
    cJSON* completion = cJSON_Parse(completion_ack.c_str());
    require(completion != nullptr &&
                cJSON_GetObjectItem(completion, "sequence")->valueint == 1,
            "v2 ACK recovery reuses the unsent outbound sequence");
    cJSON_Delete(completion);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    const size_t frames_before_failed_error = Sent().size();
    tbot::SetLessonJsonFailAfterForTest(7);
    Handle(V2StartFrame(2,
        "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"everyStep\"}"));
    require(Sent().size() == frames_before_failed_error,
            "failed ordinary error envelope sends no partial frame");
    tbot::SetLessonJsonFailAfterForTest(-1);
    Handle(V2StartFrame(3,
        "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"everyStep\"}"));
    require(Sent().size() == frames_before_failed_error + 1 &&
                FrameType(Sent().size() - 1) == "lesson_error" &&
                FrameSeq(Sent().size() - 1) == 2,
            "ordinary error recovery reuses the unsent outbound sequence");
    tbot::SetLessonJsonFailAfterForTest(-1);
}

void test_buildframe_missing_required_protocol_sends_no_partial_error() {
    ResetObservable();
    FreshSession();
    Handle(std::string("{\"type\":\"lesson_prepare\",\"assignmentId\":\"") + AID() +
           "\",\"sessionId\":\"" + SID() +
           "\",\"sequence\":1,\"body\":{\"profile\":\"espTft\"}}");
    require(Sent().empty(),
            "BuildFrame drops an error envelope that cannot echo required protocolVersion");
}

void test_renderer_v2_valid_opening_entrance_contract_acks_center_road() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(display.entrance_start_calls == 1 &&
                display.last_entrance_layout == "centerRoad",
            "valid renderer-v2 opening entrance reaches the LVGL center-road entrance");
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 2 &&
                FrameBodyBool(Sent().size() - 1, "accepted", false),
            "valid renderer-v2 opening entrance completion emits the correlated ACK");
}

void test_renderer_v2_valid_visual_state_contract_enqueues_static_completion() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();

    Handle(V2VisualFrameWithStepId(3, "valid-step", 2));
    require(display.visual_state_calls == 1 &&
                display.last_emotion == "thinking" &&
                display.last_status == "Đang suy nghĩ...",
            "valid renderer-v2 visual state contract reaches static LVGL state");
    require(App().lesson_visual_queue.empty(),
            "valid renderer-v2 visual state waits for display completion callback");
    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameStepId(Sent().size() - 1) == "valid-step" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 3 &&
                FrameBodyNum(Sent().size() - 1, "visualGeneration") == 2 &&
                FrameBodyBool(Sent().size() - 1, "accepted", false),
            "valid renderer-v2 visual state completion emits the correlated ACK");
}

void test_renderer_v2_visual_motion_is_allowlisted_once_per_generation() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();
    App().robot_uart_.calls.clear();

    Handle(V2VisualFrameWithStepId(3, "valid-step", 2));
    const std::vector<std::string> encourage_calls = {
        "both_arms_raise", "head_center"};
    require(App().robot_uart_.calls.empty(),
            "renderer-v2 motion waits for successful visual application");
    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(App().robot_uart_.calls == encourage_calls,
            "renderer-v2 visual motion executes through the fixed preset dispatcher");
    require(!FrameBodyBool(Sent().size() - 1, "degraded", true),
            "applied renderer-v2 visual motion keeps the visual ACK green");

    Handle(V2VisualFrameWithStepId(4, "valid-step", 2));
    require(App().robot_uart_.calls == encourage_calls,
            "a retried visual generation does not execute motion twice");
    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();

    Handle(V2VisualFrameWithStepId(5, "valid-step", 1));
    require(App().robot_uart_.calls == encourage_calls,
            "an older visual generation never repeats renderer-v2 motion");
    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(FrameBodyBool(Sent().size() - 1, "degraded", false) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "reducedMotion",
            "an older visual generation is acknowledged as reduced motion");

    Handle(V2VisualFrameWithStepId(6, "valid-step", 2));
    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(App().robot_uart_.calls == encourage_calls &&
                !FrameBodyBool(Sent().size() - 1, "degraded", true),
            "a stale generation cannot poison the applied generation ACK");

    std::string unknown = V2VisualFrameWithStepId(7, "valid-step", 3);
    const auto preset = unknown.find("encourage");
    require(preset != std::string::npos, "visual fixture contains the authored preset");
    unknown.replace(preset, std::strlen("encourage"), "rawSweep");
    Handle(unknown);
    require(App().robot_uart_.calls == encourage_calls,
            "unknown renderer-v2 motion never reaches raw robot commands");
    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(FrameBodyBool(Sent().size() - 1, "degraded", false) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "reducedMotion",
            "unknown renderer-v2 motion is visible as a degraded visual ACK");

    require(DispatchLessonMotionPreset(App().robot_uart_, "rest") ==
                LessonMotionResult::kApplied,
            "renderer-v2 visual motion test leaves no pending auto-rest timer");
}

void test_renderer_v2_contracts_reject_unexpected_and_duplicate_keys() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2,
        "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"oncePerLessonSession\","
        "\"layoutPreset\":\"centerRoad\",\"backgroundAssetKey\":\"scene.farm\","
        "\"robotAssetKey\":\"robotOverlay.teach\",\"fallback\":\"staticGreet\","
        "\"unexpected\":true}"));
    require(FrameType(Sent().size() - 1) == "lesson_error" &&
                FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "renderer-v2 opening entrance rejects unexpected contract keys");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2,
        "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"oncePerLessonSession\","
        "\"layoutPreset\":\"centerRoad\",\"backgroundAssetKey\":\"scene.farm\","
        "\"robotAssetKey\":\"robotOverlay.teach\",\"fallback\":\"staticGreet\","
        "\"policy\":\"oncePerLessonSession\"}"));
    require(FrameType(Sent().size() - 1) == "lesson_error" &&
                FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "renderer-v2 opening entrance rejects duplicate accepted contract keys");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    Handle(std::string("{\"type\":\"lesson_visual_state\",\"protocolVersion\":\"") +
           kLessonRendererV2 + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" +
           SID() + "\",\"stepId\":\"valid-step\",\"sequence\":3,"
           "\"body\":{\"state\":\"thinking\",\"overlayKey\":\"thinking\","
           "\"motionPreset\":\"encourage\",\"visualGeneration\":2,"
           "\"overlayKey\":\"thinking\"}}");
    require(FrameType(Sent().size() - 1) == "lesson_error" &&
                FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "renderer-v2 visual state rejects duplicate accepted contract keys");
}

void test_renderer_v2_start_and_visual_contracts_fail_closed() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    require(FrameType(0) == "lesson_ack", "valid v2 prepare is accepted");

    Handle(V2StartFrame(2,
        "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"everyStep\","
        "\"layoutPreset\":\"centerRoad\",\"backgroundAssetKey\":\"scene.farm\","
        "\"robotAssetKey\":\"robotOverlay.teach\",\"fallback\":\"staticGreet\"}"));
    require(FrameType(Sent().size() - 1) == "lesson_error",
            "malformed v2 opening entrance fails closed");
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "malformed v2 opening entrance reports a stable contract error");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(7);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(Sent().size() == 1,
            "valid v2 opening entrance does not ACK before asynchronous completion");
    const std::uint64_t start_nonce = App().lesson_visual_queue.back().visual_nonce;
    LessonQueueItem completion = MakeLessonVisualQueueItem(
        LessonQueueItemKind::kVisualCompleted, 6, 1, 2, AID(), SID(), nullptr,
        LessonVisualCompletionResult::kApplied, nullptr, start_nonce);
    std::string completion_ack;
    require(!AcceptLessonVisualCompletion(completion, &completion_ack) && completion_ack.empty(),
            "completion from a stale transport epoch is rejected");
    completion.transport_epoch = 7;
    completion.visual_generation = 2;
    require(!AcceptLessonVisualCompletion(completion, &completion_ack) && completion_ack.empty(),
            "completion from a stale visual generation is rejected");
    completion.visual_generation = 1;
    require(AcceptLessonVisualCompletion(completion, &completion_ack),
            "matching epoch, generation, sequence, and session identity emits an ACK");
    cJSON* ack = cJSON_Parse(completion_ack.c_str());
    require(ack != nullptr, "visual completion ACK parses");
    cJSON* ack_body = cJSON_GetObjectItem(ack, "body");
    require(std::string(cJSON_GetObjectItem(ack, "protocolVersion")->valuestring) == kLessonRendererV2 &&
                std::string(cJSON_GetObjectItem(ack, "assignmentId")->valuestring) == AID() &&
                std::string(cJSON_GetObjectItem(ack, "sessionId")->valuestring) == SID(),
            "visual completion ACK preserves frozen session identity");
    require(cJSON_GetObjectItem(ack_body, "acks")->valueint == 2 &&
                cJSON_IsTrue(cJSON_GetObjectItem(ack_body, "accepted")) &&
                cJSON_IsFalse(cJSON_GetObjectItem(ack_body, "degraded")) &&
                cJSON_IsNull(cJSON_GetObjectItem(ack_body, "degradedReason")) &&
                cJSON_GetObjectItem(ack_body, "visualGeneration")->valueint == 1,
            "applied visual emits the frozen positive ACK body");
    cJSON_Delete(ack);
    std::string duplicate_ack;
    require(!AcceptLessonVisualCompletion(completion, &duplicate_ack) && duplicate_ack.empty(),
            "duplicate completion emits no second ACK");

    Handle(V2VisualFrame(3, "thinking", 17));
    const std::uint64_t timeout_nonce = App().lesson_visual_queue.back().visual_nonce;
    LessonQueueItem timeout = MakeLessonVisualQueueItem(
        LessonQueueItemKind::kVisualTimedOut, 7, 17, 3, AID(), SID(), "s2",
        LessonVisualCompletionResult::kPhaseTimeout, nullptr, timeout_nonce);
    require(DispatchLessonVisualCompletion(timeout, App().protocol_.get()),
            "worker dispatch sends the matching timeout ACK");
    ack = cJSON_Parse(Sent().back().c_str());
    ack_body = cJSON_GetObjectItem(ack, "body");
    require(cJSON_IsTrue(cJSON_GetObjectItem(ack_body, "accepted")) &&
                cJSON_IsTrue(cJSON_GetObjectItem(ack_body, "degraded")) &&
                std::string(cJSON_GetObjectItem(ack_body, "degradedReason")->valuestring) == "phaseTimeout",
            "timeout ACK reports accepted degraded fallback semantics");
    cJSON_Delete(ack);

    Handle(V2VisualFrame(4, "incorrect", 18));
    const std::uint64_t rejected_nonce = App().lesson_visual_queue.back().visual_nonce;
    LessonQueueItem rejected = MakeLessonVisualQueueItem(
        LessonQueueItemKind::kVisualCompleted, 7, 18, 4, AID(), SID(), "s2",
        LessonVisualCompletionResult::kRejected, "randomReason", rejected_nonce);
    require(DispatchLessonVisualCompletion(rejected, App().protocol_.get()),
            "worker dispatch sends the matching negative visual ACK");
    ack = cJSON_Parse(Sent().back().c_str());
    ack_body = cJSON_GetObjectItem(ack, "body");
    require(cJSON_IsFalse(cJSON_GetObjectItem(ack_body, "accepted")) &&
                cJSON_IsFalse(cJSON_GetObjectItem(ack_body, "degraded")) &&
                std::string(cJSON_GetObjectItem(ack_body, "degradedReason")->valuestring) ==
                    "unsupportedContract",
            "rejected visual emits frozen negative ACK semantics");
    cJSON_Delete(ack);

    Handle(V2VisualFrame(5, "guessedLocally", 19));
    require(FrameType(Sent().size() - 1) == "lesson_error",
            "malformed v2 visual state fails closed");
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "malformed v2 visual state reports a stable contract error");
}

void test_renderer_v2_opening_layouts_and_visual_generation_contracts_fail_closed() {
    const char* layouts[] = {"leftApproach", "rightApproach"};
    for (const char* layout : layouts) {
        ResetObservable();
        FreshSession();
        LvglDisplay display;
        Board::GetInstance().display_ = &display;
        Handle(V2PrepareFrame(1));
        SetLessonTransportEpoch(41);
        Handle(V2StartFrame(2, ValidV2OpeningEntranceForLayout(layout)));
        require(display.entrance_start_calls == 1 &&
                    display.last_entrance_layout == layout,
                "renderer-v2 alternate opening layout is delegated to LVGL");
        display.CompleteEntrance();
        App().DrainLessonVisualQueue();
        require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                    FrameBodyNum(Sent().size() - 1, "acks") == 2 &&
                    FrameBodyBool(Sent().size() - 1, "accepted", false),
                "renderer-v2 alternate opening layout emits a correlated ACK");
    }

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2,
        "{\"preset\":\"flyLandWalkGreet\",\"policy\":\"oncePerLessonSession\","
        "\"layoutPreset\":\"leftApproach\",\"backgroundAssetKey\":\"scene.farm\","
        "\"robotAssetKey\":\"robotOverlay.teach\",\"fallback\":\"spinInPlace\"}"));
    require(FrameType(Sent().size() - 1) == "lesson_error",
            "renderer-v2 opening entrance with invalid fallback fails closed");
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "renderer-v2 opening entrance final contract failure reports a stable error");

    const char* invalid_generations[] = {"0", "1.5"};
    for (const char* generation : invalid_generations) {
        ResetObservable();
        FreshSession();
        LvglDisplay display;
        Board::GetInstance().display_ = &display;
        Handle(V2PrepareFrame(1));
        Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
        display.CompleteEntrance();
        App().DrainLessonVisualQueue();
        Handle(V2VisualFrameWithGeneration(3, "thinking", generation));
        require(FrameType(Sent().size() - 1) == "lesson_error",
                "invalid renderer-v2 visualGeneration fails closed");
        require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
                "invalid renderer-v2 visualGeneration reports a stable contract error");
    }

    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();
    Handle(V2VisualFrameWithStepId(3, "", 2));
    require(FrameType(Sent().size() - 1) == "lesson_error",
            "renderer-v2 visual state with invalid step identity fails closed");
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_FRAME_INVALID",
            "renderer-v2 visual state final contract failure reports a stable error");
}

void test_renderer_v2_worker_dispatch_emits_exactly_one_ack() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(23);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    const size_t before_completion = Sent().size();
    const std::uint64_t completion_nonce = App().lesson_visual_queue.back().visual_nonce;
    LessonQueueItem completion = MakeLessonVisualQueueItem(
        LessonQueueItemKind::kVisualCompleted, 23, 1, 2, AID(), SID(), nullptr,
        LessonVisualCompletionResult::kApplied, nullptr, completion_nonce);
    require(DispatchLessonVisualCompletion(completion, App().protocol_.get()),
            "production worker dispatch accepts current callback identity");
    require(Sent().size() == before_completion + 1 &&
                FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 2 &&
                FrameBodyBool(Sent().size() - 1, "accepted", false),
            "production worker dispatch sends the correlated v2 ACK");
    require(!DispatchLessonVisualCompletion(completion, App().protocol_.get()) &&
                Sent().size() == before_completion + 1,
            "duplicate production callback emits no second ACK");
}

void test_renderer_v2_production_render_callback_reaches_worker_ack() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(31);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(display.entrance_start_calls == 1 &&
                display.last_entrance_layout == "centerRoad",
            "production v2 start delegates the reviewed entrance plan to LVGL");
    require(App().lesson_visual_queue.empty(),
            "production v2 start cannot complete before the reveal callback");
    display.CompleteEntrance();
    require(App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().kind == LessonQueueItemKind::kVisualCompleted,
            "production reveal callback enqueues typed completion after animation");
    const LessonQueueItem completed_callback = App().lesson_visual_queue.front();
    const size_t before_drain = Sent().size();
    App().DrainLessonVisualQueue();
    require(Sent().size() == before_drain + 1 &&
                FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyBool(Sent().size() - 1, "accepted", false),
            "production callback crosses worker dispatch and emits the v2 ACK");

    App().lesson_visual_queue.push_back(completed_callback);
    App().DrainLessonVisualQueue();
    require(Sent().size() == before_drain + 1,
            "duplicate production callback remains an idempotent no-op");

    Handle(V2VisualFrame(3, "thinking", 17));
    display.CompleteVisualState(LessonVisualApplyResult::kRejected, "randomReason");
    require(App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().completion_result ==
                    LessonVisualCompletionResult::kRejected &&
                std::string(App().lesson_visual_queue.front().degraded_reason) ==
                    "randomReason",
            "production rejected visual callback queues the typed rejection result");
    const size_t before_rejected_drain = Sent().size();
    App().DrainLessonVisualQueue();
    require(Sent().size() == before_rejected_drain + 1 &&
                FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameStepId(Sent().size() - 1) == "s2" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 3 &&
                FrameBodyNum(Sent().size() - 1, "visualGeneration") == 17 &&
                !FrameBodyBool(Sent().size() - 1, "accepted", true) &&
                !FrameBodyBool(Sent().size() - 1, "degraded", true) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "unsupportedContract",
            "production rejected visual callback drains to an exact negative ACK");

    Handle(V2VisualFrame(4, "thinking", 18));
    display.CompleteVisualState(LessonVisualApplyResult::kPhaseTimeout, nullptr);
    require(App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().completion_result ==
                    LessonVisualCompletionResult::kPhaseTimeout &&
                App().lesson_visual_queue.front().kind == LessonQueueItemKind::kVisualTimedOut,
            "production phase-timeout visual callback queues the typed timeout result");
    const size_t before_timeout_drain = Sent().size();
    App().DrainLessonVisualQueue();
    require(Sent().size() == before_timeout_drain + 1 &&
                FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameStepId(Sent().size() - 1) == "s2" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 4 &&
                FrameBodyNum(Sent().size() - 1, "visualGeneration") == 18 &&
                FrameBodyBool(Sent().size() - 1, "accepted", false) &&
                FrameBodyBool(Sent().size() - 1, "degraded", false) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "phaseTimeout",
            "production phase-timeout visual callback drains to an exact degraded ACK");

    struct VisualExpectation {
        const char* state;
        const char* emotion;
        const char* status;
    };
    const VisualExpectation expectations[] = {
        {"teach", "happy", "Đang học..."},
        {"listen", "thinking", "Con nói nhé..."},
        {"thinking", "thinking", "Đang suy nghĩ..."},
        {"correct", "happy", "Chính xác!"},
        {"nearMiss", "neutral", "Gần đúng rồi!"},
        {"incorrect", "sad", "Chưa đúng"},
        {"retry", "thinking", "Thử lại nhé!"},
        {"celebrate", "happy", "Tuyệt vời!"},
        {"completion", "happy", "Hoàn thành bài học"},
    };
    int visual_sequence = 5;
    std::uint64_t visual_generation = 19;
    for (const auto& expectation : expectations) {
        Handle(V2VisualFrame(visual_sequence, expectation.state, visual_generation));
        require(display.last_emotion == expectation.emotion &&
                    display.last_status == expectation.status,
                "valid visual state installs its child-visible static display state");
        require(display.visual_state_calls == visual_sequence - 2,
                "valid visual state delegates static arrived-state transforms to LVGL");
        require(App().lesson_visual_queue.empty(),
                "static visual state waits for its LVGL completion callback");
        display.CompleteVisualState(LessonVisualApplyResult::kDegraded, "missingOverlay");
        require(App().lesson_visual_queue.size() == 1 &&
                    App().lesson_visual_queue.front().completion_result ==
                        LessonVisualCompletionResult::kDegraded &&
                    std::string(App().lesson_visual_queue.front().degraded_reason) ==
                        "missingOverlay",
                "static visual install queues a truthful degraded completion after display calls");
        App().DrainLessonVisualQueue();
        require(FrameBodyBool(Sent().size() - 1, "accepted", false) &&
                    FrameBodyBool(Sent().size() - 1, "degraded", false) &&
                    FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                        "missingOverlay",
                "valid visual state crosses the production worker path with a positive ACK");
        ++visual_sequence;
        ++visual_generation;
    }

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(32);
    App().defer_scheduled_callbacks = true;
    const int before_deferred_start_calls = display.entrance_start_calls;
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(App().lesson_visual_queue.empty(),
            "deferred display install cannot complete before its Application callback runs");
    Handle(V2PauseFrame(3));
    require(display.entrance_cancel_calls > 0,
            "pause cancels the active entrance before its control ACK can win");
    const size_t after_pause = Sent().size();
    App().FlushScheduledCallbacks();
    require(display.entrance_start_calls == before_deferred_start_calls,
            "stale deferred start callback cannot revive the entrance after pause");
    App().DrainLessonVisualQueue();
    require(Sent().size() == after_pause,
            "late display callback after pause is generation-gated and emits no ACK");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(33);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().completion_result ==
                    LessonVisualCompletionResult::kRejected,
            "missing production display enqueues a typed rejection");
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                !FrameBodyBool(Sent().size() - 1, "accepted", true) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "unsupportedContract",
            "production rejection crosses the worker and emits frozen negative ACK semantics");
}

void test_renderer_v2_duplicate_visual_waits_for_original_completion() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(34);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();

    const std::string visual = V2VisualFrame(3, "thinking", 17);
    Handle(visual);
    const int visual_calls = display.visual_state_calls;
    const int schedule_calls = App().schedule_calls;
    const size_t frames_before_duplicate = Sent().size();
    require(App().lesson_visual_queue.empty(),
            "visual ACK remains pending while its LVGL callback is incomplete");

    Handle(visual);
    require(display.visual_state_calls == visual_calls,
            "pending duplicate visual does not render a second time");
    require(App().schedule_calls == schedule_calls,
            "pending duplicate visual does not schedule a second display install");
    require(Sent().size() == frames_before_duplicate,
            "pending duplicate visual emits no premature ACK");

    display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(Sent().size() == frames_before_duplicate + 1 &&
                FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameStepId(Sent().size() - 1) == "s2" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 3 &&
                FrameBodyNum(Sent().size() - 1, "visualGeneration") == 17 &&
                FrameBodyBool(Sent().size() - 1, "accepted", false),
            "original visual completion emits exactly its correlated ACK");

    for (int sequence = 4; sequence <= 20; ++sequence) {
        Handle(V2VisualFrame(sequence, "thinking", sequence + 14));
        display.CompleteVisualState(LessonVisualApplyResult::kApplied, nullptr);
        App().DrainLessonVisualQueue();
    }
    require(FrameBodyNum(Sent().size() - 1, "acks") == 20,
            "completed visual ACKs advance beyond the bounded replay window");
}

void test_renderer_v2_visual_completion_nonce_wrap_skips_zero() {
    tbot::SetLessonVisualCompletionNonceForTest(UINT64_MAX);
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    App().DrainLessonVisualQueue();
    tbot::SetLessonVisualCompletionNonceForTest(UINT64_MAX);
    Handle(V2VisualFrame(3, "thinking", 2));
    require(!App().lesson_visual_queue.empty() &&
                App().lesson_visual_queue.back().visual_nonce != 0,
            "visual completion nonce skips zero after wraparound");
    tbot::SetLessonVisualCompletionNonceForTest(0);
}

void test_renderer_v2_invalid_visual_completion_result_rejects_fail_closed() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();

    Handle(V2VisualFrame(3, "thinking", 17));
    display.CompleteVisualState(static_cast<LessonVisualApplyResult>(0xff), "randomReason");
    require(App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().kind == LessonQueueItemKind::kVisualCompleted &&
                App().lesson_visual_queue.front().completion_result ==
                    LessonVisualCompletionResult::kRejected,
            "unknown visual callback result is queued as a fail-closed rejection");
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 3 &&
                !FrameBodyBool(Sent().size() - 1, "accepted", true) &&
                !FrameBodyBool(Sent().size() - 1, "degraded", true) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "unsupportedContract",
            "unknown visual callback result emits the stable rejected ACK mapping");
}

void test_renderer_v2_non_lvgl_display_rejects_start_completion() {
    ResetObservable();
    FreshSession();
    NoDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(34);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().completion_result ==
                    LessonVisualCompletionResult::kRejected,
            "non-LVGL display enqueues a renderer-v2 start rejection");
    App().DrainLessonVisualQueue();
    require(Sent().size() == 2 && FrameBodyNum(1, "acks") == 2 &&
                !FrameBodyBool(1, "accepted", true) &&
                FrameBodyStr(1, nullptr, "degradedReason") == "unsupportedContract",
            "non-LVGL display emits a correlated unsupportedContract ACK");
}

void test_renderer_v2_start_ack_is_serialized_before_early_step_ack() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(35);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    const auto stale_entrance = display.pending_entrance_completion;

    Handle(V2StepFrame(3, "early-step"));
    require(Sent().size() == 3 && FrameBodyNum(1, "acks") == 2 &&
                !FrameBodyBool(1, "accepted", true) &&
                FrameBodyStr(1, nullptr, "degradedReason") == "superseded" &&
                FrameBodyNum(2, "acks") == 3,
            "early step receives ACK only after the pending start cancellation ACK");
    if (stale_entrance) stale_entrance(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(Sent().size() == 3,
            "superseded entrance completion cannot emit a late duplicate ACK");
}

void test_renderer_v2_start_ack_is_serialized_before_early_visual_ack() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(36);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    const auto stale_entrance = display.pending_entrance_completion;

    Handle(V2VisualFrame(3, "thinking", 17));
    require(Sent().size() == 2 && FrameBodyNum(1, "acks") == 2 &&
                FrameBodyStr(1, nullptr, "degradedReason") == "superseded" &&
                App().lesson_visual_queue.empty(),
            "early visual waits behind an explicit start cancellation ACK");
    display.CompleteVisualState();
    App().DrainLessonVisualQueue();
    require(Sent().size() == 3 && FrameBodyNum(2, "acks") == 3,
            "early visual completion emits the next ACK in inbound order");
    if (stale_entrance) stale_entrance(LessonVisualApplyResult::kApplied, nullptr);
    App().DrainLessonVisualQueue();
    require(Sent().size() == 3,
            "superseded entrance callback remains fenced after the visual ACK");
}

void test_renderer_v2_old_completion_cannot_claim_reused_identity() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    SetLessonTransportEpoch(34);
    Handle(V2PrepareFrame(1, ",\"assignmentVersion\":1"));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    require(App().lesson_visual_queue.size() == 1,
            "first start produces one queued completion");
    const LessonQueueItem old_completion = App().lesson_visual_queue.front();
    App().lesson_visual_queue.clear();

    Handle(V2PrepareFrame(1, ",\"assignmentVersion\":2"));
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    const size_t before_old_completion = Sent().size();
    require(!DispatchLessonVisualCompletion(old_completion, App().protocol_.get()) &&
                Sent().size() == before_old_completion,
            "queued completion from the reset session cannot claim reused wire identity");
    require(display.entrance_start_calls == 2,
            "old completion cannot replay or revive a third entrance");

    display.CompleteEntrance();
    require(App().lesson_visual_queue.size() == 1,
            "replacement start has its own completion");
    App().DrainLessonVisualQueue();
    require(Sent().size() == before_old_completion + 1 &&
                FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 2,
            "replacement completion alone emits the reused sequence ACK");
}

void test_renderer_v2_visual_outside_running_session_is_dropped() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    const size_t frames_before_visual = Sent().size();
    Handle(V2VisualFrame(2, "thinking", 17));
    require(Sent().size() == frames_before_visual && display.visual_state_calls == 0 &&
                App().lesson_visual_queue.empty(),
            "renderer-v2 visual outside a running session is dropped without display or ACK work");
}

void test_renderer_v2_repeated_start_acks_once_and_resume_restores_teach_state() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(41);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();
    const int entrance_calls = display.entrance_start_calls;

    Handle(V2StartFrame(3, ValidV2OpeningEntrance()));
    require(Sent().size() == 3 && FrameType(2) == "lesson_ack" &&
                FrameBodyNum(2, "acks") == 3 && display.entrance_start_calls == entrance_calls &&
                App().lesson_visual_queue.empty(),
            "repeated renderer-v2 start emits an immediate correlated ACK without replaying entrance");

    Handle(V2PauseFrame(4));
    const int visual_calls_after_pause = display.visual_state_calls;
    Handle(V2ResumeFrame(5));
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 5 &&
                display.visual_state_calls == visual_calls_after_pause + 1 &&
                display.last_visual_state == LessonVisualStateKind::kTeach &&
                !display.lesson_mode_calls.empty() && display.lesson_mode_calls.back(),
            "renderer-v2 resume restores the static teach state and lesson mode before ACK");
}

void test_renderer_v2_verified_opening_assets_and_identity_mismatch() {
    const char* fixture_root = "/tmp/tbot-v2-opening-assets";
    require(system("mkdir -p /tmp/tbot-v2-opening-assets") == 0,
            "renderer-v2 opening asset directory is staged");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", fixture_root, 1);
    const std::vector<unsigned char> jpeg = JpegBody();
    for (const char* name : {"scene.farm", "robotOverlay.teach"}) {
        const std::string path = std::string(fixture_root) + "/" + name;
        FILE* file = fopen(path.c_str(), "wb");
        require(file != nullptr, "renderer-v2 opening JPEG fixture opens");
        require(fwrite(jpeg.data(), 1, jpeg.size(), file) == jpeg.size(),
                "renderer-v2 opening JPEG fixture writes completely");
        fclose(file);
    }
    const std::string checksum = "abcdef1234567890";
    const std::string ready_assets =
        ",\"manifestRef\":{\"manifestChecksum\":\"" + checksum +
        "\"},\"assetPack\":{\"cacheKey\":\"opening-" + checksum +
        "\",\"assets\":["
        "{\"key\":\"scene.farm\",\"state\":\"READY\",\"checksumOk\":true,"
        "\"localPath\":\"sd://sdcard/tbot/lesson-assets/scene.farm\",\"size\":" +
        std::to_string(jpeg.size()) + "},"
        "{\"key\":\"robotOverlay.teach\",\"state\":\"READY\",\"checksumOk\":true,"
        "\"localPath\":\"sd://sdcard/tbot/lesson-assets/robotOverlay.teach\",\"size\":" +
        std::to_string(jpeg.size()) + "}]}";

    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    HostJpegDecodeMode() = 0;
    Handle(V2PrepareFrame(1, ready_assets));
    require(FrameAssetPackReady(0), "verified renderer-v2 opening asset pack is ready");
    SetLessonTransportEpoch(42);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(!display.background_calls.empty() && display.background_calls.back() &&
                !display.overlay_calls.empty() && display.overlay_calls.back() &&
                display.entrance_start_calls == 1 && App().lesson_visual_queue.empty(),
            "verified opening background and robot assets install before entrance begins");
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 2,
            "verified opening asset entrance completion emits its correlated ACK");

    const std::string background_only_assets =
        ",\"manifestRef\":{\"manifestChecksum\":\"" + checksum +
        "\"},\"assetPack\":{\"cacheKey\":\"opening-mismatch-" + checksum +
        "\",\"assets\":["
        "{\"key\":\"scene.farm\",\"state\":\"READY\",\"checksumOk\":true,"
        "\"localPath\":\"sd://sdcard/tbot/lesson-assets/scene.farm\",\"size\":" +
        std::to_string(jpeg.size()) + "}]}";
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1, background_only_assets));
    SetLessonTransportEpoch(43);
    const int entrance_calls_before_mismatch = display.entrance_start_calls;
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    require(App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().completion_result ==
                    LessonVisualCompletionResult::kRejected &&
                std::string(App().lesson_visual_queue.front().degraded_reason) ==
                    "assetIdentityMismatch" &&
                display.entrance_start_calls == entrance_calls_before_mismatch,
            "opening asset identity mismatch queues rejection without starting entrance");
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                !FrameBodyBool(Sent().size() - 1, "accepted", true) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                    "assetIdentityMismatch",
            "opening asset identity mismatch drains to the exact stable negative ACK");

    remove("/tmp/tbot-v2-opening-assets/scene.farm");
    remove("/tmp/tbot-v2-opening-assets/robotOverlay.teach");
    rmdir(fixture_root);
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_renderer_v2_stale_visual_callback_and_non_lvgl_degraded_completion() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(44);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();

    App().defer_scheduled_callbacks = true;
    const int visual_calls_before_deferred = display.visual_state_calls;
    const int emotion_calls_before_deferred = display.set_emotion_calls;
    Handle(V2VisualFrame(3, "thinking", 17));
    Handle(V2PauseFrame(4));
    const size_t frames_after_pause = Sent().size();
    App().FlushScheduledCallbacks();
    require(display.visual_state_calls == visual_calls_before_deferred + 1 &&
                display.set_emotion_calls == emotion_calls_before_deferred &&
                App().lesson_visual_queue.empty() && Sent().size() == frames_after_pause,
            "stale deferred visual callback returns before display mutation or late ACK");

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = &display;
    Handle(V2PrepareFrame(1));
    SetLessonTransportEpoch(45);
    Handle(V2StartFrame(2, ValidV2OpeningEntrance()));
    display.CompleteEntrance();
    App().DrainLessonVisualQueue();
    NoDisplay non_lvgl;
    Board::GetInstance().display_ = &non_lvgl;
    Handle(V2VisualFrame(3, "thinking", 18));
    require(non_lvgl.last_emotion == "thinking" && non_lvgl.last_status == "Đang suy nghĩ..." &&
                App().lesson_visual_queue.size() == 1 &&
                App().lesson_visual_queue.front().completion_result ==
                    LessonVisualCompletionResult::kDegraded &&
                std::string(App().lesson_visual_queue.front().degraded_reason) == "missingOverlay",
            "non-LVGL visual applies child-visible state and queues degraded completion");
    App().DrainLessonVisualQueue();
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameBodyNum(Sent().size() - 1, "acks") == 3 &&
                FrameBodyBool(Sent().size() - 1, "accepted", false) &&
                FrameBodyBool(Sent().size() - 1, "degraded", false) &&
                FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") == "missingOverlay",
            "non-LVGL degraded visual completion drains to the exact correlated ACK");
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

void test_prepare_assetpack_accepts_large_cinematic_without_raising_image_ram_cap() {
    const char* dir = "/tmp/tbot-host-sd-cinematic/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-cinematic && mkdir -p /tmp/tbot-host-sd-cinematic/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-cinematic/lesson-assets/cinematic.mp4";
    const size_t file_size = 3 * 1024 * 1024;
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "cinematic asset fixture opens");
    require(fseek(fp, static_cast<long>(file_size - 1), SEEK_SET) == 0,
            "cinematic asset fixture seeks");
    fputc('x', fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"cinematic\"}],"
                          "\"assetPack\":{\"cacheKey\":\"ck-video-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"cinematic\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"mediaType\":\"video/mp4\","
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/cinematic.mp4\",\"size\":" +
                          std::to_string(file_size) + "}]}"));
    require(FrameAssetPackReady(0) == true,
            "verified cinematic above image RAM cap is ready from SD");
    const char* oversized_path = "/tmp/tbot-host-sd-cinematic/lesson-assets/oversized.mp4";
    const size_t oversized_file_size = 4 * 1024 * 1024 + 1;
    fp = fopen(oversized_path, "wb");
    require(fp != nullptr, "oversized cinematic asset fixture opens");
    require(fseek(fp, static_cast<long>(oversized_file_size - 1), SEEK_SET) == 0,
            "oversized cinematic asset fixture seeks");
    fputc('x', fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"oversized\"}],"
                          "\"assetPack\":{\"cacheKey\":\"ck-video-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"oversized\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"mediaType\":\"video/mp4\","
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/oversized.mp4\",\"size\":" +
                          std::to_string(oversized_file_size) + "}]}"));
    require(FrameAssetPackReady(0) == false,
            "verified cinematic above the 4 MiB MP4 cap is not ready from SD");
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_accepts_exact_production_trgb_pack_and_rejects_oversize() {
    const char* dir = "/tmp/tbot-host-sd-trgb/lesson-assets";
    require(system("rm -rf /tmp/tbot-host-sd-trgb && mkdir -p /tmp/tbot-host-sd-trgb/lesson-assets") == 0,
            "TRGB asset directory is staged");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    auto TrgbBytes = [](size_t frames) {
        const size_t data_offset = (64 + frames * 16 + 511) & ~static_cast<size_t>(511);
        return data_offset + frames * 307200;
    };
    const size_t frame_counts[] = {95, 6, 13, 30};
    std::string assets;
    for (int index = 0; index < 19; ++index) {
        const size_t frames = frame_counts[index % 4];
        const size_t cue_bytes = TrgbBytes(frames);
        const std::string name = index == 0
            ? "barn-opening.trgb" : "cue-" + std::to_string(index) + ".trgb";
        const std::string path = std::string(dir) + "/" + name;
        FILE* fp = fopen(path.c_str(), "wb");
        require(fp != nullptr && fseek(fp, static_cast<long>(cue_bytes - 1), SEEK_SET) == 0,
                "mixed-duration sparse TRGB fixture opens");
        fputc('x', fp);
        fclose(fp);
        if (!assets.empty()) assets += ',';
        assets += "{\"key\":\"cue-" + std::to_string(index) +
            "\",\"state\":\"READY\",\"checksumOk\":true,"
            "\"mediaType\":\"application/vnd.tbot.rgb565-indexed\","
            "\"localPath\":\"sd://sdcard/tbot/lesson-assets/" + name +
            "\",\"size\":" + std::to_string(cue_bytes) + "}";
    }
    for (int index = 0; index < 8; ++index) {
        const std::string name = "static-" + std::to_string(index) + ".png";
        const std::string path = std::string(dir) + "/" + name;
        FILE* fp = fopen(path.c_str(), "wb");
        require(fp != nullptr && fwrite("static", 1, 6, fp) == 6,
                "static pack fixture writes");
        fclose(fp);
        assets += ",{\"key\":\"static-" + std::to_string(index) +
            "\",\"state\":\"READY\",\"checksumOk\":true,"
            "\"localPath\":\"sd://sdcard/tbot/lesson-assets/" + name +
            "\",\"size\":6}";
    }
    const std::string pack =
        ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
        "\"assetPack\":{\"cacheKey\":\"trgb-abcdef1234567890\",\"assets\":[" +
        assets + "]}";

    ResetObservable();
    FreshSession();
    V3RendererFake fake;
    tbot::LessonFlattenedCinematicRenderer renderer(
        tbot::LessonFlattenedCinematicRendererOps{
            {&fake, V3Allocate, V3Free, V3Open, V3Close, V3Decode, V3Present},
            V4Begin, V4Queue, V4Wait, V4End, nullptr, V4StreamBytes});
    tbot::SetActiveLessonFlattenedCinematicRenderer(&renderer);
    tbot::SetLessonFlattenedCinematicRendererCapabilityReady(true);
    Handle(V4TrgbPrepareFrame(1, 501, "", pack));
    require(FrameType(0) == "lesson_ack" && FrameAssetPackReady(0),
            "mixed 6/13/30/95-frame TRGB cues plus eight static assets pass independently");
    Handle(V4V2BarnCueCommandFrame("lesson_cinematic_control", "cancel", 2, 502,
                                   ",\"reason\":\"testCleanup\""));

    const char* oversized_name = "oversized.trgb";
    const std::string oversized_path = std::string(dir) + "/" + oversized_name;
    constexpr size_t kOversizedBytes = 64U * 1024U * 1024U + 1U;
    FILE* fp = fopen(oversized_path.c_str(), "wb");
    require(fp != nullptr && fseek(fp, static_cast<long>(kOversizedBytes - 1), SEEK_SET) == 0,
            "oversized sparse TRGB fixture opens");
    fputc('x', fp);
    fclose(fp);
    const std::string oversized_pack =
        ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
        "\"assetPack\":{\"cacheKey\":\"trgb-abcdef1234567890\",\"assets\":[{"
        "\"key\":\"oversized\",\"state\":\"READY\",\"checksumOk\":true,"
        "\"mediaType\":\"application/vnd.tbot.rgb565-indexed\","
        "\"localPath\":\"sd://sdcard/tbot/lesson-assets/oversized.trgb\",\"size\":" +
        std::to_string(kOversizedBytes) + "}]}";
    ResetObservable();
    FreshSession();
    Handle(V4TrgbPrepareFrame(3, 503, "", oversized_pack));
    require(FrameType(0) == "lesson_error" || !FrameAssetPackReady(0),
            "oversized or layout-inconsistent TRGB asset is rejected");

    const size_t unaligned_bytes = TrgbBytes(13) + 1;
    const std::string unaligned_path = std::string(dir) + "/unaligned.trgb";
    fp = fopen(unaligned_path.c_str(), "wb");
    require(fp != nullptr && fseek(fp, static_cast<long>(unaligned_bytes - 1), SEEK_SET) == 0,
            "unaligned sparse TRGB fixture opens");
    fputc('x', fp);
    fclose(fp);
    const std::string unaligned_pack =
        ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
        "\"assetPack\":{\"cacheKey\":\"trgb-abcdef1234567890\",\"assets\":[{"
        "\"key\":\"unaligned\",\"state\":\"READY\",\"checksumOk\":true,"
        "\"mediaType\":\"application/vnd.tbot.rgb565-indexed\","
        "\"localPath\":\"sd://sdcard/tbot/lesson-assets/unaligned.trgb\",\"size\":" +
        std::to_string(unaligned_bytes) + "}]}";
    ResetObservable();
    FreshSession();
    Handle(V4TrgbPrepareFrame(4, 504, "", unaligned_pack));
    require(FrameType(0) == "lesson_error" || !FrameAssetPackReady(0),
            "sector-unaligned TRGB size is rejected independently");
    tbot::SetActiveLessonFlattenedCinematicRenderer(nullptr);
    unsetenv("TBOT_HOST_LESSON_ASSET_ROOT");
}

void test_prepare_assetpack_derives_local_path_from_root_and_key() {
    const char* dir = "/tmp/tbot-host-sd-derived/lesson-assets";
    system("rm -rf /tmp/tbot-host-sd-derived && mkdir -p /tmp/tbot-host-sd-derived/lesson-assets");
    setenv("TBOT_HOST_LESSON_ASSET_ROOT", dir, 1);
    const char* path = "/tmp/tbot-host-sd-derived/lesson-assets/ready%40v1";
    FILE* fp = fopen(path, "wb");
    require(fp != nullptr, "derived local-path fixture opens");
    const char bytes[10] = {1,2,3,4,5,6,7,8,9,10};
    fwrite(bytes, 1, 10, fp);
    fclose(fp);

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
                          "\"criticalAssets\":[{\"key\":\"ready@v1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"ck-derived-abcdef1234567890\","
                          "\"localRoot\":\"sd://sdcard/tbot/lesson-assets/\",\"assets\":["
                          "{\"key\":\"ready@v1\",\"state\":\"READY\","
                          "\"checksumOk\":true,\"size\":10}]}"));
    require(FrameAssetPackReady(0),
            "prepare derives an encoded local path from assetPack.localRoot and key");
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
    require(LogContains("lesson_ack TX assignmentId=" + std::string(AID()) +
                        " sessionId=" + SID() +
                        " stepId=- body.acks=1 rendered=false degraded=false "
                        "renderElapsedMs=-1 assetPack.ready=true "
                        "cacheKey=ck6-abcdef1234567890"),
            "prepare ack log exposes the privacy-safe assetPack attestation payload");
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
    for (int i = 0; i < 33; ++i) {
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
    require(FrameAssetPackReady(0) == true,
            "bounded legacy image packs remain valid under the production TRGB aggregate budget");

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

void test_prepare_reject_preserves_active_lesson_scene() {
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

    const size_t background_calls = disp.background_calls.size();
    const size_t object_calls = disp.object_calls.size();
    const size_t overlay_calls = disp.overlay_calls.size();
    const size_t mode_calls = disp.lesson_mode_calls.size();
    const int cancel_calls = App().cancel_listen_calls;
    Handle(PrepareFrame(1, ",\"criticalAssets\":[{\"key\":\"k1\"}],"
                          "\"assetPack\":{\"cacheKey\":\"w01-d01/v3-abcdef1234567890\",\"assets\":["
                          "{\"key\":\"k1\",\"state\":\"READY\",\"checksumOk\":true,"
                          "\"localPath\":\"sd://sdcard/tbot/lesson-assets/missing.png\",\"size\":10}]}"));

    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "ASSET_PACK_NOT_READY",
            "rejected prepare emits ASSET_PACK_NOT_READY");
    require(disp.background_calls.size() == background_calls &&
                disp.object_calls.size() == object_calls &&
                disp.overlay_calls.size() == overlay_calls &&
                disp.lesson_mode_calls.size() == mode_calls,
            "rejected replacement does not mutate active lesson layers");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Old prompt",
            "rejected replacement preserves active lesson prompt");
    require(App().cancel_listen_calls == cancel_calls,
            "rejected replacement preserves interactive listening state");
    require(App().lesson_runtime_active,
            "rejected replacement preserves active lesson runtime flag");
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

    // Restart the current owner; a foreign prepare must not replace it.
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":1,\"body\":{\"profile\":\"oledTiny\"}}");

    require(FrameType(Sent().size()-1) == "lesson_error", "bad fresh prepare profile -> lesson_error");
    require(FrameSeq(Sent().size()-1) == 1,
            "restart-style rejected replacement uses isolated F->S sequence");
    require(FrameBodyStr(Sent().size()-1, nullptr, "code") == "LESSON_VERSION_UNSUPPORTED",
            "bad fresh prepare profile emits contract error");
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "bad replacement profile preserves active background layer");
    require(!disp.lesson_captions.empty() && disp.lesson_captions.back() == "Old prompt",
            "bad replacement profile preserves active prompt");
    require(App().lesson_runtime_active,
            "bad replacement profile preserves active lesson runtime flag");
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

    const std::string ready_file_name = "tbot-lesson-dedup-ready.bin";
    const std::string ready_pack = ReadyAssetPackExtra(
        "ckD-abcdef1234567890", "abcdef1234567890", ready_file_name);
    Handle(PrepareFrame(1, ready_pack));
    require(Sent().size() == 1, "prepare ack");
    bool first_ready = FrameAssetPackReady(0);
    require(first_ready, "dedup fixture establishes a ready prepared owner");

    // duplicate prepare (same assignment/session, sequence <= last) -> re-ack path.
    // duplicate_prepare==true so NO session reset; sequence<=last triggers replay.
    HostEspResetLogs();
    Handle(PrepareFrame(1, ready_pack));
    require(Sent().size() == 2, "duplicate prepare re-acks");
    require(FrameType(1) == "lesson_ack", "re-ack is an ack");
    // NOTE non-tautology: re-ack must REPLAY the cached assetPack body. Mutation: drop the
    // cached-assetPack replay (re_asset_pack=nullptr) -> the re-ack would carry NO assetPack.
    require(FrameHasAssetPack(1), "duplicate re-ack replays cached assetPack body");
    require(FrameAssetPackReady(1) == first_ready, "re-ack replays cached ready flag");
    require(LogContains("body.acks=1 rendered=false degraded=false") &&
                LogContains("assetPack.ready=true cacheKey=ckD-abcdef1234567890"),
            "exact duplicate replay emits structured cached ack evidence");

    // Sequence zero is now rejected as a pure envelope error before dedup/reservation.
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() + "\","
           "\"sequence\":0,\"body\":{\"profile\":\"" + kLessonProfileEspTft + "\"}}");
    require(Sent().size() == 3, "sequence-zero duplicate emits one refusal");
    require(FrameType(2) == "lesson_error", "sequence-zero duplicate is not re-acked");
    require(FrameBodyStr(2, nullptr, "code") == "LESSON_SEQUENCE_INVALID",
            "sequence-zero duplicate uses stable envelope error");
    RemoveReadyAssetPackFixture(ready_file_name);
}

void test_delayed_duplicate_prepare_replays_assetpack_after_start_ack() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    const std::string ready_file_name = "tbot-lesson-delayed-ready.bin";
    const std::string ready_pack = ReadyAssetPackExtra(
        "ck-delayed-abcdef1234567890", "abcdef1234567890", ready_file_name);

    Handle(PrepareFrame(5, ready_pack));
    require(Sent().size() == 1, "initial prepare emits assetPack ack");
    require(FrameHasAssetPack(0), "initial prepare ack carries assetPack");

    Handle(StartFrame(6));
    require(Sent().size() == 2, "start emits lifecycle ack");
    require(!FrameHasAssetPack(1), "start ack carries no assetPack");

    for (int sequence = 7; sequence <= 24; ++sequence) {
        Handle((sequence % 2) ? PauseFrame(sequence) : ResumeFrame(sequence));
    }

    Handle(PrepareFrame(5, ready_pack));
    const size_t replay_index = Sent().size() - 1;
    require(Sent().size() == 21, "expired duplicate prepare re-acks after replay-window eviction");
    require(FrameHasAssetPack(replay_index),
            "expired duplicate prepare replays the dedicated cached assetPack");
    require(FrameBodyStr(replay_index, "assetPack", "cacheKey") ==
                "ck-delayed-abcdef1234567890",
            "expired duplicate prepare replays original assetPack cacheKey");
    require(!FrameBodyBool(replay_index, "rendered", true) &&
                !FrameBodyBool(replay_index, "degraded", true),
            "expired duplicate prepare uses conservative render fallback semantics");
    RemoveReadyAssetPackFixture(ready_file_name);
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
    // Restart the existing reservation owner; foreign prepares are refused.
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

void test_preload_reset_prepare_quiesces_without_arming_lesson_start() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    App().lesson_runtime_active = true;

    Handle(PrepareFrame(1, ",\"preloadResetOnly\":true"));
    require(FrameType(0) == "lesson_ack", "preload reset prepare is acknowledged");
    require(App().lesson_runtime_active == false,
            "preload reset clears the stale lesson runtime flag");

    Handle(StartFrame(2));
    require(App().lesson_runtime_active == false,
            "preload reset does not arm a normal lesson_start");
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
    require(App().lesson_terminal_audio_quiet,
            "stop quarantines late lesson TTS before releasing the runtime");
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

    Handle(PrepareFrame(1));
    Handle(StartFrame(2));
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

void test_step_rejects_authored_motion_media_sources_and_mime_types() {
    for (const std::string& source : {"http://x/poster.gif", "http://x/poster.WEBM?token=1"}) {
        ResetObservable();
        Board::GetInstance().display_ = nullptr;
        Board::GetInstance().network_ = nullptr;
        OpenSession();
        Handle(StepFrame(3, "forbidden-media", source, "http://x/o.png", "http://x/r.png",
                         ",\"stepType\":\"greeting\"", ""));
        require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "ASSET_PROFILE_UNAVAILABLE",
                "authored motion-media source is rejected defensively");
    }

    for (const auto& sources : {
             std::vector<std::string>{"http://x/p.jpg", "http://x/object.mov#frame", "http://x/r.png"},
             std::vector<std::string>{"http://x/p.jpg", "http://x/o.png", "http://x/overlay.mp4"},
         }) {
        ResetObservable();
        Board::GetInstance().display_ = nullptr;
        Board::GetInstance().network_ = nullptr;
        OpenSession();
        Handle(StepFrame(3, "forbidden-layer", sources[0], sources[1], sources[2],
                         ",\"stepType\":\"greeting\"", ""));
        require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "ASSET_PROFILE_UNAVAILABLE",
                "object and overlay motion-media sources are rejected defensively");
    }

    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    Handle(std::string("{\"type\":\"lesson_step\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() + "\",\"sessionId\":\"" + SID() +
           "\",\"stepId\":\"video-mime\",\"sequence\":3,\"body\":{\"profile\":\"" +
           kLessonProfileEspTft + "\",\"scene\":{"
           "\"backgroundScene\":{\"mode\":\"poster\",\"poster\":{\"src\":\"http://x/p.jpg\",\"mediaType\":\"image/gif\"}},"
           "\"teachingObject\":{\"asset\":{\"src\":\"http://x/o.png\"}},"
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.png\"}}}}}");
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "ASSET_PROFILE_UNAVAILABLE",
            "video MIME type is rejected even when the URL extension looks static");

    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Board::GetInstance().network_ = nullptr;
    OpenSession();
    const std::string video_arrived =
        "{\"versionId\":\"pose-v1\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"bytes\":4096,\"mediaType\":\"video/mp4\"}";
    Handle(TvideoStepFrame(3, TvideoProjection("", video_arrived)));
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "ASSET_PROFILE_UNAVAILABLE",
            "template arrived-pose video MIME is rejected before fallback processing");
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
    HostJpegDecodeWithCapsCalls() = 0;
    HostLastJpegDecodeCaps() = 0;
    HostHeapCapsCalls().clear();
    HostHeapSizeCalls().clear();
    HostHeapAlignmentCalls().clear();
    HostHeapPhaseMonitorStarts() = 0;
    HostHeapPhaseMonitorStops() = 0;
    HostHeapCheckpointPhases().clear();

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
           "\"robotOverlay\":{\"asset\":{\"src\":\"http://x/r.jpg\"},"
           "\"expression\":\" Teaching \",\"robotState\":\"talking\"}}}}");
    size_t idx = Sent().size() - 1;
    require(FrameType(idx) == "lesson_ack", "rendered step acks");
    // NOTE non-tautology: all three layers fetched+drew so degraded MUST be false.
    // Mutation: force overlay fetch to fail (degraded becomes true) -> this flips.
    require(FrameBodyBool(idx, "rendered", false) == true, "step ack rendered=true");
    require(FrameBodyBool(idx, "degraded", true) == false,
            "all three layers drew -> degraded=false");
    require(FrameBodyStr(idx, nullptr, "robotState") == "talking",
            "step ack echoes the rendered robotState for strict evidence correlation");
    require(disp.lesson_mode_calls.size() > lesson_mode_calls_before_step &&
            disp.lesson_mode_calls.back() == true,
            "step hides the start loading face before drawing scene layers");
    require(!disp.background_calls.empty() && disp.background_calls.back() == true,
            "background image drawn back layer");
    require(!disp.object_calls.empty() && disp.object_calls.back() == true,
            "teaching object image drawn");
    require(!disp.overlay_calls.empty() && disp.overlay_calls.back() == true,
            "robot overlay image drawn");
    require(HostJpegDecodeWithCapsCalls() == 3,
            "each lesson JPEG decodes directly with the lesson allocation contract");
    require((HostLastJpegDecodeCaps() & MALLOC_CAP_SPIRAM) != 0,
            "first decoded JPEG output allocation requires PSRAM");
    require((HostLastJpegDecodeCaps() & MALLOC_CAP_INTERNAL) == 0,
            "decoded JPEG output allocation never requests internal SRAM");
    require(std::count(HostHeapAlignmentCalls().begin(), HostHeapAlignmentCalls().end(), size_t{16}) == 3,
            "lesson JPEG decoded outputs are allocated with 16-byte alignment");
    const auto first_decoded = std::find(HostHeapAlignmentCalls().begin(),
                                         HostHeapAlignmentCalls().end(), size_t{16});
    const size_t first_decoded_index = first_decoded - HostHeapAlignmentCalls().begin();
    require(first_decoded_index < HostHeapCapsCalls().size() &&
                HostHeapSizeCalls()[first_decoded_index] == 8,
            "first decoded output allocation is directly observable before image ownership");
    require((HostHeapCapsCalls()[first_decoded_index] & MALLOC_CAP_SPIRAM) != 0 &&
                (HostHeapCapsCalls()[first_decoded_index] & MALLOC_CAP_INTERNAL) == 0,
            "first decoded output allocation is directly backed by PSRAM");
    require(HostHeapPhaseMonitorStarts() == 1 && HostHeapPhaseMonitorStops() == 1,
            "lesson render brackets one phase-local heap monitor");
    require(HostHeapCheckpointPhases() == std::vector<std::string>{"lesson_render.complete"},
            "lesson render emits the phase checkpoint before ack");
    require(!disp.lesson_captions.empty() &&
            disp.lesson_captions.back() == "Xin chào", "authored prompt caption drawn");
    require(disp.last_status == "Đang học...", "rendered step replaces loading status with active lesson status");
    require(disp.chat_messages.empty(), "new lesson step clears stale chat and does not enter normal chat history");
    require(disp.set_emotion_calls == emotion_calls_before_step,
            "lesson step does not draw realtime emotion over lesson layers");
    // passive greeting: no interactive listen window opened.
    require(App().prepare_listen_calls == 0, "passive step opens NO listen window");
}

void test_tvideo_first_step_applies_named_arrived_geometry_and_reports_fallback() {
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

    const std::string projection = TvideoProjection();
    Handle(TvideoStepFrame(3, projection));

    const size_t first_ack = Sent().size() - 1;
    require(FrameBodyBool(first_ack, "rendered", false),
            "tvideo fallback still renders teaching content");
    require(FrameBodyBool(first_ack, "degraded", false),
            "missing atlas marks the rendered first step degraded");
    require(FrameBodyStr(first_ack, nullptr, "degradedReason") == "missingAtlas",
            "tvideo ack reports a stable missing-atlas reason");
    require(disp.overlay_bounds == std::vector<int>({184, 184, 112, 56}),
            "centerRoad applies the reviewed arrived-pose overlay bounds");

    Handle(TvideoStepFrame(3, projection));
    require(FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") == "missingAtlas",
            "duplicate first-step ack replays the same degraded reason");

    Handle(StepFrame(4, "ordinary-later", "http://x/p.jpg", "http://x/o.jpg",
                     "http://x/r.jpg", ",\"stepType\":\"greeting\"", ""));
    require(!FrameBodyBool(Sent().size() - 1, "degraded", true),
            "later steps remain on the legacy three-layer render path");
    require(disp.overlay_bounds == std::vector<int>({0, 0, 0, 0}),
            "later steps do not repeat the first-step named entrance geometry");
}

void test_tvideo_malformed_projection_uses_safe_unsupported_contract_fallback() {
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

    const std::string bad_phases =
        "[{\"name\":\"hidden\",\"durationMs\":101},"
        "{\"name\":\"flyIn\",\"durationMs\":1200},"
        "{\"name\":\"landFar\",\"durationMs\":700},"
        "{\"name\":\"settle\",\"durationMs\":350},"
        "{\"name\":\"walkToward\",\"durationMs\":1800},"
        "{\"name\":\"arriveNear\",\"durationMs\":250},"
        "{\"name\":\"greetIdle\",\"durationMs\":650},"
        "{\"name\":\"revealTeachingContent\",\"durationMs\":100}]";
    Handle(TvideoStepFrame(3, TvideoProjection(bad_phases)));

    const size_t ack = Sent().size() - 1;
    require(FrameBodyBool(ack, "rendered", false),
            "invalid template version falls back without blocking content");
    require(FrameBodyBool(ack, "degraded", false),
            "invalid template version is reported as degraded");
    require(FrameBodyStr(ack, nullptr, "degradedReason") == "unsupportedContract",
            "phase drift cannot be treated as a supported contract");
    require(disp.overlay_bounds == std::vector<int>({0, 0, 0, 0}),
            "unsupported geometry cannot apply authored or raw coordinates");

    for (const std::string& malformed : {
             std::string("[]"),
             std::string("{\"templateVersion\":1}"),
             ReplaceOnce(TvideoProjection(), "\"templateVersion\":1", "\"templateVersion\":1.5"),
             ReplaceOnce(TvideoProjection(), "\"name\":\"hidden\"", "\"name\":\"flyIn\""),
             ReplaceOnce(TvideoProjection(), "\"revealPhase\":\"revealTeachingContent\"",
                          "\"revealPhase\":\"arriveNear\""),
             ReplaceOnce(TvideoProjection(), "\"fallbackPolicy\":\"snapToArriveNearAndReveal\"",
                          "\"fallbackPolicy\":\"keepWalking\""),
             ReplaceOnce(TvideoProjection(),
                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                          "gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
             ReplaceOnce(TvideoProjection(), "\"geometryVersion\":1",
                          "\"geometryVersion\":1,\"x\":42"),
         }) {
        ResetObservable();
        Board::GetInstance().display_ = &disp;
        Board::GetInstance().network_ = &net;
        OpenSession();
        ResetHostHttp();
        HostHttp().body = JpegBody();
        HostJpegDecodeMode() = 0;
        Handle(TvideoStepFrame(3, malformed));
        require(FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") == "unsupportedContract",
                "any drift from the immutable projection shape reveals safely as unsupported");
    }
}

void test_tvideo_requires_pinned_arrived_pose_linkage() {
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

    Handle(TvideoStepFrame(3, TvideoProjection(),
                           "\"key\":\"different-pose\","
                           "\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
                           ""));

    const size_t ack = Sent().size() - 1;
    require(FrameBodyStr(ack, nullptr, "degradedReason") == "missingOverlay",
            "unlinked ordinary overlay is not accepted as the pinned arrived pose");
    require(disp.overlay_calls.empty() || !disp.overlay_calls.back(),
            "unlinked ordinary overlay is not rendered as the template fallback");
    require(disp.overlay_bounds == std::vector<int>({0, 0, 0, 0}),
            "missing pinned fallback does not apply named robot bounds to another asset");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    OpenSession();
    ResetHostHttp();
    HostHttp().open_ok = false;
    Handle(TvideoStepFrame(3, TvideoProjection()));
    require(FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") == "missingOverlay",
            "a pinned arrived pose that cannot be loaded reports missingOverlay");
    require(disp.overlay_bounds == std::vector<int>({0, 0, 0, 0}),
            "failed arrived-pose load clears named bounds before revealing content");
}

void test_tvideo_is_restricted_to_the_true_first_lesson_step() {
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

    Handle(StepFrame(3, "legacy-first", "http://x/p.jpg", "http://x/o.jpg", "http://x/r.jpg",
                     ",\"templateProjection\":{\"templateId\":\"otherTemplate\"}"
                     ",\"stepType\":\"greeting\"", ""));
    require(!FrameBodyBool(Sent().size() - 1, "degraded", true),
            "non-tvideo projection on first step leaves legacy rendering unchanged");

    Handle(TvideoStepFrame(4, TvideoProjection()));
    require(FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") == "unsupportedContract",
            "a tvideo entrance arriving after the true first step is rejected safely");
    require(disp.overlay_bounds == std::vector<int>({0, 0, 0, 0}),
            "late tvideo projection cannot apply entrance geometry");
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
    HostHeapCapsCalls().clear();
    Handle(StepFrame(3, "s4", "http://x/p.png", "http://x/o.png", "http://x/r.png",
                     ",\"prompt\":\"P\",\"stepType\":\"greeting\"", ""));
    require(FrameBodyBool(Sent().size()-1, "degraded", true) == false,
            "chunked growth fetch draws all layers -> not degraded");
    require(disp.background_calls.back() == true, "chunked poster drew");
    require(!HostHeapCapsCalls().empty(), "chunked fetch records initial and grown allocations");
    for (int caps : HostHeapCapsCalls()) {
        require((caps & MALLOC_CAP_SPIRAM) != 0, "chunked buffers use PSRAM");
        require((caps & MALLOC_CAP_8BIT) != 0, "chunked buffers remain byte-addressable");
        require((caps & MALLOC_CAP_INTERNAL) == 0, "chunked buffers never use internal SRAM");
    }

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

void test_lesson_asset_reservation_blocks_prepare_before_asset_io() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    const std::string with_asset =
        ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
        "\"assetPack\":{\"cacheKey\":\"pack-abcdef1234567890\",\"assets\":["
        "{\"key\":\"poster\",\"state\":\"READY\",\"checksumOk\":true,"
        "\"localPath\":\"sd://tbot/never-open.jpg\",\"size\":4}]}";
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "test mutation lease acquired");

        Handle(PrepareFrame(1, with_asset));

        require(Sent().size() == 1, "mutation-active prepare emits one stable error");
        require(FrameType(0) == "lesson_error", "mutation-active prepare is rejected");
        require(FrameSeq(0) == 1, "mutation-active refusal starts its isolated F->S stream at 1");
        require(FrameBodyStr(0, nullptr, "code") == "LESSON_ASSET_MUTATION_ACTIVE",
                "mutation-active prepare uses stable code");
        require(FrameBodyBool(0, "retryable", false), "mutation-active prepare is retryable");
        require(FrameBodyStr(0, "context", "reason") == "asset_mutation_active",
                "mutation-active prepare uses privacy-safe reason");
        require(HostLessonAssetOpenCount() == 0,
                "mutation-active prepare performs zero lesson asset fopen calls");
        require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
                "refused prepare publishes no lesson reservation");
    }
    Handle(PrepareFrame(1));
    require(FrameType(Sent().size() - 1) == "lesson_ack",
            "same identity can prepare after mutation refusal");
    require(FrameSeq(Sent().size() - 1) == 1,
            "mutation refusal did not contaminate later owned F->S stream");
}

void test_lesson_asset_reservation_duplicate_and_foreign_prepare() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    const std::string owner_assignment = AID();
    const std::string owner_session = SID();
    Handle(PrepareFrame(1));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(owner.acquired && owner.idempotent && owner.generation != 0,
            "successful prepare owns a nonzero coordinator generation");

    Handle(PrepareFrame(1));
    auto duplicate = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(duplicate.acquired && duplicate.idempotent &&
                duplicate.generation == owner.generation,
            "duplicate prepare preserves the exact owner generation");

    const size_t before_foreign = Sent().size();
    Handle(PrepareFrameFor("foreign-assignment", "foreign-session", 1));
    require(Sent().size() == before_foreign + 1, "foreign prepare emits one refusal");
    require(FrameType(Sent().size() - 1) == "lesson_error", "foreign prepare is rejected");
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_SESSION_CONFLICT",
            "foreign prepare uses stable conflict code");
    require(FrameSeq(Sent().size() - 1) == 1,
            "foreign refusal starts its own isolated F->S stream at 1");
    auto after_foreign = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(after_foreign.acquired && after_foreign.idempotent &&
                after_foreign.generation == owner.generation,
            "foreign prepare cannot replace the reservation owner");
    Handle(StartFrame(2));
    require(FrameSeq(Sent().size() - 1) == 3,
            "foreign refusal does not advance owner F->S sequence");
}

void test_normal_prepare_consumes_one_storage_reservation_attempt_and_generation() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    require(coordinator.SetLastGenerationForTest(100),
            "single-reservation test seeds coordinator generation while idle");
    coordinator.ResetLessonSessionReservationAttemptsForTest();

    Handle(PrepareFrame(1));

    require(FrameType(0) == "lesson_ack", "normal prepare is accepted");
    require(coordinator.LessonSessionReservationAttemptsForTest() == 1,
            "normal prepare performs exactly one storage reservation attempt");
    auto owner = coordinator.TryBeginLessonSession(AID(), SID());
    require(owner.acquired && owner.idempotent && owner.generation == 101,
            "normal prepare owns exactly the next coordinator generation");
    require(coordinator.LessonSessionReservationAttemptsForTest() == 2,
            "test generation inspection accounts for its own idempotent reservation");
    require(App().AbandonLessonStorageSession(),
            "single-reservation test releases prepared lesson owner");

    auto next = coordinator.TryBeginLessonSession("next-assignment", "next-session");
    require(next.acquired && !next.idempotent && next.generation == 102,
            "next lesson owner receives the next generation after one consumed prepare");
    require(coordinator.EndLessonSession("next-assignment", "next-session", next.generation),
            "single-reservation test releases next owner");
    require(coordinator.SetLastGenerationForTest(0),
            "single-reservation test restores coordinator generation");
}

void test_prepare_pure_contract_validation_precedes_storage_reservation() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "mutation held for pure-validation ordering test");
        std::string wrong_version = PrepareFrame(1);
        const size_t version_pos = wrong_version.find(kLessonProtocolVersion);
        require(version_pos != std::string::npos, "wrong-version fixture locates token");
        wrong_version.replace(version_pos, strlen(kLessonProtocolVersion), "WRONG");
        Handle(wrong_version);
        require(FrameBodyStr(0, nullptr, "code") == "LESSON_VERSION_UNSUPPORTED",
                "wrong version wins before mutation conflict");
        require(FrameSeq(0) == 1, "wrong-version fresh refusal uses isolated sequence 1");
        require(mutation.code() == LessonAssetReservationCode::kAcquired,
                "pure validation leaves existing mutation lease unchanged");
    }

    ResetObservable();
    FreshSession();
    const std::string owner_assignment = AID();
    const std::string owner_session = SID();
    Handle(PrepareFrame(1));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    std::string foreign_wrong = PrepareFrameFor("foreign", "foreign", 1);
    const size_t version_pos = foreign_wrong.find(kLessonProtocolVersion);
    require(version_pos != std::string::npos, "foreign wrong-version fixture locates token");
    foreign_wrong.replace(version_pos, strlen(kLessonProtocolVersion), "WRONG");
    Handle(foreign_wrong);
    require(FrameBodyStr(Sent().size() - 1, nullptr, "code") == "LESSON_VERSION_UNSUPPORTED",
            "foreign wrong version wins before session conflict");
    auto retained = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(retained.acquired && retained.idempotent && retained.generation == owner.generation,
            "foreign pure-validation failure never changes owner reservation");
}

void test_prepare_sequence_zero_is_pure_rejection_without_generation_burn() {
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    require(coordinator.SetLastGenerationForTest(41),
            "sequence-zero test seeds generation while idle");
    FreshSession();
    const std::string assignment = AID();
    const std::string session = SID();
    Handle(PrepareFrame(0));
    require(Sent().size() == 1, "sequence zero emits exactly one refusal");
    require(FrameType(0) == "lesson_error", "sequence zero emits protocol error");
    require(FrameSeq(0) == 1, "sequence-zero refusal uses isolated incoming stream");
    require(FrameBodyStr(0, nullptr, "code") == "LESSON_SEQUENCE_INVALID",
            "sequence zero uses stable envelope error");
    require(!coordinator.HasLessonSession(), "sequence zero publishes no reservation");

    Handle(PrepareFrame(1));
    auto valid = coordinator.TryBeginLessonSession(assignment, session);
    require(valid.acquired && valid.idempotent && valid.generation == 42,
            "sequence zero does not burn coordinator generation");
    Handle(StopFrame(2));
    require(coordinator.SetLastGenerationForTest(0),
            "sequence-zero generation test restores seam");

    ResetObservable();
    require(coordinator.SetLastGenerationForTest(73),
            "mutation isolation test seeds generation while idle");
    FreshSession();
    {
        auto mutation = coordinator.TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "mutation active before sequence-zero frame");
        Handle(PrepareFrame(0));
        require(Sent().size() == 1,
                "sequence zero under mutation emits exactly one refusal");
        require(FrameBodyStr(0, nullptr, "code") == "LESSON_SEQUENCE_INVALID",
                "sequence error wins before mutation conflict");
        require(mutation.code() == LessonAssetReservationCode::kAcquired,
                "sequence-zero rejection leaves mutation lease unchanged");
        require(!coordinator.HasLessonSession(),
                "sequence-zero rejection under mutation creates no session");
    }
    Handle(PrepareFrame(1));
    auto after_mutation = coordinator.TryBeginLessonSession(AID(), SID());
    require(after_mutation.acquired && after_mutation.idempotent &&
                after_mutation.generation == 74,
            "sequence zero under mutation does not burn generation");
    Handle(StopFrame(2));
    require(coordinator.SetLastGenerationForTest(0),
            "mutation generation test restores seam");

    ResetObservable();
    const int owner_sequence = OpenSession();
    auto owner = coordinator.TryBeginLessonSession(AID(), SID());
    require(owner.acquired && owner.idempotent, "running owner exists before seq-zero frame");
    Handle(PrepareFrame(0));
    require(FrameSeq(Sent().size() - 1) == 3,
            "current same-identity sequence-zero error continues owner stream");
    require(App().lesson_runtime_active, "sequence-zero frame leaves running owner active");
    auto retained = coordinator.TryBeginLessonSession(AID(), SID());
    require(retained.acquired && retained.idempotent && retained.generation == owner.generation,
            "sequence-zero frame preserves owner generation");
    Handle(PauseFrame(owner_sequence));
    require(FrameSeq(Sent().size() - 1) == 4,
            "owner response remains monotonic after current-version sequence error");
    Handle(StopFrame(owner_sequence + 1));

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"assignmentVersion\":10"));
    Handle(StartFrame(2));
    auto versioned_owner = coordinator.TryBeginLessonSession(AID(), SID());
    require(versioned_owner.acquired && versioned_owner.idempotent,
            "versioned owner exists before newer seq-zero candidate");
    Handle(PrepareFrame(0, ",\"assignmentVersion\":11"));
    require(FrameSeq(Sent().size() - 1) == 1,
            "newer-version sequence-zero candidate uses isolated stream");
    require(App().lesson_runtime_active,
            "newer-version sequence-zero candidate preserves running owner");
    auto versioned_retained = coordinator.TryBeginLessonSession(AID(), SID());
    require(versioned_retained.acquired && versioned_retained.idempotent &&
                versioned_retained.generation == versioned_owner.generation,
            "newer-version sequence-zero candidate preserves owner generation");
    Handle(PauseFrame(3));
    require(FrameSeq(Sent().size() - 1) == 3,
            "isolated newer-version sequence error does not advance owner counter");
    Handle(StopFrame(4));
}

void test_lesson_transport_rejects_decoded_nul_before_cjson_truncation() {
    ResetObservable();
    FreshSession();
    const std::string owner_assignment = AID();
    const std::string owner_session = SID();
    Handle(PrepareFrame(1));
    Handle(StartFrame(2));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(owner.acquired && owner.idempotent, "ASCII owner reserved before NUL frames");

    const std::vector<std::string> nul_frames = {
        PrepareFrameFor(owner_assignment + "\\u0000foreign", owner_session, 3),
        StopFrameFor(owner_assignment, owner_session + "\\u0000foreign", 3),
        std::string("{\"type\":\"lesson_error\",\"protocolVersion\":\"") +
            kLessonProtocolVersion + "\",\"assignmentId\":\"" + owner_assignment +
            "\\u0000foreign\",\"sessionId\":\"" + owner_session +
            "\",\"sequence\":3,\"body\":{\"code\":\"STEP_TIMEOUT\"}}",
    };
    const size_t sent_before = Sent().size();
    for (const auto& frame : nul_frames) {
        require(!HandleTransportJson(frame), "decoded-NUL lesson identity is dropped pre-parse");
        require(Sent().size() == sent_before, "decoded-NUL frame emits no truncated-prefix reply");
        auto retained = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
            owner_assignment, owner_session);
        require(retained.acquired && retained.idempotent && retained.generation == owner.generation,
                "decoded-NUL frame cannot replace or release ASCII owner");
    }

    const std::string escaped_literal =
        "{\"assignmentId\":\"abc\\\\u0000def\",\"note\":\"safe literal slash\"}";
    require(!JsonHasForbiddenDecodedNull(escaped_literal.data(), escaped_literal.size()),
            "escaped literal backslash-u0000 is not decoded NUL");
    const std::string nul_in_key = "{\"assignment\\u0000Id\":\"value\"}";
    require(JsonHasForbiddenDecodedNull(nul_in_key.data(), nul_in_key.size()),
            "decoded NUL in a JSON key is rejected lexically");
    std::string raw_nul = "{\"assignmentId\":\"abc";
    raw_nul.push_back('\0');
    raw_nul += "def\"}";
    require(JsonHasForbiddenDecodedNull(raw_nul.data(), raw_nul.size()),
            "raw NUL byte is rejected before parsing");
}

void test_lesson_asset_reservation_refusal_mapping_is_total() {
    using Code = LessonAssetReservationCode;
    struct Case {
        Code code;
        const char* expected_code;
        const char* expected_message;
        const char* expected_reason;
        bool expected_retryable;
    };
    const Case cases[] = {
        {Code::kMutationActive, "LESSON_ASSET_MUTATION_ACTIVE",
         "lesson assets are being updated", "asset_mutation_active", true},
        {Code::kLessonSessionActive, "LESSON_SESSION_CONFLICT",
         "another lesson session owns lesson assets", "lesson_session_mismatch", true},
        {Code::kLessonSessionMismatch, "LESSON_SESSION_CONFLICT",
         "another lesson session owns lesson assets", "lesson_session_mismatch", true},
        {Code::kInvalidIdentity, "LESSON_IDENTITY_INVALID",
         "lesson identity is invalid", "invalid_identity", false},
        {Code::kGenerationExhausted, "LESSON_RESERVATION_EXHAUSTED",
         "lesson storage reservation unavailable", "generation_exhausted", false},
        {Code::kAcquired, "LESSON_RESERVATION_EXHAUSTED",
         "lesson storage reservation unavailable", "generation_exhausted", false},
        {static_cast<Code>(0xff), "LESSON_RESERVATION_EXHAUSTED",
         "lesson storage reservation unavailable", "generation_exhausted", false},
    };

    for (const auto& c : cases) {
        const auto mapping = tbot::LessonReservationRefusalMappingForTest(c.code);
        require(std::string(mapping.code) == c.expected_code,
                "reservation refusal mapping returns stable error code");
        require(std::string(mapping.message) == c.expected_message,
                "reservation refusal mapping returns stable message");
        require(std::string(mapping.reason) == c.expected_reason,
                "reservation refusal mapping returns privacy-safe reason");
        require(mapping.retryable == c.expected_retryable,
                "reservation refusal mapping returns retryability");
    }
}

void test_lesson_asset_reservation_invalid_and_exhausted_prepare() {
    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrameFor("", "session", 1));
    require(FrameType(0) == "lesson_error", "empty assignment identity is rejected");
    require(FrameBodyStr(0, nullptr, "code") == "LESSON_IDENTITY_INVALID",
            "invalid identity uses stable validation code");
    require(!FrameBodyBool(0, "retryable", true), "invalid identity is not retryable");

    ResetObservable();
    FreshSession();
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion + "\",\"assignmentId\":\"" + AID() +
           "\",\"sessionId\":\"" + SID() + "\",\"sequence\":1,\"body\":[]}");
    require(FrameBodyStr(0, nullptr, "code") == "LESSON_ENVELOPE_INVALID",
            "non-object prepare body is rejected before reservation");

    ResetObservable();
    Handle(PrepareFrameFor(std::string(kLessonAssetIdentityMaxBytes + 1, 'a'), "session", 1));
    require(FrameType(0) == "lesson_error", "oversized assignment identity is rejected");
    require(FrameBodyStr(0, "context", "reason") == "invalid_identity",
            "oversized identity response does not echo identity");

    ResetObservable();
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    require(coordinator.SetLastGenerationForTest(UINT64_MAX),
            "test seam sets exhausted generation while idle");
    FreshSession();
    Handle(PrepareFrame(1));
    require(FrameType(0) == "lesson_error", "generation exhaustion rejects prepare");
    require(FrameBodyStr(0, nullptr, "code") == "LESSON_RESERVATION_EXHAUSTED",
            "generation exhaustion uses stable code");
    require(!FrameBodyBool(0, "retryable", true), "generation exhaustion fails closed");
    require(!coordinator.HasLessonSession(), "generation exhaustion publishes no owner");
    require(coordinator.SetLastGenerationForTest(0), "test seam restores generation state");
}

void test_lesson_asset_reservation_prepare_failure_release_ownership() {
    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"assetPack\":{\"cacheKey\":\"pack\",\"assets\":[]}"));
    require(FrameType(0) == "lesson_error", "new prepare can fail after reservation");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "failed newly acquired prepare releases its exact reservation");
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation), "mutation can acquire after failed new prepare");
    }

    ResetObservable();
    FreshSession();
    const std::string owner_assignment = AID();
    const std::string owner_session = SID();
    Handle(PrepareFrame(1, ",\"assignmentVersion\":1"));
    Handle(StartFrame(2));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(owner.acquired && owner.idempotent, "initial prepare owns reservation");
    const bool runtime_before_replacement = App().lesson_runtime_active;
    Handle(PrepareFrame(3, ",\"assignmentVersion\":2,"
                           "\"assetPack\":{\"cacheKey\":\"pack\",\"assets\":[]}"));
    require(FrameType(Sent().size() - 1) == "lesson_error",
            "idempotent prepare can reject malformed replacement payload");
    auto retained = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(retained.acquired && retained.idempotent && retained.generation == owner.generation,
            "failed idempotent prepare does not release the existing owner");
    require(App().lesson_runtime_active == runtime_before_replacement,
            "failed idempotent replacement preserves active renderer state");
    Handle(StopFrame(4));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "old owner remains terminally usable after failed replacement");
    auto mutation_after_stop =
        LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
    require(static_cast<bool>(mutation_after_stop),
            "mutation reacquires after exact old-owner stop");
}

void test_not_ready_asset_candidate_does_not_commit_lesson_session() {
    const std::string not_ready_pack =
        ",\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
        "\"assetPack\":{\"cacheKey\":\"candidate-abcdef1234567890\",\"assets\":[]}";

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, not_ready_pack));
    require(FrameType(0) == "lesson_ack" && !FrameAssetPackReady(0),
            "fresh not-ready candidate reports truthful readiness");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "fresh not-ready candidate releases reservation for sync repair");
    {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        require(static_cast<bool>(mutation),
                "sync mutation can run after fresh not-ready candidate");
    }

    ResetObservable();
    const int replacement_sequence = OpenSession();
    const std::string owner_assignment = AID();
    const std::string owner_session = SID();
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(owner.acquired && owner.idempotent, "running owner exists before candidate");
    Handle(PrepareFrame(replacement_sequence, not_ready_pack));
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                !FrameAssetPackReady(Sent().size() - 1),
            "same-owner not-ready replacement reports readiness without commit");
    require(App().lesson_runtime_active,
            "same-owner not-ready replacement preserves running renderer");
    auto retained = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(retained.acquired && retained.idempotent && retained.generation == owner.generation,
            "same-owner not-ready replacement preserves exact reservation generation");
    Handle(StopFrame(replacement_sequence + 1));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "preserved owner can stop after not-ready replacement");
}

void test_isolated_not_ready_asset_prepare_ack_json_failure_sends_no_empty_frame() {
    const std::string not_ready_republish =
        ",\"assignmentVersion\":11,"
        "\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
        "\"assetPack\":{\"cacheKey\":\"republish-abcdef1234567890\",\"assets\":[]}";

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"assignmentVersion\":10"));
    Handle(StartFrame(2));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(AID(), SID());
    require(owner.acquired && owner.idempotent, "json-fail fixture has running owner");

    const size_t sent_before_failure = Sent().size();
    tbot::SetLessonJsonFailAfterForTest(0);
    Handle(PrepareFrame(1, not_ready_republish));
    require(Sent().size() == sent_before_failure,
            "isolated not-ready prepare ACK JSON failure sends no empty frame");

    tbot::SetLessonJsonFailAfterForTest(-1);
    Handle(PrepareFrame(1, not_ready_republish));
    require(Sent().size() == sent_before_failure + 1 &&
                FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameSeq(Sent().size() - 1) == 1 &&
                !FrameAssetPackReady(Sent().size() - 1),
            "isolated not-ready prepare ACK recovers on the same isolated sequence");
    auto retained = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(AID(), SID());
    require(retained.acquired && retained.idempotent && retained.generation == owner.generation,
            "failed isolated not-ready ACK preserves running owner generation");
    Handle(StopFrame(3));
}

void test_republished_not_ready_candidate_uses_isolated_stream() {
    const std::string not_ready_republish =
        ",\"assignmentVersion\":11,"
        "\"manifestRef\":{\"manifestChecksum\":\"abcdef1234567890\"},"
        "\"assetPack\":{\"cacheKey\":\"republish-abcdef1234567890\",\"assets\":[]}";

    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    Handle(PrepareFrame(1, ",\"assignmentVersion\":10"));
    Handle(StartFrame(2));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(AID(), SID());
    require(owner.acquired && owner.idempotent, "version 10 owner is running");

    Handle(PrepareFrame(1, not_ready_republish));
    require(FrameType(Sent().size() - 1) == "lesson_ack" &&
                FrameSeq(Sent().size() - 1) == 1 &&
                !FrameAssetPackReady(Sent().size() - 1),
            "newer not-ready republish uses isolated F->S sequence 1");
    Handle(PrepareFrame(1, not_ready_republish));
    require(FrameSeq(Sent().size() - 1) == 1,
            "retry of uncommitted republish remains on isolated sequence 1");
    require(App().lesson_runtime_active,
            "not-ready republish leaves old running renderer active");
    auto retained = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(AID(), SID());
    require(retained.acquired && retained.idempotent && retained.generation == owner.generation,
            "not-ready republish preserves old owner generation");

    Handle(PrepareFrame(1, ",\"assignmentVersion\":10"));
    require(FrameSeq(Sent().size() - 1) == 3,
            "current-version duplicate resumes unchanged owner F->S stream");
    require(!FrameHasAssetPack(Sent().size() - 1),
            "uncommitted candidate did not overwrite owner prepare-ack cache");
    Handle(PauseFrame(3));
    require(FrameSeq(Sent().size() - 1) == 4,
            "owner stream remains monotonic after cached duplicate replay");
    Handle(StopFrame(4));
}

void test_lesson_asset_reservation_retained_across_runtime_and_terminal_release() {
    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1));
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "prepare retains reservation");
    Handle(StartFrame(2));
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "start retains reservation");
    Handle(PauseFrame(3));
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "pause retains reservation");
    Handle(ResumeFrame(4));
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "resume retains reservation");
    Handle(StopFrame(5));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "completed stop releases reservation");

    for (const std::string reason : {"\"reason\":\"CANCELLED\"", "\"reason\":\"FAILED\""}) {
        ResetObservable();
        FreshSession();
        Handle(PrepareFrame(1));
        Handle(StopFrame(2, reason));
        require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
                "cancelled/failed stop releases reservation");
    }

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1));
    Handle(ErrorFrame(2));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "terminal lesson_error releases reservation");

    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    const int failing_step_sequence = OpenSession();
    std::string bad_contract_step =
        StepFrame(failing_step_sequence, "bad-contract", "", "", "");
    const size_t protocol_pos = bad_contract_step.find(kLessonProtocolVersion);
    require(protocol_pos != std::string::npos, "bad-contract fixture finds protocol token");
    bad_contract_step.replace(protocol_pos, strlen(kLessonProtocolVersion), "WRONG");
    Handle(bad_contract_step);
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "internal contract failure releases reservation");

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1, ",\"preloadResetOnly\":true,\"assignmentVersion\":7"));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "preload reset-only prepare abandons and releases reservation");

    ResetObservable();
    Board::GetInstance().display_ = nullptr;
    const int sequence = OpenSession();
    Handle(StepFrame(sequence, "blank", "", "", "", ",\"prompt\":\"\""));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "terminal no-visible-content step releases reservation");
}

void test_abandon_lesson_storage_session_noop_failure_and_success() {
    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1));
    Handle(StopFrame(2));
    require(!App().AbandonLessonStorageSession(),
            "abandon without an active lesson generation is a no-op");

    ResetObservable();
    FreshSession();
    Handle(PrepareFrame(1));
    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    require(!App().AbandonLessonStorageSession(),
            "abandon reports failure when the coordinator already ended the owner");

    FreshSession();
    Handle(PrepareFrame(1));
    require(App().AbandonLessonStorageSession(),
            "abandon releases the matching active lesson generation");
    require(!App().lesson_runtime_active,
            "successful abandon clears the lesson runtime state");
    auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
    require(static_cast<bool>(mutation),
            "storage mutation can acquire immediately after successful abandon");
}

void test_renderer_v5_transport_abandon_releases_lesson_mode_and_storage() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);

    Handle(V5PrepareFrame(1));
    Handle(V5Frame("lesson_start", 2,
        "{\"cinematicPhase\":{\"command\":\"start\",\"phaseId\":\"flyIn\","
        "\"commandSequenceId\":92}}"));
    require(App().lesson_runtime_active && renderer.prepared(),
            "renderer-v5 step owns lesson mode before transport loss");
    require(!display.lesson_mode_calls.empty() && display.lesson_mode_calls.back(),
            "renderer-v5 step hides the normal conversation face");
    App().BeginLessonTerminalAudioQuiet();

    require(App().AbandonLessonStorageSession(),
            "current transport abandonment releases the renderer-v5 owner");
    require(!App().lesson_runtime_active,
            "transport abandonment clears the firmware lesson-mode latch");
    require(!renderer.prepared() && fake.closes == 1 && fake.frees == 4,
            "transport abandonment discards renderer-v5 resources");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "transport abandonment releases storage ownership");
    require(!display.background_calls.empty() && !display.background_calls.back() &&
                !display.object_calls.empty() && !display.object_calls.back() &&
                !display.overlay_calls.empty() && !display.overlay_calls.back(),
            "transport abandonment clears all lesson layers");
    require(!display.lesson_mode_calls.empty() && !display.lesson_mode_calls.back(),
            "transport abandonment restores the normal conversation face");
    require(App().lesson_terminal_audio_quiet,
            "transport abandonment preserves late terminal TTS quarantine");
    auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
    require(static_cast<bool>(mutation),
            "normal MCP and SD mutation reacquires immediately after cleanup");

    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    Board::GetInstance().display_ = nullptr;
}

void test_renderer_v5_transport_abandon_retries_storage_after_reader_release() {
    ResetObservable();
    FreshSession();
    LvglDisplay display;
    Board::GetInstance().display_ = &display;
    V3RendererFake fake;
    tbot::LessonLayeredCinematicRenderer renderer(
        {&fake, V3Allocate, V3Free, V5DecodeJpeg, V5DecodePng,
         V3Open, V3Close, V3Decode, V3Present, V3LastError, V3MonotonicMs});
    tbot::SetActiveLessonLayeredCinematicRenderer(&renderer);

    Handle(V5PrepareFrame(1));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(AID(), SID());
    require(owner.acquired && owner.idempotent,
            "renderer-v5 disconnect retry test owns the prepared asset session");
    auto retained = LessonAssetStorageCoordinator::GetInstance().TryRetainLessonSession(
        AID(), SID(), owner.generation);
    require(static_cast<bool>(retained),
            "renderer-v5 disconnect retry test holds a blocking read lease");

    require(!App().AbandonLessonStorageSession(),
            "transport abandonment reports the temporarily blocked exact release");
    require(!App().lesson_runtime_active && !renderer.prepared(),
            "blocked storage release still clears renderer and runtime ownership immediately");
    require(!display.lesson_mode_calls.empty() && !display.lesson_mode_calls.back(),
            "blocked storage release still restores the normal conversation face");
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "blocked storage release retains coordinator ownership for an exact retry");

    retained = {};
    require(App().AbandonLessonStorageSession(),
            "transport abandonment retries the retained exact storage identity");
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "retry releases storage ownership after the blocking reader exits");
    auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
    require(static_cast<bool>(mutation),
            "normal MCP and SD mutation reacquires after the bounded cleanup retry");

    tbot::SetActiveLessonLayeredCinematicRenderer(nullptr);
    Board::GetInstance().display_ = nullptr;
}

void test_lesson_asset_reservation_foreign_and_stale_terminal_cannot_release() {
    ResetObservable();
    FreshSession();
    const std::string owner_assignment = AID();
    const std::string owner_session = SID();
    Handle(PrepareFrame(1));
    auto owner = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        owner_assignment, owner_session);
    require(owner.acquired && owner.idempotent, "owner prepare reserved storage");

    Handle(StopFrameFor("foreign-assignment", "foreign-session", 2));
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "foreign stop cannot release owner");
    require(!LessonAssetStorageCoordinator::GetInstance().EndLessonSession(
                owner_assignment, owner_session, owner.generation + 1),
            "wrong generation cannot release owner");
    require(LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "wrong generation leaves owner active");

    Handle(StopFrame(2));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "owner terminal releases exact generation");
    Handle(StopFrame(2));
    require(!LessonAssetStorageCoordinator::GetInstance().HasLessonSession(),
            "duplicate terminal does not double-release or recreate state");
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

    Handle(StopFrame(seq + 1));
    seq = OpenMotionEnabledSession();
    App().robot_uart_.calls.clear();
    Handle(StepFrame(seq, "motion-on", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"celebrate\"}"));
    require(!App().robot_uart_.calls.empty(), "manifest control enables named motion");

    Handle(StopFrame(seq + 1));
    seq = OpenSession();
    App().robot_uart_.calls.clear();
    Handle(StepFrame(seq, "motion-reset", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"celebrate\"}"));
    require(App().robot_uart_.calls.empty(), "fresh manifest resets motion control to disabled");
}

void test_step_evidence_telemetry_and_privacy_safe_logs() {
    LvglDisplay disp;
    NetworkInterface net;

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    int seq = OpenMotionEnabledSession();
    Handle(StepFrame(seq, "motion-success", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"teach\"}", "", "modeling"));
    const size_t success_ack = Sent().size() - 1;
    require(Sent()[success_ack].find("\"renderDegraded\":false") != std::string::npos,
            "complete visual render explicitly reports telemetry.renderDegraded=false");
    require(!FrameBodyTelemetryBool(success_ack, "renderDegraded", true),
            "complete visual render has visual-only degradation false");
    require(FrameBodyStr(success_ack, "telemetry", "motionDispatch") == "success",
            "applied UART preset reports telemetry.motionDispatch=success");
    require(LogContains("lesson_step_started assignmentId=" + std::string(AID()) +
                        " sessionId=" + SID() +
                        " lessonId=L1 lessonVersion=3 stepId=motion-success sequence=3"),
            "step-start evidence log carries canonical identity fields");
    require(LogContains("motion_preset outcome=success"),
            "successful preset emits canonical outcome log");
    require(LogContains("lesson_ack TX assignmentId=" + std::string(AID()) +
                        " sessionId=" + SID() +
                        " stepId=motion-success body.acks=3 rendered=true degraded=false "
                        "robotState=modeling"),
            "firmware logs privacy-safe canonical ack payload evidence");
    require(!LogContains("preset=teach") && !LogContains("angle=") &&
                !LogContains("transcript="),
            "evidence logs omit raw motion arguments and transcripts");
    require(!LogContains("motion_degraded"),
            "successful preset does not emit motion_degraded marker");
    require(!LogContains("render_degraded"),
            "complete visual render does not emit render_degraded marker");
    require(!LogContains("optional_asset_missing"),
            "complete visual render does not emit optional_asset_missing marker");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    seq = OpenSession();
    Handle(StepFrame(seq, "unsafe-state", "http://poster", "http://object", "http://overlay",
                     "", "", "modeling\\ntranscript=private-child-text"));
    const size_t unsafe_state_ack = Sent().size() - 1;
    require(FrameBodyStr(unsafe_state_ack, nullptr, "robotState").empty(),
            "noncanonical robotState is not echoed into the ack body");
    require(!LogContains("private-child-text") && !LogContains("transcript="),
            "control characters cannot inject child text into ack evidence logs");

    ResetObservable();
    FreshSession();
    Handle(std::string("{\"type\":\"lesson_prepare\",\"protocolVersion\":\"") +
           kLessonProtocolVersion +
           "\",\"assignmentId\":\"unsafe\\ntranscript=private-child-text\","
           "\"sessionId\":\"safe-session\",\"sequence\":1,"
           "\"body\":{\"profile\":\"" + kLessonProfileEspTft + "\"}}");
    require(LogContains("lesson_ack TX assignmentId=invalid sessionId=safe-session"),
            "invalid identity tokens are replaced instead of logged verbatim");
    require(!LogContains("private-child-text") && !LogContains("transcript="),
            "identity log sanitization prevents newline injection and transcript leakage");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    seq = OpenMotionEnabledSession();
    App().robot_uart_.send_ok = false;
    Handle(StepFrame(seq, "motion-failed", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"teach\"}"));
    const size_t failed_ack = Sent().size() - 1;
    require(Sent()[failed_ack].find("\"renderDegraded\":false") != std::string::npos,
            "motion-only failure explicitly keeps telemetry.renderDegraded=false");
    require(!FrameBodyTelemetryBool(failed_ack, "renderDegraded", true),
            "motion-only failure is not visual degradation");
    require(FrameBodyStr(failed_ack, "telemetry", "motionDispatch") == "failed",
            "UART preset failure reports telemetry.motionDispatch=failed");
    require(FrameBodyBool(failed_ack, "degraded", false),
            "UART preset failure preserves degraded ack semantics");
    require(LogContains("motion_preset outcome=failed"),
            "failed preset emits canonical outcome log");
    require(LogContains("motion_degraded"),
            "failed preset emits motion_degraded only-when-true marker");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    seq = OpenSession();
    Handle(StepFrame(seq, "motion-skipped", "http://poster", "http://object", "http://overlay",
                     ",\"motion\":{\"present\":\"teach\"}"));
    const size_t skipped_ack = Sent().size() - 1;
    require(FrameBodyStr(skipped_ack, "telemetry", "motionDispatch") == "skipped",
            "disabled motion control reports telemetry.motionDispatch=skipped");
    require(LogContains("motion_preset outcome=skipped"),
            "disabled preset emits canonical skipped outcome log");
    require(!LogContains("motion_degraded"),
            "intentional motion skip does not emit motion_degraded");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    seq = OpenSession();
    Handle(StepFrame(seq, "no-motion", "http://poster", "http://object", "http://overlay"));
    require(Sent().back().find("\"motionDispatch\"") == std::string::npos,
            "step without authored motion omits telemetry.motionDispatch");
    require(!LogContains("motion_preset"),
            "step without authored motion does not claim a motion preset outcome");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    seq = OpenSession();
    Handle(StepFrame(seq, "optional-missing", "http://poster", "", ""));
    const size_t optional_missing_ack = Sent().size() - 1;
    require(Sent()[optional_missing_ack].find("\"renderDegraded\":true") != std::string::npos,
            "missing visual layers explicitly report telemetry.renderDegraded=true");
    require(FrameBodyTelemetryBool(optional_missing_ack, "renderDegraded", false),
            "missing visual layers set visual-only degradation true");
    require(LogContains("render_degraded"),
            "visual fallback emits render_degraded only-when-true marker");
    require(LogContains("optional_asset_missing"),
            "missing optional layers emit canonical optional_asset_missing marker");

    ResetObservable();
    Board::GetInstance().display_ = &disp;
    Board::GetInstance().network_ = &net;
    ResetHostHttp(); HostHttp().body = JpegBody(); HostJpegDecodeMode() = 0;
    seq = OpenMotionEnabledSession();
    App().robot_uart_.send_ok = false;
    Handle(StepFrame(seq, "motion-and-visual-failed", "http://poster", "", "",
                     ",\"motion\":{\"present\":\"teach\"}"));
    const size_t simultaneous_ack = Sent().size() - 1;
    require(FrameBodyStr(simultaneous_ack, "telemetry", "degradedReason") == "motionPreset",
            "legacy degradedReason priority remains motionPreset for simultaneous failures");
    require(Sent()[simultaneous_ack].find("\"renderDegraded\":true") != std::string::npos,
            "simultaneous motion and visual failure exposes visual-only degradation");
    require(FrameBodyTelemetryBool(simultaneous_ack, "renderDegraded", false),
            "simultaneous failure keeps render degradation observable");
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

void test_duplicate_prepare_replays_cached_ack_summary_when_history_is_unavailable() {
    ResetObservable();
    FreshSession();
    Board::GetInstance().display_ = nullptr;
    const std::string ready_file_name = "tbot-task8-cached-last-ready.bin";
    const std::string ready_pack = ReadyAssetPackExtra(
        "ck-task8-cached-last-abcdef1234567890",
        "abcdef1234567890",
        ready_file_name);
    Handle(PrepareFrame(1, ready_pack));
    require(FrameType(0) == "lesson_ack" && FrameHasAssetPack(0) && FrameAssetPackReady(0),
            "baseline prepare caches a ready assetPack ACK body");

    tbot::ClearLessonAckReplayHistoryForTest();
    Handle(PrepareFrame(1, ready_pack));
    RemoveReadyAssetPackFixture(ready_file_name);
    require(Sent().size() == 2 &&
                FrameType(1) == "lesson_ack" &&
                FrameBodyNum(1, "acks") == 1 &&
                !FrameBodyBool(1, "rendered", true) &&
                !FrameBodyBool(1, "degraded", true) &&
                FrameHasAssetPack(1) &&
                FrameAssetPackReady(1),
            "duplicate prepare falls back to cached last ACK summary and assetPack when replay history is unavailable");
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
    test_embodied_action_capability_and_async_terminal_ack();
    test_embodied_action_reduced_motion_and_partial_servo_degrade();
    test_embodied_action_cancel_duplicate_and_supersession_are_safe();
    test_embodied_action_fail_closed_capability_focus_and_timer_failure();
    test_embodied_software_journeys_16_through_20();
    test_embodied_lifecycle_failure_and_teardown_paths();
    test_embodied_ledger_mastery_cap_and_no_return_settle();
    test_safe_motion_presets_and_auto_rest();
    test_motion_unknown_raw_failures_and_stale_rest_are_nonfatal_degrades();
    test_queued_old_timer_callback_cannot_rest_a_new_pose_early();
    test_renderer_v4_trgb_exact_asset_contract();
    test_envelope_guards();
    test_prepare_basic();
    test_renderer_v2_capability_shape_and_exact_tokens();
    test_renderer_v3_capability_is_fail_closed_until_initialized();
    test_cinematic_rejects_unsupported_frames_and_accepts_all_late_phases();
    test_renderer_v3_routes_lesson_step_after_cinematic_start();
    test_cinematic_renderer_failures_use_stable_error_mapping();
    test_cinematic_prepare_reservation_refusal_and_v3_rejection_cleanup();
    test_renderer_v5_capability_exact_layers_and_lifecycle();
    test_renderer_v5_course_mode_activity_fallback_without_object();
    test_renderer_v5_first_course_prepare_reports_static_degradation();
    test_course_activity_durable_outcome_replay_and_write_failure();
    test_course_activity_reconciles_outcome_and_removal_write_failures();
    test_course_activity_failed_thirteenth_claim_restores_evicted_oldest();
    test_course_activity_retains_w19_static_layers_and_dedupes_delivery();
    test_course_activity_delivery_persistence_reload_bounds_and_corruption();
    test_course_activity_handler_persistence_round_trip_after_reboot();
    test_renderer_v5_course_mode_activity_fallback_rejects_manifest_identity_mix();
    test_renderer_v5_dynamic_course_mode_requires_ready_matching_asset_pack();
    test_renderer_v4_capability_and_exact_single_asset_routing();
    test_renderer_v4_accepts_template_v2_cue_identity_prepare_and_controls();
    test_renderer_v4_course_mode_compatibility_is_exact_and_narrow();
    test_renderer_v4_accepts_external_flattened_cue_metadata_matrix();
    test_renderer_v4_numeric_narrowing_rejects_before_renderer_work();
    test_renderer_v4_template_v2_exact_cue_schema_and_ack_identity();
    test_tvideo_farm_cross_repository_fixture_runs_prepare_start_through_handler();
    test_renderer_v4_fresh_prepare_resets_session_sequence_stream();
    test_renderer_v4_failed_same_session_reprepare_keeps_session_playable();
    test_cinematic_controls_cannot_cross_renderer_session_identity();
    test_cinematic_cross_renderer_handoff_releases_old_resources();
    test_layered_cinematic_coverage_boundaries();
    test_cinematic_cross_renderer_handoff_fails_closed_without_old_renderer();
    test_renderer_v4_lesson_stop_routes_to_flattened_renderer();
    test_cinematic_terminal_waits_for_asset_lease_release();
    test_renderer_v3_exact_typed_ack_lifecycle_and_idempotency();
    test_renderer_v3_controls_reject_v4_only_fields();
    test_renderer_v3_rejections_use_task7_consumed_lesson_error();
    test_renderer_v3_duplicate_sequence_requires_exact_original_command();
    test_renderer_v3_json_safe_sequence_boundaries_and_ack_oom();
    test_generic_lesson_json_failures_drop_partial_frames_and_clean_up();
    test_buildframe_failure_does_not_consume_outbound_sequence();
    test_buildframe_missing_required_protocol_sends_no_partial_error();
    test_renderer_v2_valid_opening_entrance_contract_acks_center_road();
    test_renderer_v2_valid_visual_state_contract_enqueues_static_completion();
    test_renderer_v2_contracts_reject_unexpected_and_duplicate_keys();
    test_renderer_v2_start_and_visual_contracts_fail_closed();
    test_renderer_v2_opening_layouts_and_visual_generation_contracts_fail_closed();
    test_renderer_v2_worker_dispatch_emits_exactly_one_ack();
    test_renderer_v2_production_render_callback_reaches_worker_ack();
    test_renderer_v2_duplicate_visual_waits_for_original_completion();
    test_renderer_v2_visual_completion_nonce_wrap_skips_zero();
    test_renderer_v2_invalid_visual_completion_result_rejects_fail_closed();
    test_renderer_v2_non_lvgl_display_rejects_start_completion();
    test_renderer_v2_start_ack_is_serialized_before_early_step_ack();
    test_renderer_v2_start_ack_is_serialized_before_early_visual_ack();
    test_renderer_v2_old_completion_cannot_claim_reused_identity();
    test_renderer_v2_visual_outside_running_session_is_dropped();
    test_renderer_v2_repeated_start_acks_once_and_resume_restores_teach_state();
    test_renderer_v2_verified_opening_assets_and_identity_mismatch();
    test_renderer_v2_stale_visual_callback_and_non_lvgl_degraded_completion();
    test_prepare_assetpack_not_ready_branches();
    test_prepare_assetpack_ready_with_real_file();
    test_prepare_assetpack_accepts_large_cinematic_without_raising_image_ram_cap();
    test_prepare_assetpack_accepts_exact_production_trgb_pack_and_rejects_oversize();
    test_prepare_assetpack_derives_local_path_from_root_and_key();
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
    test_lesson_asset_reservation_blocks_prepare_before_asset_io();
    test_lesson_asset_reservation_duplicate_and_foreign_prepare();
    test_normal_prepare_consumes_one_storage_reservation_attempt_and_generation();
    test_prepare_pure_contract_validation_precedes_storage_reservation();
    test_prepare_sequence_zero_is_pure_rejection_without_generation_burn();
    test_lesson_transport_rejects_decoded_nul_before_cjson_truncation();
    test_lesson_asset_reservation_refusal_mapping_is_total();
    test_lesson_asset_reservation_invalid_and_exhausted_prepare();
    test_lesson_asset_reservation_prepare_failure_release_ownership();
    test_not_ready_asset_candidate_does_not_commit_lesson_session();
    test_isolated_not_ready_asset_prepare_ack_json_failure_sends_no_empty_frame();
    test_republished_not_ready_candidate_uses_isolated_stream();
    test_lesson_asset_reservation_retained_across_runtime_and_terminal_release();
    test_abandon_lesson_storage_session_noop_failure_and_success();
    test_renderer_v5_transport_abandon_releases_lesson_mode_and_storage();
    test_renderer_v5_transport_abandon_retries_storage_after_reader_release();
    test_lesson_asset_reservation_foreign_and_stale_terminal_cannot_release();
    test_prepare_reject_preserves_active_lesson_scene();
    test_version_profile_gate();
    test_fresh_prepare_contract_reject_clears_stale_lesson_scene();
    test_unknown_session_and_staleness();
    test_dedup_reack();
    test_delayed_duplicate_prepare_replays_assetpack_after_start_ack();
    test_prepare_new_assignment_version_same_session_resets_stream();
    test_fresh_prepare_clears_stale_active_lesson_before_start();
    test_preload_reset_prepare_quiesces_without_arming_lesson_start();
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
    test_step_rejects_authored_motion_media_sources_and_mime_types();
    test_invalid_step_clears_stale_lesson_scene();
    test_contract_reject_clears_stale_lesson_scene();
    test_step_full_render_http();
    test_tvideo_first_step_applies_named_arrived_geometry_and_reports_fallback();
    test_tvideo_malformed_projection_uses_safe_unsupported_contract_fallback();
    test_tvideo_requires_pinned_arrived_pose_linkage();
    test_tvideo_is_restricted_to_the_true_first_lesson_step();
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
    test_step_reads_only_body_motion_present_and_motion_degrades_ack();
    test_motion_runtime_control_defaults_disabled_and_resets_per_manifest();
    test_step_evidence_telemetry_and_privacy_safe_logs();
    test_teaching_word_telemetry_reuse_and_duplicate_ack_parity();
    test_ack_replay_window_handles_delayed_and_expired_duplicates();
    test_duplicate_prepare_replays_cached_ack_summary_when_history_is_unavailable();
    test_layer_install_timeout_degrades_without_committing_layer_state();
    test_renderer_v2_visual_motion_is_allowlisted_once_per_generation();
    test_renderer_v5_course_mode_exact_identity_and_fail_closed_metadata();
    std::cout << "lesson host test OK (" << g_checks << " checks)\n";
    return 0;
}
