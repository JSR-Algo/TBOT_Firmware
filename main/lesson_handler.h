#ifndef LESSON_HANDLER_H
#define LESSON_HANDLER_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "lesson_transport_epoch_gate.h"

struct cJSON;
class Protocol;

// US-006 Slice-01 — additive lesson_* renderer (LANE-FIRMWARE / S10 / CP-6).
//
// Frozen wire contract (S2/CP-2):
//   robot/docs/stories/US-006-learning-course-runtime/fixtures/lesson-protocol.v1.json
// Decisions: robot/docs/decisions/0013-lesson-slice-01-gate-b-resolutions.md
//   §C D-PRELOAD-OWNER, §I D-CAP-FLAG, §J D-LV (lessonVersion integer wire type),
//   §M D-ATLAS-CRITICAL (Layer-3 = emoji-face v1, no atlas; poses optional).
//
// Entry point: Application::HandleLessonMessage(const cJSON*) — declared in
// application.h, implemented in lesson_handler.cc. It is reached ONLY via the
// additive `else if (strncmp(type->valuestring,"lesson_",7)==0)` branch in the
// OnIncomingJson dispatch ladder, which sits immediately ABOVE the unknown-type
// no-op. Un-upgraded firmware (without this branch) keeps hitting that no-op and
// drops lesson_* silently — the backward-compat contract (protocol §2, ADR §I).
//
// Capability gate (D-CAP-FLAG): the firmware advertises support in hello.features
// ({"lesson":true,"renderer":"teebot-lesson-renderer.v1"}); absence == no support.
// Contract negotiation is an EXACT-STRING match (NOT a semver) on protocolVersion.
//
// Asset bytes / image render (D-PRELOAD-OWNER, ADR §C): the ESP Server remains the
// authoritative sha256 VERIFIER and gates READY — the firmware does NOT checksum or
// verify asset bytes and never owns the preload/READY decision. It DOES, at render
// time, FETCH the already-ESP-verified bytes (see US-006 IMAGE RENDER below):
// FetchLessonImage HTTP-GETs scene.*.src via Board::GetNetwork()->CreateHttp(), so the
// firmware is a render-time downloader of verified bytes, NOT the authoritative
// downloader/verifier. The LVGL PNG/JPEG decoder is
// compiled in (CONFIG_LV_USE_LODEPNG=y, sdkconfig.defaults:62) and a full
// download->decode->draw path already ships (mcp_server.cc preview_image ->
// LvglAllocatedImage -> LcdDisplay::SetPreviewImage).
//
// US-006 IMAGE RENDER (IMPLEMENTED): the lesson_step renderer now FETCHES the authored
// poster/object source (scene.*.src, already carried in the lesson_step body) over HTTP
// or reads a verified local SD/file path, then DECODES it, REUSING that proven pipeline:
//   FetchLessonImage(src) [lesson_handler.cc] = HTTP GET or sd:// file read ->
//   heap_caps_malloc -> LvglAllocatedImage  (mirrors mcp_server.cc:332-361),
// then draws it full-screen via the NEW LcdDisplay::SetLessonBackground — a PERSISTENT
// (non-auto-hiding) background, distinct from the centered 5s preview_image_
// (PREVIEW_IMAGE_DURATION_MS). The verified bytes the ESP server already downloaded are
// served back over the network or referenced by their SD pack paths. Failure is
// non-fatal: FetchLessonImage returns nullptr on any error and the ladder falls back
// to the caption-only render (no crash). The teachingObject PNG uses the same fetch/decode
// path and draws through LcdDisplay::SetLessonObject as a foreground layer. Background
// and foreground are cleared on lesson_stop and caption-only fallback so stale lesson
// media never lingers into idle realtime.
//
// VIDEO is OUT OF SCOPE on espTft (poster-only). A non-poster bg.mode or a non-null
// bg.video is hard-rejected with ASSET_PROFILE_UNAVAILABLE (lesson_handler.cc) — lifting
// that gate would additionally require a video pipeline that does not exist; treat it as
// a separate, larger firmware effort. All of the above ships only via a firmware
// rebuild + reflash/OTA.

// Frozen contract identity (fixture _meta.protocolVersion). Single source of truth
// shared by the renderer and the hello.features advertisement.
constexpr char kLessonRendererV1[] = "teebot-lesson-renderer.v1";
constexpr char kLessonRendererV2[] = "teebot-lesson-renderer.v2";
constexpr char kLessonProtocolVersion[] = "teebot-lesson-renderer.v1";
constexpr char kLessonRendererName[] = "teebot-lesson-renderer.v1";
constexpr std::size_t kLessonMessageDataQueueDepth = 16;

void AddLessonRendererFeatures(cJSON* features);
namespace tbot {
void SetLessonCinematicJsonFailAfterForTest(int successful_operations_before_failure);
#ifdef TBOT_HOST_NATIVE_COVERAGE
void SetLessonJsonFailAfterForTest(int successful_operations_before_failure);
#endif
}

