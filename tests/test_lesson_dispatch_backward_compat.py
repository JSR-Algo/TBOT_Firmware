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
    assert "EnqueueLessonMessage(root, callback_transport_epoch);" in branch
    assert "HandleLessonMessage(root);" not in branch
    # ...sits BELOW the custom branch and ABOVE the unchanged unknown-type no-op,
    # so un-upgraded firmware keeps dropping lesson_* silently (backward compat).
    assert custom < lesson < noop

def test_lesson_frames_are_serialized_off_websocket_receive_stack():
    app = read("main/application.cc")
    header = read("main/application.h")
    constructor = app[app.index("Application::Application()") : app.index("Application::~Application()")]
    initialize_protocol = app[app.index("void Application::InitializeProtocol()") : app.index("protocol_->OnConnected")]

    assert "QueueHandle_t lesson_message_queue_" in header
    assert "static void LessonMessageTask(void* arg);" in header
    assert "void EnqueueLessonMessage(const cJSON* root, std::uint64_t transport_epoch);" in header
    assert "kLessonMessageWorkerStackDepth = 32768" in app
    assert 'xTaskCreateStatic(\n            &Application::LessonMessageTask, "lesson_worker"' in constructor
    assert "kLessonMessageWorkerStackDepth, this" in constructor
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in constructor
    assert "lesson_worker" not in initialize_protocol
    assert "xQueueSend(lesson_message_queue_, &item" in app
    assert "LessonQueueItemKind::kFrame" in app
    assert "transport_epoch" in app
    worker = app[app.index("void Application::LessonMessageTask") : app.index("bool Application::SetDeviceState")]
    assert "xQueueReceive" in worker
    assert "HandleLessonMessage(root);" in worker


def test_lesson_worker_logs_stack_watermark_around_each_frame():
    app = read("main/application.cc")
    worker = app[app.index("void Application::LessonMessageTask") : app.index("bool Application::SetDeviceState")]

    assert "kLessonMessageWorkerMinimumFreeStackBytes = 4096" in app
    assert 'LogLessonWorkerStackWatermark("before_parse")' in worker
    assert 'LogLessonWorkerStackWatermark("after_handle")' in worker
    assert "uxTaskGetStackHighWaterMark(nullptr)" in app
    assert 'ESP_LOGE(TAG, "lesson_worker stack low' in app


