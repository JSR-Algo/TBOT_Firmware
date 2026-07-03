"""US-006 Slice-01 / S10 / CP-6 — backward-compat + canonical-ack static proof.

Source-parsing contract tests (no hardware) mirroring docs plan §10.3. They pin
the additive-guarantee: the lesson_* renderer is layered ON TOP of the existing
dispatch without disturbing the 8 legacy message types, the Google-Live voice
path, or the MCP arm tools, and the lesson_ack shape is canonical (body.acks, no
ackFor). If a future edit to the moving-target application.cc breaks any of these,
CP-6 regresses and these tests fail.
"""
import re
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
    branch = app[lesson:noop]
    # The lesson branch dispatches through the serialized worker instead of doing
    # blocking HTTP/TLS asset fetches on the WebSocket receive callback stack.
    assert "EnqueueLessonMessage(root);" in branch
    assert "HandleLessonMessage(root);" not in branch
    # ...sits BELOW the custom branch and ABOVE the unchanged unknown-type no-op,
    # so un-upgraded firmware keeps dropping lesson_* silently (backward compat).
    assert custom < lesson < noop

def test_lesson_frames_are_serialized_off_websocket_receive_stack():
    app = read("main/application.cc")
    header = read("main/application.h")
    initialize = app[app.index("void Application::Initialize()") : app.index("void Application::Run()")]
    initialize_protocol = app[app.index("void Application::InitializeProtocol()") : app.index("protocol_->OnConnected")]

    assert "QueueHandle_t lesson_message_queue_" in header
    assert "static void LessonMessageTask(void* arg);" in header
    assert "void EnqueueLessonMessage(const cJSON* root);" in header
    assert "kLessonMessageWorkerStackBytes = 12288" in app
    assert 'xTaskCreateWithCaps(&Application::LessonMessageTask, "lesson_worker"' in initialize_protocol
    assert "kLessonMessageWorkerStackBytes, this" in initialize_protocol
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in initialize_protocol
    assert "lesson_worker" not in initialize
    assert "xQueueSend(lesson_message_queue_, &payload" in app
    worker = app[app.index("void Application::LessonMessageTask") : app.index("bool Application::SetDeviceState")]
    assert "xQueueReceive" in worker
    assert "HandleLessonMessage(root);" in worker


def test_lesson_worker_queue_full_drop_is_nonblocking_and_frees_payload():
    app = read("main/application.cc")
    enqueue = app[
        app.index("void Application::EnqueueLessonMessage") :
        app.index("void Application::LessonMessageTask")
    ]

    assert "char* payload = cJSON_PrintUnformatted(root);" in enqueue
    assert "xQueueSend(lesson_message_queue_, &payload, 0) != pdTRUE" in enqueue
    assert 'ESP_LOGW(TAG, "lesson_* dropped: worker queue full type=%s seq=%d"' in enqueue
    drop = enqueue[
        enqueue.index("xQueueSend(lesson_message_queue_, &payload, 0) != pdTRUE") :
        enqueue.index("} else {")
    ]
    assert "cJSON_free(payload);" in drop
    assert "HandleLessonMessage(root);" not in enqueue


def test_unknown_type_noop_is_unchanged():
    app = read("main/application.cc")
    assert app.count('ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring)') == 1


def test_handle_robot_action_message_untouched_and_mcp_arm_tools_have_no_lesson_leak():
    # The lesson path must reuse robot_action for arm motion, never fork it, and the
    # MCP arm tools (mcp_server.cc) must carry zero lesson control coupling. A source
    # comment may reference the shared image-fetch hardening, but MCP must not dispatch
    # lesson frames or call the lesson handler.
    assert "bool Application::HandleRobotActionMessage(const cJSON* root)" in read("main/application.cc")
    mcp = read("main/mcp_server.cc")
    assert "HandleLessonMessage" not in mcp
    assert "SendLessonFrame" not in mcp
    assert '"lesson_' not in mcp


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