enum class LessonQueueItemKind : std::uint8_t {
    kFrame,
    kAbandonTransport,
    kVisualCompleted,
    kVisualTimedOut,
};

enum class LessonVisualCompletionResult : std::uint8_t {
    kApplied,
    kDegraded,
    kRejected,
    kPhaseTimeout,
};

// FreeRTOS queues copy items as bytes. Completion identity therefore uses fixed
// buffers and never borrows a parsed frame or cJSON node.
struct LessonQueueItem {
    static constexpr std::size_t kIdentityBytes = 129;
    static constexpr std::size_t kReasonBytes = 32;

    LessonQueueItemKind kind = LessonQueueItemKind::kFrame;
    char* payload = nullptr;
    std::uint64_t transport_epoch = 0;
    std::uint64_t visual_generation = 0;
    std::uint64_t visual_nonce = 0;
    std::int64_t server_sequence = 0;
    char assignment_id[kIdentityBytes] = {};
    char session_id[kIdentityBytes] = {};
    char step_id[kIdentityBytes] = {};
    char degraded_reason[kReasonBytes] = {};
    LessonVisualCompletionResult completion_result = LessonVisualCompletionResult::kRejected;
};

inline LessonQueueItem MakeLessonVisualQueueItem(
    LessonQueueItemKind kind,
    std::uint64_t transport_epoch,
    std::uint64_t visual_generation,
    std::int64_t server_sequence,
    const char* assignment_id,
    const char* session_id,
    const char* step_id,
    LessonVisualCompletionResult result,
    const char* degraded_reason,
    std::uint64_t visual_nonce = 0
) {
    LessonQueueItem item{};
    item.kind = kind;
    item.transport_epoch = transport_epoch;
    item.visual_generation = visual_generation;
    item.visual_nonce = visual_nonce;
    item.server_sequence = server_sequence;
    item.completion_result = result;
    std::snprintf(item.assignment_id, sizeof(item.assignment_id), "%s",
                  assignment_id != nullptr ? assignment_id : "");
    std::snprintf(item.session_id, sizeof(item.session_id), "%s",
                  session_id != nullptr ? session_id : "");
    std::snprintf(item.step_id, sizeof(item.step_id), "%s",
                  step_id != nullptr ? step_id : "");
    std::snprintf(item.degraded_reason, sizeof(item.degraded_reason), "%s",
                  degraded_reason != nullptr ? degraded_reason : "");
    return item;
}

class LessonTransportTerminalControl {
public:
    struct PublishResult {
        bool enqueue_control = false;
    };

    PublishResult Publish(std::uint64_t epoch) {
        std::uint64_t observed = latest_epoch_.load(std::memory_order_relaxed);
        while (observed < epoch &&
               !latest_epoch_.compare_exchange_weak(
                   observed, epoch, std::memory_order_release, std::memory_order_relaxed)) {}
        return {!pending_.exchange(true, std::memory_order_acq_rel)};
    }

    bool WorkerShouldDrain() const {
        return pending_.load(std::memory_order_acquire);
    }

    bool FinishWorkerDrain(
        const LessonTransportEpochGate& gate,
        std::uint64_t applied_epoch
    ) {
        pending_.exchange(false, std::memory_order_acq_rel);
        const std::uint64_t latest_epoch = latest_epoch_.load(std::memory_order_acquire);
        if (latest_epoch <= applied_epoch && gate.PublishedEpoch() <= applied_epoch) return false;
        return !pending_.exchange(true, std::memory_order_acq_rel);
    }

    void CancelQueuedControl() {
        pending_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> pending_{false};
    std::atomic<std::uint64_t> latest_epoch_{0};
};

class LessonQueueDataAdmission {
public:
    explicit LessonQueueDataAdmission(std::size_t capacity) : capacity_(capacity) {}

    bool TryAcquire() {
        std::size_t observed = in_use_.load(std::memory_order_relaxed);
        while (observed < capacity_) {
            if (in_use_.compare_exchange_weak(
                    observed, observed + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void Release() {
        in_use_.fetch_sub(1, std::memory_order_acq_rel);
    }

private:
    const std::size_t capacity_;
    std::atomic<std::size_t> in_use_{0};
};

void SetLessonTransportEpoch(std::uint64_t transport_epoch);
void InvalidateLessonVisualCompletionState(std::uint64_t transport_epoch);
bool AcceptLessonVisualCompletion(const LessonQueueItem& item, std::string* ack_frame);
bool DispatchLessonVisualCompletion(const LessonQueueItem& item, Protocol* protocol);

// The only render profile this firmware (lcdwiki-es3c35p) implements (plan §7).
constexpr char kLessonProfileEspTft[]   = "espTft";

#endif  // LESSON_HANDLER_H