def test_lesson_worker_queue_full_drop_is_nonblocking_and_frees_payload():
    app = read("main/application.cc")
    enqueue = app[
        app.index("void Application::EnqueueLessonMessage") :
        app.index("void Application::LessonMessageTask")
    ]

    assert "char* payload = cJSON_PrintUnformatted(root);" in enqueue
    assert "xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE" in enqueue
    assert "uxQueueMessagesWaiting(lesson_message_queue_) >= kLessonMessageQueueDepth" in enqueue
    assert 'ESP_LOGW(TAG, "lesson_* dropped: worker queue full type=%s seq=%d"' in enqueue
    drop = enqueue[
        enqueue.index("xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE") :
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
    robot_tools_start = mcp.index("auto add_robot_arm_tool")
    robot_tools = mcp[
        robot_tools_start : mcp.index("auto backlight =", robot_tools_start)
    ]
    assert '"lesson_' not in robot_tools


def test_hello_features_advertises_lesson_capability_additively():
    ws = read("main/protocols/websocket_protocol.cc")
    # existing capability flags stay...
    assert 'cJSON_AddBoolToObject(features, "mcp", true);' in ws
    # ...lesson capability is advertised next to them (D-CAP-FLAG, ADR 0013 §I).
    assert 'cJSON_AddBoolToObject(features, "lesson", true);' in ws
    assert 'cJSON_AddStringToObject(features, "renderer", kLessonRendererName);' in ws


def test_renderer_v5_is_additive_and_preserves_v1_through_v4_contracts():
    handler = read("main/lesson_handler.cc")
    header = read("main/lesson_layered_cinematic_renderer.h")

    assert 'kLessonRendererV5[] = "teebot-lesson-renderer.v5"' in header
    assert 'kLessonLayeredCinematicTemplate[] = "layeredCinematic"' in header
    assert 'cJSON_CreateString(kLessonRendererV1)' in handler
    assert 'cJSON_CreateString(kLessonRendererV2)' in handler
    assert 'cJSON_CreateString(tbot::kLessonRendererV3)' in handler
    assert 'cJSON_CreateString(tbot::kLessonRendererV4)' in handler
    assert 'cJSON_CreateString(tbot::kLessonRendererV5)' in handler
    assert handler.index('cJSON_CreateString(tbot::kLessonRendererV3)') < handler.index(
        'cJSON_CreateString(tbot::kLessonRendererV4)'
    ) < handler.index('cJSON_CreateString(tbot::kLessonRendererV5)')
    assert '"lessonRendererV5"' in handler
    assert '"layeredCinematic"' in handler
    assert '"sdAssetPack"' in handler


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


def test_named_tvideo_template_is_additive_and_never_dispatches_raw_motion():
    handler = read("main/lesson_handler.cc")
    build = read("main/CMakeLists.txt")
    assert '#include "lesson_tvideo_template.h"' in handler
    assert 'cJSON_GetObjectItem(body, "templateProjection")' in handler
    assert 'lesson_tvideo::StateMachine' in handler
    assert '"lesson_tvideo_template.cc"' in build
    template_path = read("main/lesson_tvideo_template.cc")
    for forbidden in ("servoCommand", "motorCommand", "chassisCommand", "SendRobotAction", "RobotUart"):
        assert forbidden not in template_path


def test_tvideo_fallback_contributes_to_degraded_ack_without_blocking_content():
    handler = read("main/lesson_handler.cc")
    assert "tvideo_degraded" in handler
    assert "snapToArriveNearAndReveal" in handler
    assert "degraded = tvideo_degraded ||" in handler


def test_named_tvideo_overlay_uses_an_exact_outer_rectangle():
    display = read("main/display/lcd_display.cc")
    assert "lv_obj_set_size(lesson_robot_overlay_, max_width, max_height)" in display
    assert "lv_image_set_inner_align(lesson_robot_overlay_, LV_IMAGE_ALIGN_CONTAIN)" in display


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
    assert "const bool has_visible_child_prompt = has_caption_prompt && !caption.empty();" in render_tail
    assert "const bool should_listen = !passive && has_visible_content && has_visible_child_prompt;" in render_tail
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
    assert render_tail.index("const bool has_visible_child_prompt") < render_tail.index("const bool should_listen")
    assert render_tail.index("if (!should_listen)") < render_tail.index("if (should_listen)")
    assert render_tail.index("BeginLessonInteractiveListeningRequest") < render_tail.index("Schedule([listen_generation]")
    assert render_tail.index("if (should_listen)") < render_tail.index("Schedule([listen_generation]") < render_tail.index("PrepareLessonInteractiveListening")


# ── FW-02: prepare validates before either transactional commit path ────────────────
def test_prepare_validates_version_before_transactional_session_commit():
    h = read("main/lesson_handler.cc")
    normal_prepare_start = h.index('const bool is_prepare = strcmp(type, "lesson_prepare") == 0;')
    normal_prepare_end = h.index('if (strcmp(type, "lesson_start") == 0)', normal_prepare_start)
    normal_prepare = h[normal_prepare_start:normal_prepare_end]
    gate = normal_prepare.index("const bool version_ok =")
    version_rejection = normal_prepare.index(
        "if (is_prepare && (!version_ok || !profile_ok))", gate
    )
    isolated_error = normal_prepare.index("emit_isolated_prepare_error", gate)
    reset = normal_prepare.index("g_session = LessonSession{};", version_rejection)

    assert gate < isolated_error < reset
    assert gate < version_rejection < reset
    assert "emit_prepare_error" in normal_prepare[version_rejection:reset]


# ── FW-LESSON-02: duplicate re-ack replays the cached rendered/degraded ────────────
def test_duplicate_reack_replays_cached_rendered_degraded():
    h = read("main/lesson_handler.cc")
    assert "ack_history" in h and "last_ack_sequence" in h
    # Exact body replay includes rendered/degraded, elapsed time and telemetry.
    dedup = h.index("sequence <= g_session.last_in_sequence) {")
    replay = h.index("cJSON_Parse(it->body_json.c_str())", dedup)
    reack = h.index('emit(root, "lesson_ack", replay_body)', dedup)
    assert dedup < replay < reack


def test_duplicate_prepare_is_deduped_before_session_reset():
    h = read("main/lesson_handler.cc")
    normal_prepare_start = h.index('const bool is_prepare = strcmp(type, "lesson_prepare") == 0;')
    normal_prepare_end = h.index('if (strcmp(type, "lesson_start") == 0)', normal_prepare_start)
    normal_prepare = h[normal_prepare_start:normal_prepare_end]
    duplicate_prepare = normal_prepare.index("const bool duplicate_prepare =")
    dedup = normal_prepare.index(
        "if ((!is_prepare || duplicate_prepare) && sequence <= g_session.last_in_sequence)",
        duplicate_prepare,
    )
    reset = normal_prepare.index("g_session = LessonSession{};", dedup)

    assert "g_session.session_id == session_id" in normal_prepare[:duplicate_prepare]
    assert duplicate_prepare < dedup < reset


def test_duplicate_prepare_reack_replays_cached_asset_pack_metadata():
    h = read("main/lesson_handler.cc")
    assert "last_ack_asset_pack_json" in h
    assert "cJSON_PrintUnformatted(asset_pack_ack)" in h
    assert "g_session.last_ack_asset_pack_json" in h

    dedup = h.index("sequence <= g_session.last_in_sequence) {")
    replay_parse = h.index("cJSON_Parse(it->body_json.c_str())", dedup)
    reack = h.index('emit(root, "lesson_ack", replay_body)', dedup)
    assert dedup < replay_parse < reack


def test_non_prepare_frames_must_match_active_assignment_and_session_before_dedup():
    h = read("main/lesson_handler.cc")
    context = h[h.index("// --- session context") : h.index("// Staleness drop")]
    assert "!is_prepare" in context
    assert "g_session.assignment_id != assignment_id" in context
    assert "g_session.session_id != session_id" in context
    assert context.index("g_session.session_id != session_id") < h.index("sequence <= g_session.last_in_sequence)")


def test_non_prepare_session_guard_runs_before_version_profile_error_emit():
    h = read("main/lesson_handler.cc")
    session_context = h.index("// --- session context")
    version_gate = h.index("if (!is_prepare && (!version_ok || !profile_ok))")
    error_emit = h.index('emit(root, "lesson_error", eb);', version_gate)
    assert session_context < version_gate < error_emit


# ── FW-LESSON-03: emit() always consumes frame_body (no leak when protocol_ null) ──
def test_emit_builds_frame_before_guarding_send_so_body_is_always_consumed():
    h = read("main/lesson_handler.cc")
    emit_start = h.index("auto emit = [this]")
    build = h.index("BuildFrame(in, frame_type, seq, frame_body)", emit_start)
    guard = h.index("if (protocol_ && !frame.empty())", emit_start)
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
    assert "LessonLocalFileReady(" in helper
    assert "LessonAssetFileLimit(media_type)" in helper
    assert 'cJSON_AddBoolToObject(out, "ready", ready)' in helper
    assert 'cJSON_AddStringToObject(out, "cacheKey", cache_key_value.c_str())' in helper

def test_prepare_ack_validates_sd_asset_pack_file_size_when_declared():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert 'Num(asset, "size", size_value)' in helper
    assert "LessonLocalFileReady(" in helper
    assert "LessonAssetFileLimit(media_type)" in helper

    readiness_start = h.index("bool LessonLocalFileReady")
    readiness = h[readiness_start : h.index("cJSON* BuildAssetPackAck", readiness_start)]
    assert "expected_size" in readiness
    assert "static_cast<size_t>(file_size) == expected_size" in readiness


def test_prepare_ack_keeps_image_ram_cap_separate_from_cinematic_sd_file_cap():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]
    readiness_start = h.index("bool LessonLocalFileReady")
    readiness = h[readiness_start : helper_start]

    assert "kMaxLessonImageBytes = 512 * 1024" in h
    assert "kMaxLessonCinematicFileBytes" in h
    assert "kMaxLessonTrgbFileBytes = tbot::kLessonTrgbMaxFileBytes" in h
    assert "kMaxLessonAssetPackDeclaredBytes = tbot::kLessonTrgbMaxPackBytes" in h
    assert "PlausibleTrgbContainerBytes" in helper
    assert 'Str(asset, "mediaType")' in helper
    assert "LessonAssetFileLimit(media_type)" in helper
    assert "size_t max_file_bytes" in readiness
    assert "static_cast<size_t>(file_size) <= max_file_bytes" in readiness