def test_firmware_does_not_checksum_or_verify_assets_but_does_fetch():
    # D-PRELOAD-OWNER (CR-FW-09 reconciled 2026-06-20): the ESP Server owns sha256
    # verification + READY gating; the firmware does NOT checksum/verify/OTA. It DOES,
    # at render time, FETCH the already-verified bytes (FetchLessonImage HTTP GET).
    # This guard pins the TRUE contract (no checksum/verify) AND the real download path
    # — replacing the old vacuous grep for `esp_http_client`, whose absence wrongly
    # implied "no download" while FetchLessonImage downloads via the Board abstraction.
    h = read("main/lesson_handler.cc")
    # firmware never checksums / verifies / OTA-updates assets (ESP server owns that)...
    for forbidden in ("esp_crypto_sha256", "mbedtls_sha256", "esp_https_ota"):
        assert forbidden not in h, f"{forbidden}: firmware must not checksum/verify/OTA assets (ESP owns it)"
    # ...but it DOES download the verified bytes at render time via the board HTTP stack.
    assert "FetchLessonImage" in h
    assert "Board::GetInstance().GetNetwork()" in h
    assert "->CreateHttp(" in h
    assert '->Open("GET"' in h


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
    render_tail = h[h.index("emit_ack(root, sequence") : h.index('ESP_LOGI(TAG, "lesson_step rendered')]
    assert "const bool passive = IsPassiveStep" in render_tail
    assert 'emit(root, "lesson_progress", pb)' not in render_tail


def test_no_unconditional_step_completed_emit():
    # The lesson renderer must not report step_completed from draw success. Child
    # response completion is owned by transcript/tap response paths, not render ack.
    h = read("main/lesson_handler.cc")
    assert h.count('emit(root, "lesson_progress"') == 0

def test_interactive_render_does_not_fabricate_child_response_progress():
    h = read("main/lesson_handler.cc")
    render_tail = h[h.index("emit_ack(root, sequence") : h.index('ESP_LOGI(TAG, "lesson_step rendered')]

    assert 'cJSON_AddStringToObject(pb, "result", "success")' not in render_tail
    assert 'cJSON_AddItemToObject(pb, "detail", cJSON_CreateObject())' not in render_tail
    assert 'emit(root, "lesson_progress", pb)' not in render_tail

def test_interactive_step_opens_listening_instead_of_completing_from_render():
    h = read("main/lesson_handler.cc")
    render_tail = h[h.index("emit_ack(root, sequence") : h.index('ESP_LOGI(TAG, "lesson_step rendered')]

    assert "const bool has_visible_content = rendered &&" in render_tail
    assert "!caption.empty() || poster_drew || object_drew || overlay_drew" in render_tail
    assert "const bool should_listen = !passive && has_visible_content;" in render_tail
    assert "if (!should_listen)" in render_tail
    assert "Application::GetInstance().CancelLessonInteractiveListening();" in render_tail
    assert "if (should_listen)" in render_tail
    assert "const uint32_t listen_generation =" in render_tail
    assert "BeginLessonInteractiveListeningRequest();" in render_tail
    listen_schedule = re.search(
        r"Schedule\(\[listen_generation\]\(\)\s*\{\s*Application::GetInstance\(\)\.PrepareLessonInteractiveListening\(listen_generation\);\s*\}\);",
        render_tail,
        re.S,
    )
    assert listen_schedule is not None
    assert render_tail.index("if (!should_listen)") < render_tail.index("if (should_listen)")
    assert render_tail.index("BeginLessonInteractiveListeningRequest") < render_tail.index("Schedule([listen_generation]")
    assert render_tail.index("if (should_listen)") < render_tail.index("Schedule([listen_generation]") < render_tail.index("PrepareLessonInteractiveListening")


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


def test_duplicate_prepare_is_deduped_before_session_reset():
    h = read("main/lesson_handler.cc")
    duplicate_prepare = h.index("const bool duplicate_prepare =")
    reset = h.index("g_session = LessonSession{};")
    assert duplicate_prepare < reset
    assert "if (is_prepare && !duplicate_prepare)" in h
    assert "g_session.session_id == session_id" in h


def test_duplicate_prepare_reack_replays_cached_asset_pack_metadata():
    h = read("main/lesson_handler.cc")
    assert "last_ack_asset_pack_json" in h
    assert "cJSON_PrintUnformatted(asset_pack_ack)" in h
    assert "g_session.last_ack_asset_pack_json" in h

    dedup = h.index("if (sequence <= g_session.last_in_sequence) {")
    replay_parse = h.index("cJSON_Parse(g_session.last_ack_asset_pack_json.c_str())", dedup)
    reack = h.index("emit_ack(root, sequence, re_rendered, re_degraded, re_asset_pack", dedup)
    assert dedup < replay_parse < reack


def test_non_prepare_frames_must_match_active_assignment_and_session_before_dedup():
    h = read("main/lesson_handler.cc")
    context = h[h.index("// --- session context") : h.index("// Staleness drop")]
    assert "!is_prepare" in context
    assert "g_session.assignment_id != assignment_id" in context
    assert "g_session.session_id != session_id" in context
    assert context.index("g_session.session_id != session_id") < h.index("if (sequence <= g_session.last_in_sequence)")


