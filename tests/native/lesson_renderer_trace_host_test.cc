#include "application.h"
#include "lesson_tvideo_template.h"

#include <cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef fread
#undef fread
#endif
#include <stdio.h>
extern "C" size_t fread(void* ptr, size_t size, size_t count, FILE* stream);

#ifdef fopen
#undef fopen
#endif
extern "C" FILE* fopen(const char* path, const char* mode);

extern "C" FILE* HostLessonFopen(const char* path, const char* mode) {
    return ::fopen(path, mode);
}

extern "C" size_t HostLessonFread(void* ptr, size_t size, size_t count, FILE* stream) {
    return ::fread(ptr, size, count, stream);
}

// The runner copies lesson_handler.cc beside this file before compilation. Including
// the production unit lets this host exporter exercise its file-local ACK mapping.
#include "lesson_handler.cc"

namespace {

[[noreturn]] void Fail(const std::string& message) {
    std::cerr << "renderer v2 firmware trace: " << message << "\n";
    std::exit(1);
}

const cJSON* Object(const cJSON* parent, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(parent, key);
    return cJSON_IsObject(value) ? value : nullptr;
}

const cJSON* Array(const cJSON* parent, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(parent, key);
    return cJSON_IsArray(value) ? value : nullptr;
}

const char* String(const cJSON* parent, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(parent, key);
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

std::uint64_t Uint64(const cJSON* parent, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0) Fail(std::string("invalid ") + key);
    return static_cast<std::uint64_t>(value->valuedouble);
}

void AddNullableString(cJSON* object, const char* key, const char* value) {
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, "none") == 0) {
        cJSON_AddNullToObject(object, key);
    } else {
        cJSON_AddStringToObject(object, key, value);
    }
}

struct AckFields {
    bool accepted = false;
    bool degraded = false;
    std::string reason;
};

AckFields MapAck(LessonVisualCompletionResult result, const char* reason,
                 std::uint64_t generation, bool timeout = false) {
    g_session = LessonSession{};
    g_session.current_transport_epoch = 1;
    g_session.visual_generation = generation;
    g_session.assignment_id = "trace-assignment";
    g_session.session_id = "trace-session";
    g_session.pending_ack = true;
    g_session.pending_server_sequence = 9;
    g_session.pending_visual_nonce = 11;
    g_session.pending_protocol_version = kLessonRendererV2;
    g_session.pending_step_id = "trace-step";

    const LessonQueueItem item = MakeLessonVisualQueueItem(
        timeout ? LessonQueueItemKind::kVisualTimedOut : LessonQueueItemKind::kVisualCompleted,
        1, generation, 9, "trace-assignment", "trace-session", "trace-step",
        result, reason, 11);
    std::string frame;
    if (!AcceptLessonVisualCompletion(item, &frame)) Fail("production ACK mapping rejected trace completion");

    cJSON* root = cJSON_Parse(frame.c_str());
    const cJSON* body = root == nullptr ? nullptr : Object(root, "body");
    if (body == nullptr) Fail("production ACK mapping emitted invalid JSON");
    AckFields fields;
    fields.accepted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "accepted"));
    fields.degraded = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "degraded"));
    const cJSON* degraded_reason = cJSON_GetObjectItemCaseSensitive(body, "degradedReason");
    if (cJSON_IsString(degraded_reason)) fields.reason = degraded_reason->valuestring;
    cJSON_Delete(root);
    return fields;
}

cJSON* RectJson(const lesson_tvideo::Rect& rect) {
    cJSON* value = cJSON_CreateObject();
    cJSON_AddNumberToObject(value, "x", rect.left);
    cJSON_AddNumberToObject(value, "y", rect.top);
    cJSON_AddNumberToObject(value, "width", rect.width);
    cJSON_AddNumberToObject(value, "height", rect.height);
    return value;
}

void AddTraceRow(cJSON* rows, const char* boundary, const lesson_tvideo::StateMachine& state,
                 const char* overlay, std::uint64_t generation, const char* visual_state,
                 const char* motion, const AckFields& ack) {
    cJSON* row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "boundary", boundary);
    cJSON_AddStringToObject(row, "phase", lesson_tvideo::PhaseName(static_cast<std::uint8_t>(state.phase())));
    cJSON_AddItemToObject(row, "bounds", RectJson(state.geometry().robot));
    cJSON_AddBoolToObject(row, "contentVisible", state.content_visible());
    AddNullableString(row, "overlay", overlay);
    cJSON_AddNumberToObject(row, "generation", static_cast<double>(generation));
    cJSON_AddStringToObject(row, "state", visual_state);
    cJSON_AddStringToObject(row, "motion", motion);
    cJSON* ack_json = cJSON_CreateObject();
    cJSON_AddBoolToObject(ack_json, "accepted", ack.accepted);
    cJSON_AddBoolToObject(ack_json, "degraded", ack.degraded);
    AddNullableString(ack_json, "reason", ack.reason.c_str());
    cJSON_AddItemToObject(row, "ack", ack_json);
    cJSON_AddItemToArray(rows, row);
}

