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
#include "protocol.h"
#include "lesson_handler.h"
// US-006 image render: the on-device LVGL decoder + draw path. LvglDisplay carries
// SetLessonBackground (the new persistent full-screen draw) and LvglAllocatedImage is
// the same decoded-bytes wrapper the proven mcp_server.cc preview path uses.
#include "lvgl_display.h"
#include "lvgl_image.h"

#include <ctime>
#include <cstring>
#include <string>
#include <memory>

#include <cJSON.h>
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
// OnIncomingJson runs on the single protocol receive task, so lesson frames are
// processed sequentially and this state needs no lock. Draws are marshalled to the
// app task via Application::Schedule(). The F->S sequence is the only firmware-owned
// wire state (shared by acks + progress, monotonic per (assignmentId,sessionId),
// starting at 1 — fixture sequenceStreams F->S).
struct LessonSession {
    std::string assignment_id;
    double      assignment_version = -1.0;  // staleness guard (plan §7.2)
    int64_t     last_in_sequence   = 0;     // highest processed S->F sequence (0 = none)
    int64_t     fs_sequence        = 0;     // firmware F->S counter, pre-inc on emit
    bool        prepared           = false;
    bool        running            = false;
    // FW-LESSON-02: the (rendered, degraded) of the ack we emitted for the last
    // processed inbound sequence, so a duplicate re-ack can REPLAY the prior ack body
    // idempotently (protocol §6 / lesson-robot-protocol.md:436-438) instead of
    // hardcoding rendered=false/degraded=false. Slice-01 has a single active step, so
    // caching the last ack's flags keyed on its sequence is sufficient.
    int64_t     last_ack_sequence  = 0;     // S->F sequence this cached ack acked (0 = none)
    bool        last_ack_rendered  = false;
    bool        last_ack_degraded  = false;
};
LessonSession g_session;

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
// ack ONLY (it auto-advances on that ack and the firmware NEVER emits step_completed),
// while an INTERACTIVE step gets an ack AND a lesson_progress step_completed. The
// firmware previously emitted step_completed UNCONDITIONALLY, sending a spurious
// step_completed for every passive step — the very bug the ESP runtime defends against
// (runtime.py:80-88, latch-contamination guard runtime.py:284-300) and whose
// un-mitigated failure mode is off-by-one step-skipping (test_lesson_runtime.py:979).
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

// Layer-3 pose -> a valid EyesEmojiCollection emotion. The sprite-atlas has no
// on-device renderer, so the robot overlay collapses to an emoji-face (the defined
// espTft v1 full render — D-ATLAS-CRITICAL / DIV-FW-ATLAS).
const char* ExpressionToEmotion(const char* expr) {
    if (expr == nullptr) return "neutral";
    if (strcmp(expr, "teaching") == 0 || strcmp(expr, "modeling") == 0)   return "happy";
    if (strcmp(expr, "celebrating") == 0)                                 return "laughing";
    if (strcmp(expr, "thinking") == 0 || strcmp(expr, "listening") == 0)  return "thinking";
    return "neutral";
}

// Is a named asset present in the on-device read-only (build-time) asset image?
// D-PRELOAD-OWNER: the firmware never downloads/checksums; it renders only bytes
// already on-device. In slice-01 the poster/teachingObject PNG are not flashed and
// no ESP->firmware byte channel exists, so this returns false for them and the
// ladder degrades to the always-available rungs (see lesson_handler.h).
bool AssetAvailable(const char* name) {
    if (name == nullptr) return false;
    void* ptr = nullptr;
    size_t size = 0;
    return Assets::GetInstance().GetAssetData(std::string(name), ptr, size) && size > 0;
}