def test_non_prepare_session_guard_runs_before_version_profile_error_emit():
    h = read("main/lesson_handler.cc")
    session_context = h.index("// --- session context")
    version_gate = h.index("const bool version_ok =")
    error_emit = h.index('emit(root, "lesson_error", eb);', version_gate)
    assert session_context < version_gate < error_emit


# ── FW-LESSON-03: emit() always consumes frame_body (no leak when protocol_ null) ──
def test_emit_builds_frame_before_guarding_send_so_body_is_always_consumed():
    h = read("main/lesson_handler.cc")
    emit_start = h.index("auto emit = [this]")
    build = h.index("BuildFrame(in, frame_type, seq, frame_body)", emit_start)
    guard = h.index("if (protocol_) protocol_->SendLessonFrame(frame)", emit_start)
    # BuildFrame (the sole consumer of frame_body) is called unconditionally, BEFORE
    # the protocol_ guard, so frame_body is freed even when protocol_ is null.
    assert build < guard

def test_prepare_ack_reports_sd_asset_pack_readiness_from_local_files():
    h = read("main/lesson_handler.cc")
    prepare_branch = h[h.index("if (is_prepare) {") : h.index('if (strcmp(type, "lesson_start") == 0)')]

    assert "BuildAssetPackAck(body)" in prepare_branch
    assert "emit_ack(root, sequence, /*rendered*/ false, /*degraded*/ false, asset_pack_ack)" in prepare_branch

    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]
    assert 'Obj(body, "assetPack")' in helper
    assert 'Str(pack, "cacheKey")' in helper
    assert 'cJSON_GetObjectItem(pack, "assets")' in helper
    assert 'Str(asset, "localPath")' in helper
    assert "LessonLocalFileReady(local_path, expected_size)" in helper
    assert 'cJSON_AddBoolToObject(out, "ready", ready)' in helper
    assert 'cJSON_AddStringToObject(out, "cacheKey", cache_key)' in helper

def test_prepare_ack_validates_sd_asset_pack_file_size_when_declared():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert 'Num(asset, "size", size_value)' in helper
    assert "LessonLocalFileReady(local_path, expected_size)" in helper

    readiness_start = h.index("bool LessonLocalFileReady")
    readiness = h[readiness_start : h.index("cJSON* BuildAssetPackAck", readiness_start)]
    assert "expected_size" in readiness
    assert "static_cast<size_t>(file_size) == expected_size" in readiness

def test_prepare_ack_requires_declared_positive_sd_asset_pack_file_size():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert "bool has_declared_size" in helper
    assert "has_declared_size && size_value > 0.0" in helper
    assert "!has_declared_size" in helper

def test_prepare_ack_rejects_empty_sd_asset_pack():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert "cJSON_GetArraySize(assets) > 0" in helper

def test_prepare_ack_requires_esp_verified_ready_asset_pack_metadata():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert 'Str(asset, "key")' in helper
    assert "asset_key" in helper
    assert 'Str(asset, "state")' in helper
    assert 'strcmp(state, "READY") == 0' in helper
    assert 'cJSON_GetObjectItem(asset, "checksumOk")' in helper
    assert "cJSON_IsTrue(checksum_ok)" in helper
    assert "asset_verified" in helper
    assert "asset_key == nullptr" in helper
    assert "asset_key[0] == '\\0'" in helper
    assert "!asset_verified" in helper


def test_prepare_ack_rejects_duplicate_and_missing_critical_asset_pack_keys():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert 'cJSON_GetObjectItem(body, "criticalAssets")' in helper
    assert "ready_asset_keys" in helper
    assert "!ready_asset_keys.insert(asset_key).second" in helper
    assert "critical_key" in helper
    assert "ready_asset_keys.find(critical_key) == ready_asset_keys.end()" in helper

def test_prepare_ack_requires_asset_pack_cache_key_to_include_full_manifest_checksum():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert 'Obj(body, "manifestRef")' in helper
    assert 'Str(manifest_ref, "manifestChecksum")' in helper
    assert "manifest_checksum_required" in helper
    assert "!Blank(manifest_checksum)" in helper
    assert "cache_key_has_manifest_checksum" in helper
    assert "strstr(cache_key, manifest_checksum)" in helper
    assert "!manifest_checksum_required" in helper
    assert "!cache_key_has_manifest_checksum" in helper