lesson_tvideo::Config ConfigFor(const cJSON* projection, bool arrived, bool atlas,
                                bool reduced, bool unsupported = false) {
    return {
        String(projection, "templateId"),
        static_cast<std::uint8_t>(unsupported ? 2 : Uint64(projection, "templateVersion")),
        String(projection, "layoutPreset"),
        static_cast<std::uint8_t>(Uint64(projection, "geometryVersion")),
        arrived,
        atlas,
        reduced,
    };
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || argv[1][0] == '\0') Fail("usage: exporter FIXTURE.json");
    std::ifstream stream(argv[1]);
    if (!stream) Fail(std::string("cannot read fixture: ") + argv[1]);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    cJSON* fixture = cJSON_Parse(buffer.str().c_str());
    if (fixture == nullptr) Fail("fixture is not valid JSON");

    const cJSON* steps = Array(fixture, "steps");
    const cJSON* step = steps == nullptr ? nullptr : cJSON_GetArrayItem(steps, 0);
    const cJSON* projection = step == nullptr ? nullptr : Object(step, "templateProjection");
    const cJSON* trace = Object(fixture, "trace");
    const cJSON* boundaries = trace == nullptr ? nullptr : Array(trace, "boundaries");
    const cJSON* fallbacks = trace == nullptr ? nullptr : Array(trace, "fallbacks");
    if (projection == nullptr || boundaries == nullptr || fallbacks == nullptr) Fail("fixture schema is incomplete");

    const char* state_name = String(trace, "state");
    const char* motion = String(trace, "motion");
    const char* overlay = String(trace, "overlay");
    const std::uint64_t generation = Uint64(trace, "visualGeneration");
    if (VisualStateKind(state_name) != LessonVisualStateKind::kTeach) Fail("fixture state is not firmware teach");
    RobotUart robot_uart;
    if (DispatchLessonMotionPreset(robot_uart, motion) != LessonMotionResult::kApplied) Fail("fixture motion is not firmware-safe");

    cJSON* document = cJSON_CreateObject();
    cJSON_AddStringToObject(document, "schemaVersion", String(fixture, "schemaVersion"));
    cJSON_AddStringToObject(document, "manifestVersion", kLessonRendererV2);
    cJSON_AddStringToObject(document, "protocolVersion", kLessonRendererV2);
    cJSON* rows = cJSON_CreateArray();
    cJSON_AddItemToObject(document, "trace", rows);

    lesson_tvideo::StateMachine animation(ConfigFor(projection, true, true, false));
    const AckFields applied = MapAck(LessonVisualCompletionResult::kApplied, nullptr, generation);
    cJSON* boundary = nullptr;
    cJSON_ArrayForEach(boundary, boundaries) {
        animation.Advance(static_cast<std::uint32_t>(Uint64(boundary, "advanceMs")));
        AddTraceRow(rows, String(boundary, "name"), animation, overlay, generation,
                    state_name, motion, applied);
    }

    cJSON* fallback = nullptr;
    cJSON_ArrayForEach(fallback, fallbacks) {
        const char* mode = String(fallback, "mode");
        const bool missing = std::strcmp(mode, "missingOverlay") == 0;
        const bool reduced = std::strcmp(mode, "reducedMotion") == 0;
        const bool timeout = std::strcmp(mode, "phaseTimeout") == 0;
        const bool unsupported = std::strcmp(mode, "unsupportedContract") == 0;
        lesson_tvideo::StateMachine fallback_state(ConfigFor(projection, !missing, true, reduced, unsupported));
        if (timeout) fallback_state.Timeout();
        const char* reason = lesson_tvideo::DegradedReasonName(fallback_state.degraded_reason());
        LessonVisualCompletionResult result = LessonVisualCompletionResult::kDegraded;
        if (timeout) result = LessonVisualCompletionResult::kPhaseTimeout;
        if (unsupported) result = LessonVisualCompletionResult::kRejected;
        const AckFields ack = MapAck(result, reason, generation, timeout);
        AddTraceRow(rows, String(fallback, "name"), fallback_state,
                    missing || unsupported ? nullptr : overlay, generation,
                    state_name, motion, ack);
    }

    char* serialized = cJSON_PrintUnformatted(document);
    if (serialized == nullptr) Fail("could not serialize trace");
    std::cout << serialized << '\n';
    cJSON_free(serialized);
    cJSON_Delete(document);
    cJSON_Delete(fixture);
    return 0;
}
