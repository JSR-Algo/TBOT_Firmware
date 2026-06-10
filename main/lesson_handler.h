#ifndef LESSON_HANDLER_H
#define LESSON_HANDLER_H

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
// Asset bytes (D-PRELOAD-OWNER, ADR §C): the firmware NEVER downloads or checksums
// assets. The ESP Server is the single authoritative downloader + sha256 verifier
// and gates READY; the firmware renders only on-device verified bytes. In slice-01
// the espTft poster + teachingObject PNG are NOT flashed into the read-only asset
// image (main/assets/ carries only sounds + locales) and no ESP->firmware byte
// channel exists, so the image rungs of the degraded ladder fall through to the
// always-available rungs (primitive glyph card + emoji-face v1 + caption). Wiring
// the image rungs — flash the espTft variant (or expose an ESP path), add
// LcdDisplay::SetLessonBackground + a centered image draw, and enable the LVGL PNG
// decoder — is the hardware-gated leg (plan §7.4 / §10.5 / DIV-FW-DECODE), not
// CP-6 (which is the build-green + backward-compat + canonical-ack proof).

// Frozen contract identity (fixture _meta.protocolVersion). Single source of truth
// shared by the renderer and the hello.features advertisement.
constexpr char kLessonProtocolVersion[] = "teebot-lesson-renderer.v1";
constexpr char kLessonRendererName[]    = "teebot-lesson-renderer.v1";

// The only render profile this firmware (lcdwiki-es3c35p) implements (plan §7).
constexpr char kLessonProfileEspTft[]   = "espTft";

#endif  // LESSON_HANDLER_H