def test_prepare_ack_requires_declared_positive_sd_asset_pack_file_size():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert "bool has_declared_size" in helper
    assert "size_value > 0.0" in helper
    assert "size_value <= static_cast<double>(SIZE_MAX)" in helper
    assert "static_cast<double>(static_cast<size_t>(size_value)) == size_value" in helper
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
    assert "NormalizeAsciiToken(state)" in helper
    assert 'normalized_state == "READY"' in helper
    assert 'cJSON_GetObjectItem(asset, "checksumOk")' in helper
    assert "cJSON_IsTrue(checksum_ok)" in helper
    assert "asset_verified" in helper
    assert "std::string asset_key_value" in helper
    assert "TrimAsciiWhitespace(asset_key_value)" in helper
    assert "asset_key_value.empty()" in helper
    assert "!asset_verified" in helper


def test_prepare_ack_rejects_duplicate_and_missing_critical_asset_pack_keys():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert 'cJSON_GetObjectItem(body, "criticalAssets")' in helper
    assert "critical_assets != nullptr && !cJSON_IsArray(critical_assets)" in helper
    assert "ready_asset_keys" in helper
    assert "!ready_asset_keys.insert(asset_key_value).second" in helper
    assert "critical_key" in helper
    assert "std::string critical_key_value" in helper
    assert "TrimAsciiWhitespace(critical_key_value)" in helper
    assert "ready_asset_keys.find(critical_key_value) == ready_asset_keys.end()" in helper

