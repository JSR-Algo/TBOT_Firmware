"""US-006 Slice-01 / S10 / CP-6 — backward-compat + canonical-ack static proof.

Source-parsing contract tests (no hardware) mirroring docs plan §10.3. They pin
the additive-guarantee: the lesson_* renderer is layered ON TOP of the existing
dispatch without disturbing the 8 legacy message types, the Google-Live voice
path, or the MCP arm tools, and the lesson_ack shape is canonical (body.acks, no
ackFor). If a future edit to the moving-target application.cc breaks any of these,
CP-6 regresses and these tests fail.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EIGHT_LEGACY_TYPES = ["tts", "stt", "llm", "mcp", "system", "alert", "robot_action", "custom"]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_eight_legacy_dispatch_branches_present_and_ordered():
    app = read("main/application.cc")
    idx = [app.index(f'strcmp(type->valuestring, "{t}") == 0') for t in EIGHT_LEGACY_TYPES]
    assert idx == sorted(idx), "the 8 legacy type branches must stay in their original order"


def test_null_guard_added_immediately_after_type_fetch_and_before_first_branch():
    app = read("main/application.cc")
    fetch = app.index('cJSON_GetObjectItem(root, "type")')
    guard = app.index("if (!cJSON_IsString(type)) {", fetch)
    first_branch = app.index('strcmp(type->valuestring, "tts") == 0', fetch)
    # guard sits AFTER the fetch and BEFORE the first deref of type->valuestring.
    assert fetch < guard < first_branch
    assert "Missing or non-string message type, dropping frame" in app


def test_lesson_branch_is_additive_above_the_unknown_type_noop():
    app = read("main/application.cc")
    custom = app.index('strcmp(type->valuestring, "custom") == 0')
    lesson = app.index('strncmp(type->valuestring, "lesson_", 7) == 0')
    noop = app.index('ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring)')
    # The lesson branch dispatches to the new handler...
    assert "HandleLessonMessage(root);" in app
    # ...sits BELOW the custom branch and ABOVE the unchanged unknown-type no-op,
    # so un-upgraded firmware keeps dropping lesson_* silently (backward compat).
    assert custom < lesson < noop


def test_unknown_type_noop_is_unchanged():
    app = read("main/application.cc")
    assert app.count('ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring)') == 1


def test_handle_robot_action_message_untouched_and_mcp_arm_tools_have_no_lesson_leak():
    # The lesson path must reuse robot_action for arm motion, never fork it, and the
    # MCP arm tools (mcp_server.cc) must carry zero lesson coupling.
    assert "bool Application::HandleRobotActionMessage(const cJSON* root)" in read("main/application.cc")
    assert "lesson" not in read("main/mcp_server.cc").lower()


def test_hello_features_advertises_lesson_capability_additively():
    ws = read("main/protocols/websocket_protocol.cc")
    # existing capability flags stay...
    assert 'cJSON_AddBoolToObject(features, "mcp", true);' in ws
    # ...lesson capability is advertised next to them (D-CAP-FLAG, ADR 0013 §I).
    assert 'cJSON_AddBoolToObject(features, "lesson", true);' in ws
    assert 'cJSON_AddStringToObject(features, "renderer", kLessonRendererName);' in ws


def test_additive_lesson_sender_does_not_alter_protected_send_text():
    proto_h = read("main/protocols/protocol.h")
    proto_cc = read("main/protocols/protocol.cc")
    assert "bool SendLessonFrame(const std::string& frame);" in proto_h
    assert "bool Protocol::SendLessonFrame(const std::string& frame)" in proto_cc
    # SendText stays the protected primitive; the lesson sender just delegates to it.
    assert "virtual bool SendText(const std::string& text) = 0;" in proto_h


def test_lesson_ack_shape_is_canonical_body_acks_no_ackfor():
    h = read("main/lesson_handler.cc")
    # body.acks echoes the acked sequence; there is NO ackFor JSON key anywhere (P0).
    # (Match the quoted key form so the comment that names the absent field is allowed.)
    assert 'cJSON_AddNumberToObject(b, "acks"' in h
    assert '"ackFor"' not in h
    # rendered/degraded complete the canonical ack body.
    assert '"rendered"' in h and '"degraded"' in h


def test_lessonversion_is_echoed_as_a_number_not_a_string():
    h = read("main/lesson_handler.cc")
    # D-LV: lessonVersion rides every echoed frame as a JSON number.
    assert "CopyNum(dst, src, k)" not in h or "cJSON_AddNumberToObject" in h
    assert 'CopyNum(root, in, "lessonVersion")' in h


def test_render_is_gated_on_exact_contract_identity_and_esptft_profile():
    h = read("main/lesson_handler.cc")
    header = read("main/lesson_handler.h")
    assert 'kLessonProtocolVersion[] = "teebot-lesson-renderer.v1"' in header
    assert 'kLessonProfileEspTft[]   = "espTft"' in header
    # exact-string negotiation (never a semver) + profile gate -> LESSON_VERSION_UNSUPPORTED.
    assert "strcmp(protocol_version, kLessonProtocolVersion) == 0" in h
    assert "strcmp(profile, kLessonProfileEspTft) == 0" in h
    assert "LESSON_VERSION_UNSUPPORTED" in h


def test_firmware_does_not_download_or_checksum_assets():
    # D-PRELOAD-OWNER: the firmware renders only on-device verified bytes; the ESP
    # Server owns download + sha256. No HTTPS fetch / hashing in the lesson handler.
    h = read("main/lesson_handler.cc")
    for forbidden in ("esp_http_client", "esp_crypto_sha256", "mbedtls_sha256", "esp_https_ota"):
        assert forbidden not in h


def test_lesson_handler_registered_in_build():
    assert '"lesson_handler.cc"' in read("main/CMakeLists.txt")


# ── FW-01 / FW-LESSON-01: passive steps get NO lesson_progress step_completed ──────
def test_lesson_progress_is_gated_on_interactive_completion_class():
    h = read("main/lesson_handler.cc")
    # The passive classifier mirrors the ESP PASSIVE_STEP_TYPES (runtime.py:90-92).
    for t in ("greeting", "review", "focus", "feedback", "celebrate"):
        assert f'"{t}"' in h, f"passive type {t} must be in the firmware classifier"
    assert "bool IsPassiveStep(" in h
    assert "completionClass" in h, "must read body.completionClass (authoritative)"
    # The step_completed lesson_progress build/emit is guarded so a PASSIVE step does
    # NOT emit it (fixture multiStepThread: progress for s4 'model', none for s5
    # 'review'). Assert the emit sits under the !passive guard, before the build.
    guard = h.index("if (!passive) {")
    build = h.index('cJSON_AddStringToObject(pb, "event", "step_completed")')
    emit = h.index('emit(root, "lesson_progress", pb)')
    assert guard < build < emit, "step_completed must be built+emitted only when !passive"


def test_no_unconditional_step_completed_emit():
    # There is exactly ONE lesson_progress emit and it is inside the !passive branch,
    # so step_completed is never emitted unconditionally for every rendered step.
    h = read("main/lesson_handler.cc")
    assert h.count('emit(root, "lesson_progress"') == 1


# ── FW-02: a session-opening prepare resets the F->S counter BEFORE the gate ───────
def test_prepare_resets_fs_counter_before_version_profile_gate():
    h = read("main/lesson_handler.cc")
    reset = h.index("g_session = LessonSession{};")
    gate = h.index("const bool version_ok =")
    # the session (incl. fs_sequence) is reset for a prepare BEFORE the version/profile
    # gate, so a rejected fresh prepare emits lesson_error at sequence=1 (restart-at-1).
    assert reset < gate, "prepare reset must precede the version/profile gate (FW-02)"
    # and there is no second duplicate reset after the gate.
    assert h.count("g_session = LessonSession{};") == 1


# ── FW-LESSON-02: duplicate re-ack replays the cached rendered/degraded ────────────
def test_duplicate_reack_replays_cached_rendered_degraded():
    h = read("main/lesson_handler.cc")
    assert "last_ack_rendered" in h and "last_ack_degraded" in h
    assert "last_ack_sequence" in h
    # the dedup re-ack path replays the cached flags rather than hardcoding false/false.
    dedup = h.index("if (sequence <= g_session.last_in_sequence) {")
    replay = h.index("g_session.last_ack_rendered", dedup)
    reack = h.index("emit_ack(root, sequence, re_rendered, re_degraded", dedup)
    assert dedup < replay < reack


# ── FW-LESSON-03: emit() always consumes frame_body (no leak when protocol_ null) ──
def test_emit_builds_frame_before_guarding_send_so_body_is_always_consumed():
    h = read("main/lesson_handler.cc")
    emit_start = h.index("auto emit = [this]")
    build = h.index("BuildFrame(in, frame_type, seq, frame_body)", emit_start)
    guard = h.index("if (protocol_) protocol_->SendLessonFrame(frame)", emit_start)
    # BuildFrame (the sole consumer of frame_body) is called unconditionally, BEFORE
    # the protocol_ guard, so frame_body is freed even when protocol_ is null.
    assert build < guard