// US-006 image render — HTTP GET a lesson image URL and DECODE it into an LvglImage.
// This is the firmware half the recon flagged as missing: the backend already emits
// resolved scene.*.src URLs and the ESP server downloads/sha256-verifies the bytes,
// but the firmware previously only probed the flashed asset image. We now fetch the
// authored URL over the existing network stack and hand the raw bytes to
// LvglAllocatedImage, REUSING the exact proven pipeline in mcp_server.cc:332-361
// (HTTP GET -> heap_caps_malloc -> LvglAllocatedImage). The LVGL PNG/JPEG decoder is
// already compiled in (CONFIG_LV_USE_LODEPNG=y), so LvglAllocatedImage's ctor decodes
// via lv_image_decoder_get_info on first draw.
//
// FAILURE IS NEVER FATAL: any error (no URL, no network, bad status, alloc fail, short
// read) returns nullptr and the caller falls back to the caption-only ladder. We run
// on the protocol receive task (HandleLessonMessage), so this blocking download does
// NOT stall the LVGL/app task; the decoded draw is then marshalled via Schedule().
std::unique_ptr<LvglImage> FetchLessonImage(const char* url) {
    if (url == nullptr || url[0] == '\0') return nullptr;

    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "lesson image fetch: no network");
        return nullptr;
    }
    auto http = network->CreateHttp(3);
    if (!http) return nullptr;

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
    if (content_length == 0) {
        ESP_LOGW(TAG, "lesson image fetch: empty body");
        http->Close();
        return nullptr;
    }
    char* data = static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT));
    if (data == nullptr) {
        ESP_LOGW(TAG, "lesson image fetch: alloc %u failed", (unsigned)content_length);
        http->Close();
        return nullptr;
    }
    size_t total_read = 0;
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

    // LvglAllocatedImage takes ownership of `data` (frees it in its dtor, which runs
    // when the cached unique_ptr in LcdDisplay is replaced/reset).
    return std::make_unique<LvglAllocatedImage>(data, content_length);
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
                            bool cache = true) {
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
        }
        emit(in, "lesson_ack", b);
    };

    const bool is_prepare = strcmp(type, "lesson_prepare") == 0;

    // FW-02: a session-opening lesson_prepare resets the F->S counter + inbound cursor
    // BEFORE the version/profile gate, so that even a REJECTED fresh prepare sources
    // its lesson_error at envelope sequence=1 (the frozen per-session "F->S restarts at
    // 1" contract — fixture sequenceStreams F->S). Without this, a fresh prepare whose
    // profile/protocolVersion is unsupported would emit lesson_error on the STALE prior
    // session's advanced counter (e.g. seq=6), violating restart-at-1 and poisoning the
    // ESP's fresh inbound cursor. Mid-session lesson_step/lesson_start rejects (the case
    // the gate comment below protects) still ride the live continued counter because
    // they do NOT reset here.
    if (is_prepare) {
        // lesson_prepare (re)establishes the single active session and resets the
        // F->S counter + the inbound sequence cursor for this assignment.
        g_session = LessonSession{};
        g_session.assignment_id = assignment_id;
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
        emit(root, "lesson_error", eb);
        ESP_LOGW(TAG, "lesson_* rejected: version_ok=%d profile_ok=%d", version_ok, profile_ok);
        return;
    }

    // --- session context (per (assignmentId,sessionId)) ---
    // The session reset for a session-opening prepare already ran BEFORE the gate
    // (FW-02). Here we only reject non-prepare frames for an unknown assignment.
    if (!is_prepare && g_session.assignment_id != assignment_id) {
        ESP_LOGW(TAG, "lesson_%s for unknown assignment; dropping", type + 7);
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
        ESP_LOGI(TAG, "lesson_* duplicate seq=%lld; re-acking rendered=%d degraded=%d",
                 (long long)sequence, re_rendered, re_degraded);
        emit_ack(root, sequence, re_rendered, re_degraded, /*cache*/ false);
        return;
    }
    g_session.last_in_sequence = sequence;

    // --- slice subset dispatch ---
    if (is_prepare) {
        g_session.prepared = true;
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        return;
    }
    if (strcmp(type, "lesson_start") == 0) {
        g_session.running = true;
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        return;
    }
    if (strcmp(type, "lesson_stop") == 0) {
        g_session.running = false;
        g_session.prepared = false;
        emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false);
        // Return the robot display to its idle realtime state (protocol §4.6): clear any
        // persistent lesson background poster (US-006) and restore the neutral face.
        Display* display = Board::GetInstance().GetDisplay();
        LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (display) {
            Schedule([display, lvgl_display]() {
                if (lvgl_display) lvgl_display->SetLessonBackground(nullptr);
                display->SetEmotion("neutral");
            });
        }
        return;
    }
    if (strcmp(type, "lesson_step") != 0) {
        // lesson_pause/resume (DEFERRED) + F->S/ESP-synth frames (ack/error/
        // progress/preload_status) are not part of the slice command subset.
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
        emit(root, "lesson_error", eb);
        ESP_LOGW(TAG, "lesson_step rejected: video forced on espTft");
        return;
    }

    // Degraded fallback ladder: poster -> teachingObject PNG -> primitive glyph
    // card -> emoji-face + caption. US-006 image render: instead of only probing the
    // on-device flashed asset image (which never holds per-assignment lesson posters),
    // we now FETCH the authored URL (scene.backgroundScene.poster.src) over HTTP and
    // DECODE it, reusing the proven mcp_server.cc download->LvglAllocatedImage path.
    // The fetched bytes are drawn as a full-screen, persistent background via the new
    // LcdDisplay::SetLessonBackground (distinct from the centered 5s auto-hide preview).
    // The teachingObject PNG (object_src) is NOT drawn as a foreground object in this
    // slice — there is one full-screen background draw target; teachingObject still
    // contributes its label/glyph to the caption below. AssetAvailable() is retained as
    // a cheap on-device fallback for the (rare) case a poster is also flashed.
    const char* poster_src = Str(Obj(bg, "poster"), "src");
    const char* object_src = Str(Obj(to, "asset"), "src");

    // Resolve the display once and require it to be an LvglDisplay (the only class with
    // a real image draw path; OledDisplay/NoDisplay get caption-only). dynamic_cast
    // mirrors mcp_server.cc:255 and yields nullptr on non-LVGL boards.
    Display* base_display = Board::GetInstance().GetDisplay();
    LvglDisplay* lvgl_display = dynamic_cast<LvglDisplay*>(base_display);

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
    // On-device fallback: if no URL poster drew but the poster IS flashed locally, the
    // existing build-time asset image still satisfies the rung (slice-01: usually false).
    if (!poster_drew && AssetAvailable(poster_src)) poster_drew = true;
    const bool object_drew = AssetAvailable(object_src);   // teachingObject still flash-only

    const cJSON* card = Obj(to, "primitiveFallbackCard");
    const char* glyph = Str(card, "glyph");
    const char* label = Str(card, "label");
    if (label == nullptr) label = Str(to, "primaryWord");
    const char* emotion = ExpressionToEmotion(Str(ro, "expression"));
    const char* alt = Str(bg, "altCaption");

    // Caption line — AUTHORED lesson content only (COPPA-safe; never child speech,
    // never logged). When Layer-2 falls to the glyph card, fold glyph+label in.
    std::string caption;
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
    if (caption.size() > 96) caption.resize(96);  // truncate for the 480px line (STORYBOARD §46-48)

    // §7.5 degraded semantics: false only when poster+PNG+emoji+caption all drew;
    // true for a glyph-card fallback or a dropped media layer. The step is counted
    // regardless (rendered:true — emoji-face + caption always draw).
    const bool degraded = !(poster_drew && object_drew);

    // Marshal the draw onto the LVGL task, exactly like the TTS-display pattern
    // (application.cc SetChatMessage Schedule). Layer-3 emoji-face + the caption are
    // ALWAYS drawn (over the poster background when one fetched). If THIS step drew no
    // poster, clear any background a previous step left up so a caption-only step is not
    // shown over a stale picture.
    Display* display = base_display;
    if (display) {
        const bool clear_bg = !poster_drew;
        Schedule([display, lvgl_display, clear_bg,
                  emo = std::string(emotion), cap = caption]() {
            if (clear_bg && lvgl_display) lvgl_display->SetLessonBackground(nullptr);
            display->SetEmotion(emo.c_str());
            if (!cap.empty()) display->SetChatMessage("assistant", cap.c_str());
        });
    }

    // Canonical step-ack first (body.acks echoes the step's sequence). For a PASSIVE
    // narration step this ack IS the completion signal (the ESP auto-advances on it),
    // so it is the ONLY F->S frame for such a step.
    emit_ack(root, sequence, /*rendered*/ true, degraded);

    // FW-01 / FW-LESSON-01: emit lesson_progress step_completed ONLY for an INTERACTIVE
    // step (fixture multiStepThread step 8 for s4 'model'); a PASSIVE narration step
    // gets NO step_completed (fixture step 10 for s5 'review' — "the firmware never
    // emits step_completed for narration"). Emitting it for a passive step would set the
    // ESP completion latch on the WRONG step (latch-contamination, runtime.py:284-300)
    // and skip downstream steps. Classify from body.completionClass, falling back to
    // body.stepType membership in PASSIVE_STEP_TYPES — mirroring the ESP exactly.
    const char* step_type       = Str(body, "stepType");
    const char* completion_class = Str(body, "completionClass");
    const bool passive = IsPassiveStep(completion_class, step_type);
    const char* sid = Str(root, "stepId");
    if (!passive) {
        // Child-observable progress for the rendered interactive step. Wire field is
        // `result` (the ESP forwarder renames result->outcome before REST ingest —
        // §5.7). Interactive tap scoring is DEFERRED (plan §7.4.2), so no tapTargetHit
        // is fabricated; the step completes successfully by being demonstrated.
        cJSON* pb = cJSON_CreateObject();
        cJSON_AddStringToObject(pb, "event", "step_completed");
        cJSON_AddStringToObject(pb, "stepType", step_type != nullptr ? step_type : "model");
        cJSON_AddStringToObject(pb, "result", "success");
        cJSON_AddItemToObject(pb, "detail", cJSON_CreateObject());
        emit(root, "lesson_progress", pb);
    }
    ESP_LOGI(TAG, "lesson_step rendered stepId=%s passive=%d degraded=%d",
             sid != nullptr ? sid : "?", passive, degraded);
}