def test_prepare_ack_requires_asset_pack_cache_key_to_include_full_manifest_checksum():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("cJSON* BuildAssetPackAck")
    helper = h[helper_start : h.index("// US-006 image render", helper_start)]

    assert 'Obj(body, "manifestRef")' in helper
    assert 'Str(manifest_ref, "manifestChecksum")' in helper
    assert "std::string cache_key_value" in helper
    assert "TrimAsciiWhitespace(cache_key_value)" in helper
    assert "manifest_checksum_required" in helper
    assert "std::string manifest_checksum_value" in helper
    assert "TrimAsciiWhitespace(manifest_checksum_value)" in helper
    assert "!manifest_checksum_value.empty()" in helper
    assert "cache_key_has_manifest_checksum" in helper
    assert "strstr(cache_key_value.c_str(), manifest_checksum_value.c_str())" in helper
    assert "!manifest_checksum_required" in helper
    assert "!cache_key_has_manifest_checksum" in helper
    assert 'cJSON_AddStringToObject(out, "cacheKey", cache_key_value.c_str())' in helper


def test_prepare_reserves_lesson_asset_session_before_asset_pack_io():
    h = read("main/lesson_handler.cc")
    assert '#include "lesson_asset_storage_coordinator.h"' in h
    prepare_start = h.index("const bool is_prepare =")
    prepare_end = h.index('if (strcmp(type, "lesson_start") == 0)', prepare_start)
    prepare = h[prepare_start:prepare_end]
    assert prepare.index("TryBeginLessonSession(") < prepare.index("BuildAssetPackAck(body)")
    assert "reservation.generation" in prepare
    assert "reservation.idempotent" in prepare
    assert "lesson_asset_generation" in h


def test_prepare_handles_every_storage_reservation_refusal_before_io():
    h = read("main/lesson_handler.cc")
    prepare_start = h.index("const bool is_prepare =")
    prepare_end = h.index('if (strcmp(type, "lesson_start") == 0)', prepare_start)
    prepare = h[prepare_start:prepare_end]
    refusal = prepare[:prepare.index("BuildAssetPackAck(body)")]
    helper_start = h.index("ReservationRefusalMapping MapLessonReservationRefusal")
    helper_end = h.index("#ifdef TBOT_HOST_NATIVE_COVERAGE", helper_start)
    helper = h[helper_start:helper_end]
    assert "MapLessonReservationRefusal(reservation.code)" in refusal
    for code in (
        "LessonAssetReservationCode::kMutationActive",
        "LessonAssetReservationCode::kLessonSessionMismatch",
        "LessonAssetReservationCode::kInvalidIdentity",
        "LessonAssetReservationCode::kGenerationExhausted",
        '"LESSON_ASSET_MUTATION_ACTIVE"',
        '"LESSON_SESSION_CONFLICT"',
        '"LESSON_IDENTITY_INVALID"',
        '"LESSON_RESERVATION_EXHAUSTED"',
    ):
        assert code in helper


def test_terminal_lesson_paths_release_exact_storage_generation_without_force():
    h = read("main/lesson_handler.cc")
    assert "EndLessonSession(" in h
    assert "g_session.lesson_asset_generation" in h
    assert "ForceEndLessonSession" not in h

    stop_start = h.index('if (strcmp(type, "lesson_stop") == 0)')
    error_start = h.index('if (strcmp(type, "lesson_error") == 0)')
    stop = h[stop_start:error_start]
    error = h[error_start:h.index('if (strcmp(type, "lesson_step") != 0)', error_start)]
    no_visible_start = h.index("if (!has_visible_content) {")
    no_visible = h[no_visible_start:h.index("// FW-01 / FW-LESSON-01", no_visible_start)]
    prepare_start = h.index("if (is_prepare) {")
    prepare = h[prepare_start:h.index('if (strcmp(type, "lesson_start") == 0)', prepare_start)]

    assert "end_lesson_asset_session();" in stop
    assert "end_lesson_after_failure();" in error
    assert "end_lesson_asset_session();" in no_visible
    assert "end_lesson_asset_session();" in prepare
    assert "release_new_lesson_asset_session();" in prepare


def test_lesson_raw_transport_rejects_decoded_nul_before_enqueue_roundtrip():
    safety_h = read("main/json_payload_safety.h")
    safety_cc = read("main/json_payload_safety.cc")
    protocol_cc = read("main/protocols/protocol.cc")
    protocol_h = read("main/protocols/protocol.h")
    websocket = read("main/protocols/websocket_protocol.cc")
    mqtt = read("main/protocols/mqtt_protocol.cc")
    cmake = read("main/CMakeLists.txt")
    host_runner = read("scripts/run_host_native_lesson_handler_test.sh")
    coverage_runner = read("scripts/run_host_native_lesson_coverage.sh")

    assert "JsonHasForbiddenDecodedNull" in safety_h
    assert safety_cc.count("bool JsonHasForbiddenDecodedNull(") == 1
    assert "JsonHasForbiddenDecodedNull" not in protocol_h
    assert "JsonHasForbiddenDecodedNull" not in protocol_cc
    assert "TBOT_JSON_VALIDATOR_ONLY" not in protocol_cc
    assert "json_payload_safety.cc" in cmake
    assert "json_payload_safety.cc" in host_runner
    assert "json_payload_safety.cc" in coverage_runner
    assert "TBOT_JSON_VALIDATOR_ONLY" not in host_runner
    assert "TBOT_JSON_VALIDATOR_ONLY" not in coverage_runner
    assert '#include "json_payload_safety.h"' in websocket
    assert '#include "json_payload_safety.h"' in mqtt
    ws_parse = websocket.index("cJSON_ParseWithLength(data, len)")
    ws_guard = websocket.rfind("JsonHasForbiddenDecodedNull(data, len)", 0, ws_parse)
    assert ws_guard != -1 and ws_guard < ws_parse
    mqtt_parse = mqtt.index("cJSON_Parse(payload.c_str())")
    mqtt_guard = mqtt.rfind(
        "JsonHasForbiddenDecodedNull(payload.data(), payload.size())", 0, mqtt_parse
    )
    assert mqtt_guard != -1 and mqtt_guard < mqtt_parse


def test_prepare_pure_validation_precedes_reservation_and_candidate_io():
    h = read("main/lesson_handler.cc")
    prepare_start = h.index("const bool is_prepare =")
    prepare_end = h.index('if (strcmp(type, "lesson_start") == 0)', prepare_start)
    prepare = h[prepare_start:prepare_end]
    version = prepare.index("const bool version_ok =")
    identity = prepare.index("valid_prepare_identity")
    body_shape = prepare.index("valid_prepare_body")
    sequence_guard = prepare.index("if (is_prepare && sequence == 0)")
    reserve = prepare.index("TryBeginLessonSession(")
    asset_ack = prepare.index("BuildAssetPackAck(body)")
    commit = prepare.index("g_session = LessonSession{};", asset_ack)
    assert version < reserve
    assert identity < reserve
    assert body_shape < reserve
    assert sequence_guard < reserve
    assert reserve < asset_ack < commit


def test_unowned_prepare_refusal_uses_isolated_sequence_without_owner_mutation():
    h = read("main/lesson_handler.cc")
    helper_start = h.index("auto emit_isolated_prepare_error")
    helper_end = h.index("const bool is_prepare =", helper_start)
    helper = h[helper_start:helper_end]
    assert "BuildFrame(in, \"lesson_error\", 1, frame_body)" in helper
    assert "g_session.fs_sequence" not in helper
    assert "++g_session.fs_sequence" not in helper


def test_prepare_candidate_stream_routing_distinguishes_owner_from_republish():
    h = read("main/lesson_handler.cc")
    assert "const bool prepare_isolated_stream" in h
    routing_start = h.index("const bool prepare_isolated_stream")
    routing = h[routing_start:h.index("auto emit_prepare_error", routing_start)]
    assert "prepare_has_newer_assignment_version" in routing
    assert "restart_prepare" in routing
    assert "g_session.lesson_asset_generation == 0" in routing

    seq_zero = h[h.index("if (is_prepare && sequence == 0)"):
                 h.index("bool prepare_newly_acquired_asset_session")]
    assert "emit_prepare_error(" in seq_zero

    readiness = h[h.index("if (!candidate_asset_pack_ready)"):
                  h.index("const cJSON* runtime_controls")]
    assert "prepare_isolated_stream" in readiness
    assert "emit_isolated_prepare_ack" in readiness
    assert "emit_ack(" in readiness
