from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_runtime_logs_use_target_supported_integer_formats():
    app_cc = read("main/application.cc")
    lesson_cc = read("main/lesson_handler.cc")

    unsupported_logs = []
    unsupported_sd_helpers = []
    for source_path in (ROOT / "main").rglob("*"):
        if source_path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
            continue
        contents = source_path.read_text(encoding="utf-8")
        if "%lld" in contents or "%llu" in contents:
            unsupported_logs.append(str(source_path.relative_to(ROOT)))
        if "sdmmc_card_print_info(" in contents:
            unsupported_sd_helpers.append(str(source_path.relative_to(ROOT)))

    assert unsupported_logs == []
    assert unsupported_sd_helpers == []
    assert "tts_stop_received ts=%lu%03lu" in app_cc
    assert "lesson_* dropped: sequence must be an integer between 0 and INT32_MAX" in lesson_cc
    assert "sequence_d > static_cast<double>(INT32_MAX)" in lesson_cc
    assert "token=%lu%09lu%09lu" in read("main/boards/common/blufi.cpp")
    sd_board_sources = (
        read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")
        + read("main/boards/wireless-tag-wtp4c5mp07s/wireless-tag-wtp4c5mp07s.cc")
    )
    assert sd_board_sources.count("size_mb=%lu sector_size=%lu") == 3
    assert sd_board_sources.count("static_cast<uint64_t>(card->csd.capacity)") == 3
    assert "lesson_step_started assignmentId=%s" in lesson_cc
    assert "sequence=%ld" in lesson_cc


def test_listening_transition_uses_bounded_playback_drain():
    app_cc = read("main/application.cc")
    audio_h = read("main/audio/audio_service.h")
    audio_cc = read("main/audio/audio_service.cc")

    assert "kListenPlaybackDrainTimeoutMs" in app_cc
    assert "WaitForPlaybackQueueEmpty(kListenPlaybackDrainTimeoutMs)" in app_cc
    assert "audio_service_.WaitForPlaybackQueueEmpty();" not in app_cc
    assert "playback_queue_drain_timeout" in app_cc
    assert "bool WaitForPlaybackQueueEmpty(uint32_t timeout_ms" in audio_h
    assert "audio_queue_cv_.wait_for(" in audio_cc
    assert "std::chrono::milliseconds(timeout_ms)" in audio_cc


def test_audio_metrics_include_input_and_wake_state_for_real_device_wake_debug():
    app_cc = read("main/application.cc")

    assert "audio_metrics decode_q=%lu send_q=%lu playback_q=%lu input_count=%lu wake_running=%d" in app_cc
    assert "(unsigned long)audio_stats.input_count" in app_cc
    assert "audio_service_.IsWakeWordRunning() ? 1 : 0" in app_cc

def test_afe_wake_word_uses_more_sensitive_hiesp_threshold_on_lcdwiki():
    wake_cc = read("main/audio/wake_words/afe_wake_word.cc")

    assert "kHiEspWakeThreshold" in wake_cc
    assert "0.55f" in wake_cc
    assert "afe_iface_->set_wakenet_threshold(afe_data_, 1, kHiEspWakeThreshold)" in wake_cc
    assert "hiesp_wakenet_threshold" in wake_cc

def test_abort_speaking_clears_playback_before_relistening():
    app_cc = read("main/application.cc")
    start = app_cc.index("void Application::AbortSpeaking")
    end = app_cc.index("void Application::SetListeningMode", start)
    body = app_cc[start:end]

    assert "audio_service_.ResetDecoder();" in body
    assert body.index("audio_service_.ResetDecoder();") < body.index(
        "protocol_->SendAbortSpeaking(reason);"
    )

def test_tts_start_arms_speaking_timeout_watchdog():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    assert "kSpeakingTimeoutMs" in app_cc
    assert "speaking_generation_" in app_h
    assert "++speaking_generation_" in app_cc
    assert "current_generation = speaking_generation_" in app_cc
    assert "esp_timer_handle_t speaking_timeout_timer_" in app_h
    assert "esp_timer_start_once(speaking_timeout_timer_, kSpeakingTimeoutMs * 1000ULL)" in app_cc
    assert "SpeakingTimeoutTask" not in app_cc
    assert "void Application::HandleSpeakingTimeout" in app_cc

    start = app_cc.index('strcmp(state->valuestring, "start") == 0')
    end = app_cc.index('} else if (strcmp(state->valuestring, "stop") == 0)', start)
    tts_start_body = app_cc[start:end]
    assert "ArmSpeakingTimeout();" in tts_start_body


def test_tts_audio_is_accepted_before_scheduled_speaking_state_runs():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    assert "tts_audio_accepting_" in app_h

    incoming_audio_start = app_cc.index("protocol_->OnIncomingAudio")
    incoming_audio_end = app_cc.index("protocol_->OnAudioChannelOpened", incoming_audio_start)
    incoming_audio_body = app_cc[incoming_audio_start:incoming_audio_end]
    assert "tts_audio_accepting_.load()" in incoming_audio_body
    assert "PushPacketToDecodeQueue" in incoming_audio_body

    start = app_cc.index('strcmp(state->valuestring, "start") == 0')
    start_schedule = app_cc.index("Schedule([this]()", start)
    start_body_before_schedule = app_cc[start:start_schedule]
    assert "tts_audio_accepting_.store(true);" in start_body_before_schedule

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    stop_schedule = app_cc.index("Schedule([this]()", stop)
    stop_body_before_schedule = app_cc[stop:stop_schedule]
    assert "tts_audio_accepting_.store(false);" in stop_body_before_schedule

    abort = app_cc.index("void Application::AbortSpeaking")
    abort_end = app_cc.index("void Application::SetListeningMode", abort)
    assert "tts_audio_accepting_.store(false);" in app_cc[abort:abort_end]


def test_speaking_state_does_not_clear_tts_audio_accepted_before_state_transition():
    app_cc = read("main/application.cc")

    start = app_cc.index('strcmp(state->valuestring, "start") == 0')
    start_schedule = app_cc.index("Schedule([this]()", start)
    start_body_before_schedule = app_cc[start:start_schedule]
    assert "audio_service_.ResetDecoder();" in start_body_before_schedule
    assert start_body_before_schedule.index("audio_service_.ResetDecoder();") < start_body_before_schedule.index(
        "tts_audio_accepting_.store(true);"
    )

    speaking = app_cc.index("case kDeviceStateSpeaking:")
    wifi_config = app_cc.index("case kDeviceStateWifiConfiguring:", speaking)
    speaking_body = app_cc[speaking:wifi_config]
    assert "audio_service_.ResetDecoder();" not in speaking_body

def test_tts_start_stops_listening_audio_before_accepting_downlink_audio():
    app_cc = read("main/application.cc")

    start = app_cc.index('strcmp(state->valuestring, "start") == 0')
    start_schedule = app_cc.index("Schedule([this]()", start)
    start_body_before_schedule = app_cc[start:start_schedule]

    assert "audio_service_.EnableVoiceProcessing(false);" in start_body_before_schedule
    assert "listening_started_ms_.store(0);" in start_body_before_schedule
    assert start_body_before_schedule.index("audio_service_.EnableVoiceProcessing(false);") < start_body_before_schedule.index(
        "tts_audio_accepting_.store(true);"
    )

def test_tts_downlink_audio_pipeline_has_packet_decode_playback_diagnostics():
    audio_h = read("main/audio/audio_service.h")
    audio_cc = read("main/audio/audio_service.cc")
    es8311_cc = read("main/audio/codecs/es8311_audio_codec.cc")
    es8311_h = read("main/audio/codecs/es8311_audio_codec.h")

    assert "incoming_decode_packet_count" in audio_h
    assert "decode_fail_count" in audio_h
    assert "#define MAX_DECODE_PACKETS_IN_QUEUE (7200 / OPUS_FRAME_DURATION_MS)" in audio_h
    assert "tts_packet_queued" in audio_cc
    assert "tts_packet_dropped reason=decode_queue_full" in audio_cc
    assert "tts_packet_decoded" in audio_cc
    assert "tts_packet_playback" in audio_cc
    assert audio_cc.count("debug_statistics_.decode_count++") == 1
    assert "write_count_" in es8311_h
    assert "es8311_write" in es8311_cc
    assert ".channel = static_cast<uint8_t>(std::max(input_channels_, output_channels_))" in es8311_cc
    assert "if (output_channels_ == 2)" in es8311_cc
    assert "stereo_data[i * 2] = data[i];" in es8311_cc
    assert "stereo_data[i * 2 + 1] = data[i];" in es8311_cc
    assert "write_samples=%d" in es8311_cc
    assert "channels=%d" in es8311_cc
    assert "es8311_state" in es8311_cc
    assert "gpio_get_level(pa_pin_)" in es8311_cc


def test_websocket_protocol_preconnects_before_first_wake_word():
    ws_cc = read("main/protocols/websocket_protocol.cc")

    start = ws_cc.index("bool WebsocketProtocol::Start()")
    send_audio = ws_cc.index("bool WebsocketProtocol::SendAudio", start)
    start_body = ws_cc[start:send_audio]
    assert "Preconnecting websocket audio channel" in start_body
    assert "OpenAudioChannel();" in start_body
    assert "wake word will retry on demand" in start_body
    assert "return true;" in start_body


def test_screen_snapshot_is_rejected_while_lesson_runtime_active():
    mcp_cc = read("main/mcp_server.cc")
    app_h = read("main/application.h")
    app_cc = read("main/application.cc")

    assert "bool IsLessonRuntimeActive() const;" in app_h
    assert "bool Application::IsLessonRuntimeActive() const" in app_cc
    snapshot_start = mcp_cc.index('AddUserOnlyTool("self.screen.snapshot"')
    preview_start = mcp_cc.index('AddUserOnlyTool("self.screen.preview_image"', snapshot_start)
    snapshot_body = mcp_cc[snapshot_start:preview_start]

    assert "Application::GetInstance().IsLessonRuntimeActive()" in snapshot_body
    assert "screen snapshot disabled during lesson" in snapshot_body
    assert snapshot_body.index("IsLessonRuntimeActive()") < snapshot_body.index("SnapshotToJpeg")


def test_lesson_snapshot_requires_explicit_opt_in_argument():
    mcp_cc = read("main/mcp_server.cc")

    snapshot_start = mcp_cc.index('AddUserOnlyTool("self.screen.snapshot"')
    preview_start = mcp_cc.index('AddUserOnlyTool("self.screen.preview_image"', snapshot_start)
    snapshot_body = mcp_cc[snapshot_start:preview_start]

    assert 'Property("allowDuringLesson", kPropertyTypeBoolean, false)' in snapshot_body
    assert 'properties["allowDuringLesson"].value<bool>()' in snapshot_body
    assert "IsLessonRuntimeActive() && !allow_during_lesson" in snapshot_body
    assert "screen snapshot disabled during lesson" in snapshot_body


def test_lesson_runtime_allows_only_explicit_snapshot_before_scheduling():
    mcp_cc = read("main/mcp_server.cc")

    assert "bool IsLessonSnapshotEvidenceCall(" in mcp_cc
    helper_start = mcp_cc.index("bool IsLessonSnapshotEvidenceCall(")
    helper_end = mcp_cc.index("\n}", helper_start)
    helper = mcp_cc[helper_start:helper_end]
    assert 'tool_name != "self.screen.snapshot"' in helper
    assert 'cJSON_GetObjectItem(tool_arguments, "allowDuringLesson")' in helper
    assert "cJSON_IsTrue(allow_during_lesson)" in helper

    start = mcp_cc.index("void McpServer::DoToolCall")
    body = mcp_cc[start:]
    before_schedule = body[: body.index("app.Schedule(")]
    assert "const bool lesson_tool_allowed =" in before_schedule
    assert "IsLessonSnapshotEvidenceCall(tool_name, tool_arguments)" in before_schedule
    assert "!lesson_tool_allowed &&" in before_schedule
    assert "Application::GetInstance().IsLessonRuntimeActive()" in before_schedule


def test_lesson_runtime_allows_only_fixed_server_owned_motion_tools():
    mcp_cc = read("main/mcp_server.cc")

    assert "bool IsLessonMotionToolName(" in mcp_cc
    helper_start = mcp_cc.index("bool IsLessonMotionToolName(")
    helper_end = mcp_cc.index("\n}", helper_start)
    helper = mcp_cc[helper_start:helper_end]
    expected = {
        "self.robot.left_arm_raise",
        "self.robot.right_arm_raise",
        "self.robot.left_arm_lower",
        "self.robot.right_arm_lower",
        "self.robot.both_arms_raise",
        "self.robot.both_arms_lower",
        "self.robot.head_turn_left",
        "self.robot.head_turn_right",
        "self.robot.head_center",
    }
    for tool_name in expected:
        assert f'tool_name == "{tool_name}"' in helper
    assert "set_percent" not in helper
    assert "set_angle" not in helper

    start = mcp_cc.index("void McpServer::DoToolCall")
    before_schedule = mcp_cc[start : mcp_cc.index("app.Schedule(", start)]
    assert "IsLessonMotionToolName(tool_name)" in before_schedule


def test_lesson_snapshot_allowance_is_rechecked_after_scheduling():
    mcp_cc = read("main/mcp_server.cc")

    start = mcp_cc.index("void McpServer::DoToolCall")
    body = mcp_cc[start:]
    scheduled = body[body.index("app.Schedule(") :]

    assert "lesson_tool_allowed" in scheduled.split("]()", 1)[0]
    assert "if (!lesson_tool_allowed &&" in scheduled
    assert "scheduled MCP tool call rejected during lesson" in scheduled
    assert scheduled.index("if (!lesson_tool_allowed &&") < scheduled.index(
        "(*tool_iter)->Call(arguments)"
    )


def test_lesson_runtime_rejects_mcp_tool_calls_before_scheduling():
    mcp_cc = read("main/mcp_server.cc")

    start = mcp_cc.index("void McpServer::DoToolCall")
    body = mcp_cc[start:]

    assert "Application::GetInstance().IsLessonRuntimeActive()" in body
    assert 'ReplyError(id, "MCP tools disabled during lesson");' in body
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index("app.Schedule(")
    guard = body[
        body.index("Application::GetInstance().IsLessonRuntimeActive()") :
        body.index("PropertyList arguments")
    ]
    assert "return;" in guard


def test_lesson_runtime_rejects_scheduled_mcp_tool_calls_before_callback():
    mcp_cc = read("main/mcp_server.cc")

    start = mcp_cc.index("void McpServer::DoToolCall")
    body = mcp_cc[start:]
    scheduled = body[body.index("app.Schedule(") :]

    assert "Application::GetInstance().IsLessonRuntimeActive()" in scheduled
    assert "scheduled MCP tool call rejected during lesson" in scheduled
    assert 'ReplyError(id, "MCP tools disabled during lesson");' in scheduled
    assert scheduled.index("Application::GetInstance().IsLessonRuntimeActive()") < scheduled.index(
        "(*tool_iter)->Call(arguments)"
    )
    guard = scheduled[
        scheduled.index("Application::GetInstance().IsLessonRuntimeActive()") :
        scheduled.index("(*tool_iter)->Call(arguments)")
    ]
    assert "return;" in guard


def test_lesson_runtime_rejects_system_reboot_before_scheduling():
    app_cc = read("main/application.cc")

    system_start = app_cc.index('strcmp(type->valuestring, "system") == 0')
    alert_start = app_cc.index('strcmp(type->valuestring, "alert") == 0', system_start)
    system_body = app_cc[system_start:alert_start]

    reboot_start = system_body.index('strcmp(command->valuestring, "reboot") == 0')
    reboot_body = system_body[reboot_start:]

    assert "lesson_runtime_active_.load()" in reboot_body
    assert reboot_body.index("lesson_runtime_active_.load()") < reboot_body.index("Schedule([this]()")
    guard = reboot_body[
        reboot_body.index("lesson_runtime_active_.load()") :
        reboot_body.index("Schedule([this]()")
    ]
    assert "return;" in guard
    assert "Reboot();" not in guard


def test_lesson_runtime_direct_reboot_ignored_before_restart_side_effects():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::Reboot()")
    end = app_cc.index("bool Application::UpgradeFirmware", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson reboot ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index("CloseAudioChannelByIntent();")
    assert body.index("lesson_runtime_active_.load()") < body.index("audio_service_.Stop();")
    assert body.index("lesson_runtime_active_.load()") < body.index("esp_restart();")
    guard = body[
        body.index("lesson_runtime_active_.load()") :
        body.index("CloseAudioChannelByIntent();")
    ]
    assert "return;" in guard
    assert "esp_restart" not in guard


def test_lesson_runtime_direct_firmware_upgrade_ignored_before_ota_side_effects():
    app_cc = read("main/application.cc")

    start = app_cc.index("bool Application::UpgradeFirmware")
    end = app_cc.index("void Application::WakeWordInvoke", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson firmware upgrade ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index("CloseAudioChannelByIntent();")
    assert body.index("lesson_runtime_active_.load()") < body.index("Alert(Lang::Strings::OTA_UPGRADE")
    assert body.index("lesson_runtime_active_.load()") < body.index("audio_service_.Stop();")
    assert body.index("lesson_runtime_active_.load()") < body.index("Ota::Upgrade")
    guard = body[
        body.index("lesson_runtime_active_.load()") :
        body.index("CloseAudioChannelByIntent();")
    ]
    assert "return false;" in guard
    assert "Ota::Upgrade" not in guard


def test_lesson_runtime_reset_protocol_ignored_before_scheduling_teardown():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::ResetProtocol()")
    body = app_cc[start:]

    assert "lesson_runtime_active_.load()" in body
    guard_idx = body.index("lesson_runtime_active_.load()")
    schedule_idx = body.index("Schedule([this]()")
    generation_idx = body.index("++connect_generation_;")
    reset_idx = body.index("DoResetProtocol();")
    assert guard_idx < schedule_idx < generation_idx < reset_idx
    guard = body[guard_idx:schedule_idx]
    assert "return;" in guard
    assert "Schedule(" not in guard
    assert "connect_generation_" not in guard
    assert "reset_pending_" not in guard
    assert "DoResetProtocol" not in guard


def test_protocol_and_network_resets_abandon_exact_lesson_asset_session():
    app_cc = read("main/application.cc")
    disconnect_start = app_cc.index("void Application::HandleNetworkDisconnectedEvent()")
    disconnect_end = app_cc.index("void Application::HandleActivationDoneEvent()", disconnect_start)
    reset_start = app_cc.index("void Application::DoResetProtocol()")
    reset_end = app_cc.index("void Application::ResetProtocol()", reset_start)

    assert "RequestLessonStorageAbandonment();" in app_cc[disconnect_start:disconnect_end]
    assert "RequestLessonStorageAbandonment();" in app_cc[reset_start:reset_end]
    assert "AbandonLessonStorageSession();" not in app_cc[disconnect_start:disconnect_end]
    assert "AbandonLessonStorageSession();" not in app_cc[reset_start:reset_end]
    assert "ForceEndLessonSession" not in app_cc[disconnect_start:disconnect_end]
    assert "ForceEndLessonSession" not in app_cc[reset_start:reset_end]

def test_lesson_runtime_suppresses_low_battery_popup_and_sound_before_interrupting_scene():
    lvgl_cc = read("main/display/lvgl_display/lvgl_display.cc")
    app_h = read("main/application.h")

    assert "bool IsLessonRuntimeActive() const;" in app_h

    start = lvgl_cc.index("void LvglDisplay::UpdateStatusBar")
    end = lvgl_cc.index("    // Update network icon", start)
    body = lvgl_cc[start:end]

    assert "app.IsLessonRuntimeActive()" in body
    low_battery_start = body.index("if (low_battery_popup_ != nullptr && !update_all)")
    low_battery_body = body[low_battery_start:]
    assert low_battery_body.index("app.IsLessonRuntimeActive()") < low_battery_body.index(
        "lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);"
    )
    assert low_battery_body.index("app.IsLessonRuntimeActive()") < low_battery_body.index(
        "app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);"
    )
    lesson_guard = low_battery_body[
        low_battery_body.index("app.IsLessonRuntimeActive()") :
        low_battery_body.index("lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);")
    ]
    assert "lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);" in lesson_guard
    assert "OGG_LOW_BATTERY" not in lesson_guard

def test_lesson_runtime_suppresses_lvgl_clock_status_before_overwriting_lesson_status():
    lvgl_cc = read("main/display/lvgl_display/lvgl_display.cc")

    start = lvgl_cc.index("void LvglDisplay::UpdateStatusBar")
    time_start = lvgl_cc.index("    // Update time", start)
    battery_start = lvgl_cc.index("    esp_pm_lock_acquire(pm_lock_);", time_start)
    time_body = lvgl_cc[time_start:battery_start]

    assert "app.IsLessonRuntimeActive()" in time_body
    assert time_body.index("app.IsLessonRuntimeActive()") < time_body.index("SetStatus(time_str);")
    guard = time_body[
        time_body.index("app.GetDeviceState() == kDeviceStateIdle") :
        time_body.index("SetStatus(time_str);")
    ]
    assert "!app.IsLessonRuntimeActive()" in guard

def test_lesson_runtime_suppresses_lvgl_notifications_before_overlaying_scene():
    lvgl_cc = read("main/display/lvgl_display/lvgl_display.cc")

    start = lvgl_cc.index("void LvglDisplay::ShowNotification(const char* notification")
    end = lvgl_cc.index("void LvglDisplay::UpdateStatusBar", start)
    body = lvgl_cc[start:end]

    assert "Application::GetInstance().IsLessonRuntimeActive()" in body
    assert "lesson notification suppressed" in body
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index(
        "DisplayLockGuard lock(this);"
    )
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index(
        "lv_label_set_text(notification_label_, notification);"
    )
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index(
        "lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);"
    )
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index(
        "esp_timer_start_once(notification_timer_, duration_ms * 1000)"
    )
    guard = body[
        body.index("Application::GetInstance().IsLessonRuntimeActive()") :
        body.index("DisplayLockGuard lock(this);")
    ]
    assert "return;" in guard
    assert "lv_label_set_text" not in guard
    assert "esp_timer_start_once" not in guard

def test_lesson_runtime_suppresses_lvgl_power_save_repaint_before_overwriting_scene():
    lvgl_cc = read("main/display/lvgl_display/lvgl_display.cc")

    start = lvgl_cc.index("void LvglDisplay::SetPowerSaveMode")
    end = lvgl_cc.index("bool LvglDisplay::SnapshotToJpeg", start)
    body = lvgl_cc[start:end]

    assert "Application::GetInstance().IsLessonRuntimeActive()" in body
    assert "lesson power save repaint suppressed" in body
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index(
        'SetChatMessage("system", "");'
    )
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index(
        'SetEmotion("sleepy");'
    )
    assert body.index("Application::GetInstance().IsLessonRuntimeActive()") < body.index(
        'SetEmotion("neutral");'
    )
    guard = body[
        body.index("Application::GetInstance().IsLessonRuntimeActive()") :
        body.index('SetChatMessage("system", "");')
    ]
    assert "return;" in guard
    assert "SetChatMessage" not in guard
    assert "SetEmotion" not in guard

def test_lesson_runtime_blocks_sleep_mode_eligibility_before_idle_checks():
    app_cc = read("main/application.cc")
    start = app_cc.index("bool Application::CanEnterSleepMode()")
    end = app_cc.index("void Application::SendMcpMessage", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    guard_idx = body.index("lesson_runtime_active_.load()")
    state_idx = body.index("GetDeviceState()")
    channel_idx = body.index("protocol_->IsAudioChannelOpened()")
    audio_idx = body.index("audio_service_.IsIdle()")
    true_idx = body.index("return true;")
    assert guard_idx < state_idx < channel_idx < audio_idx < true_idx
    guard = body[guard_idx:state_idx]
    assert "return false;" in guard
    assert "GetDeviceState" not in guard
    assert "IsAudioChannelOpened" not in guard
    assert "IsIdle" not in guard

def test_lesson_runtime_repair_pairing_ignored_before_claim_reset_wifi_clear_and_reboot():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::EnterRepairPairingMode()")
    end = app_cc.index("void Application::MaybeDispatchDeferredCloudRelease", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson re-pair ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index("StopHeartbeat();")
    assert body.index("lesson_runtime_active_.load()") < body.index("CloseAudioChannelByIntent();")
    assert body.index("lesson_runtime_active_.load()") < body.index("SystemReset::ReleaseCloudOwnership()")
    assert body.index("lesson_runtime_active_.load()") < body.index('claim_state.SetInt("confirmed", 0);')
    assert body.index("lesson_runtime_active_.load()") < body.index("SsidManager::GetInstance().Clear();")
    assert body.index("lesson_runtime_active_.load()") < body.index("esp_restart();")
    guard = body[
        body.index("lesson_runtime_active_.load()") :
        body.index("StopHeartbeat();")
    ]
    assert "return;" in guard
    assert "SystemReset::ReleaseCloudOwnership" not in guard
    assert 'SetInt("confirmed", 0)' not in guard
    assert "SsidManager::GetInstance().Clear" not in guard
    assert "esp_restart" not in guard

def test_lesson_runtime_defers_heartbeat_auth_failure_until_lesson_end():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    assert "deferred_heartbeat_auth_failure_status_" in app_h

    start = app_cc.index("void Application::HandleHeartbeatAuthFailure")
    end = app_cc.index("void Application::EnterRepairPairingMode", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "deferred_heartbeat_auth_failure_status_.store(status_code)" in body
    assert body.index("lesson_runtime_active_.load()") < body.index("CloseAudioChannelByIntent();")
    assert body.index("lesson_runtime_active_.load()") < body.index('backend_settings.SetString("device_secret", "");')
    assert body.index("lesson_runtime_active_.load()") < body.index('claim_state.SetInt("confirmed", 0);')
    assert body.index("lesson_runtime_active_.load()") < body.index("RenderClaimSubstate(claim_substate_);")
    guard = body[
        body.index("lesson_runtime_active_.load()") :
        body.index("CloseAudioChannelByIntent();")
    ]
    assert "StopHeartbeat();" in guard
    assert "return;" in guard
    assert 'SetString("device_secret", "")' not in guard
    assert 'SetInt("confirmed", 0)' not in guard
    assert "RenderClaimSubstate" not in guard

    start = app_cc.index("void Application::SetLessonRuntimeActive")
    end = app_cc.index("bool Application::IsLessonRuntimeActive", start)
    setter = app_cc[start:end]

    assert "deferred_heartbeat_auth_failure_status_.exchange(0)" in setter
    assert "HandleHeartbeatAuthFailure(status_code)" in setter
    exchange = setter.index("deferred_heartbeat_auth_failure_status_.exchange(0)")
    assert setter.index("lesson_runtime_active_.store(false);") < exchange
    post_lesson = setter[setter.index("deferred_heartbeat_auth_failure_status_.exchange(0)") :]
    assert "Schedule([this, status_code]()" in post_lesson

def test_lesson_runtime_deactivation_invalidates_stale_child_turn():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::SetLessonRuntimeActive")
    end = app_cc.index("bool Application::IsLessonRuntimeActive", start)
    setter = app_cc[start:end]
    inactive_branch = setter[
        setter.index("if (!active)") :
        setter.index("xEventGroupSetBits(event_group_", setter.index("if (!active)"))
    ]

    assert "lesson_interactive_listen_generation_.fetch_add(1);" in inactive_branch
    assert "lesson_interactive_listen_pending_.store(false);" in inactive_branch
    assert "lesson_interactive_listening_active_.store(false);" in inactive_branch
    assert inactive_branch.index("lesson_interactive_listen_generation_.fetch_add(1);") < inactive_branch.index(
        "deferred_heartbeat_auth_failure_status_.exchange(0)"
    )
    assert inactive_branch.index("lesson_interactive_listen_pending_.store(false);") < inactive_branch.index(
        "deferred_heartbeat_auth_failure_status_.exchange(0)"
    )

def test_lesson_runtime_ignores_activation_done_before_idle_repaint_and_success_sound():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleActivationDoneEvent()")
    end = app_cc.index("void Application::RefreshPendingTbotClaim()", start)
    body = app_cc[start:end]

    assert "if (lesson_runtime_active_.load())" in body
    assert body.index("if (lesson_runtime_active_.load())") < body.index("SetDeviceState(kDeviceStateIdle);")
    assert body.index("if (lesson_runtime_active_.load())") < body.index("display->ShowNotification")
    assert body.index("if (lesson_runtime_active_.load())") < body.index("audio_service_.PlaySound")
    guard = body[
        body.index("if (lesson_runtime_active_.load())") :
        body.index("SetDeviceState(kDeviceStateIdle);")
    ]
    assert "return;" in guard
    assert "display->SetChatMessage" not in guard

def test_preconnected_wake_word_can_continue_from_idle_state():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::ContinueWakeWordInvoke")
    end = app_cc.index("void Application::HandleStateChangedEvent", start)
    body = app_cc[start:end]
    assert "state != kDeviceStateConnecting && state != kDeviceStateIdle" in body
    assert "protocol_->SendWakeWordDetected(wake_word);" in body
    assert "SetListeningMode(kListeningModeAutoStop);" in body


def test_wake_word_listening_uses_autostop_even_when_default_mode_is_realtime():
    app_cc = read("main/application.cc")

    default_mode = app_cc[app_cc.index("ListeningMode Application::GetDefaultListeningMode") :]
    assert "return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;" in default_mode

    start = app_cc.index("void Application::FinishWakeWordInvoke")
    end = app_cc.index("// H3: localized screen copy", start)
    body = app_cc[start:end]

    # Wake-word turns should be finite. If this follows GetDefaultListeningMode(),
    # CONFIG_USE_SERVER_AEC/DEVICE_AEC pushes the turn into realtime mode and the
    # robot can stay Listening forever when the backend waits for a client stop.
    assert "SetListeningMode(kListeningModeAutoStop);" in body
    assert "SetListeningMode(GetDefaultListeningMode());" not in body


def test_wake_word_data_is_sent_after_listen_control_frames():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::FinishWakeWordInvoke")
    end = app_cc.index("// H3: localized screen copy", start)
    body = app_cc[start:end]

    assert body.index("protocol_->SendWakeWordDetected(wake_word);") < body.index(
        "SetListeningMode(kListeningModeAutoStop);"
    )
    assert body.index("SetListeningMode(kListeningModeAutoStop);") < body.index(
        "audio_service_.PopWakeWordPacket()"
    )
    assert body.index("SetListeningMode(kListeningModeAutoStop);") < body.index(
        "protocol_->SendAudio(std::move(packet));"
    )


def test_protocol_idle_timeout_allows_sixty_minute_sessions():
    protocol_cc = read("main/protocols/protocol.cc")

    timeout_start = protocol_cc.index("bool Protocol::IsTimeout() const")
    timeout_body = protocol_cc[timeout_start:]

    assert "server keeps WebSocket sessions open for 61 minutes" in timeout_body
    assert "const int kTimeoutSeconds = 61 * 60;" in timeout_body
    assert "const int kTimeoutSeconds = 25;" not in timeout_body


def test_websocket_open_resets_idle_timer_before_hello_send():
    websocket_cc = read("main/protocols/websocket_protocol.cc")

    open_start = websocket_cc.index("bool WebsocketProtocol::OpenAudioChannel()")
    open_end = websocket_cc.index("std::string WebsocketProtocol::GetHelloMessage", open_start)
    open_body = websocket_cc[open_start:open_end]

    assert "last_incoming_time_ = std::chrono::steady_clock::now();" in open_body
    reset_idx = open_body.index("last_incoming_time_ = std::chrono::steady_clock::now();")
    create_idx = open_body.index("auto replacement_websocket = network->CreateWebSocket(1);")
    hello_idx = open_body.index("if (!replacement_websocket->Send(message))")

    assert reset_idx < create_idx < hello_idx


def test_websocket_audio_uplink_refreshes_timeout_activity_after_successful_send():
    websocket_cc = read("main/protocols/websocket_protocol.cc")

    send_audio_start = websocket_cc.index("bool WebsocketProtocol::SendAudio")
    send_audio_end = websocket_cc.index("bool WebsocketProtocol::SendText", send_audio_start)
    send_audio_body = websocket_cc[send_audio_start:send_audio_end]

    assert "if (sent) {" in send_audio_body
    assert "last_incoming_time_ = std::chrono::steady_clock::now();" in send_audio_body
    refresh_idx = send_audio_body.index("last_incoming_time_ = std::chrono::steady_clock::now();")
    log_idx = send_audio_body.index("static uint32_t send_audio_count = 0;")

    assert refresh_idx < log_idx


def test_lesson_owned_turns_ignore_wake_word_hijack():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleWakeWordDetectedEvent")
    end = app_cc.index("void Application::ContinueWakeWordInvoke", start)
    body = app_cc[start:end]

    assert "lesson_interactive_listen_pending_.load()" in body
    assert "lesson_interactive_listening_active_.load()" in body
    assert "lesson wake ignored" in body
    assert body.index("lesson_interactive_listen_pending_.load()") < body.index(
        "AbortSpeaking(kAbortReasonWakeWordDetected);"
    )
    assert body.index("lesson_interactive_listening_active_.load()") < body.index(
        "AbortSpeaking(kAbortReasonWakeWordDetected);"
    )


def test_lesson_runtime_wake_word_ignores_non_answer_detection():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleWakeWordDetectedEvent")
    end = app_cc.index("void Application::ContinueWakeWordInvoke", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson wake ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index(
        "if (state == kDeviceStateIdle)"
    )
    assert body.index("lesson_runtime_active_.load()") < body.index(
        "AbortSpeaking(kAbortReasonWakeWordDetected);"
    )


def test_lesson_runtime_direct_wake_word_invoke_ignored_before_audio_state_changes():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::WakeWordInvoke")
    end = app_cc.index("bool Application::CanEnterSleepMode", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson direct wake ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index("if (state == kDeviceStateIdle)")
    assert body.index("lesson_runtime_active_.load()") < body.index("ContinueWakeWordInvoke(wake_word);")
    assert body.index("lesson_runtime_active_.load()") < body.index("AbortSpeaking(kAbortReasonNone);")
    guard = body[
        body.index("lesson_runtime_active_.load()") :
        body.index("if (state == kDeviceStateIdle)")
    ]
    assert "return;" in guard


def test_lesson_runtime_aec_mode_change_ignored_before_notification_and_channel_close():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::SetAecMode")
    end = app_cc.index("void Application::PlaySound", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson aec mode ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index("aec_mode_ = mode;")
    assert body.index("lesson_runtime_active_.load()") < body.index("Schedule([this")
    guard = body[
        body.index("lesson_runtime_active_.load()") :
        body.index("aec_mode_ = mode;")
    ]
    assert "return;" in guard
    assert "ShowNotification" not in guard
    assert "CloseAudioChannelByIntent" not in guard

def test_lesson_runtime_aec_scheduled_change_rechecks_before_ui_and_channel_close():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::SetAecMode")
    end = app_cc.index("void Application::PlaySound", start)
    body = app_cc[start:end]
    scheduled = body[
        body.index("Schedule([this") :
        body.index("});", body.index("Schedule([this"))
    ]

    assert "lesson_runtime_active_.load()" in scheduled
    guard_idx = scheduled.index("lesson_runtime_active_.load()")
    mode_idx = scheduled.index("aec_mode_ = mode;")
    aec_idx = scheduled.index("audio_service_.EnableDeviceAec")
    notify_idx = scheduled.index("display->ShowNotification")
    close_idx = scheduled.index("CloseAudioChannelByIntent();")
    assert guard_idx < mode_idx < aec_idx < notify_idx < close_idx
    guard = scheduled[guard_idx:mode_idx]
    assert "return;" in guard
    assert "aec_mode_" not in guard
    assert "EnableDeviceAec" not in guard
    assert "ShowNotification" not in guard
    assert "CloseAudioChannelByIntent" not in guard

def test_lesson_runtime_start_and_toggle_events_guard_before_setup_state_shortcuts():
    app_cc = read("main/application.cc")

    toggle_start = app_cc.index("void Application::HandleToggleChatEvent")
    toggle_end = app_cc.index("namespace {", toggle_start)
    toggle_body = app_cc[toggle_start:toggle_end]

    assert "if (lesson_runtime_active_.load())" in toggle_body
    assert toggle_body.index("if (lesson_runtime_active_.load())") < toggle_body.index(
        "SetDeviceState(kDeviceStateIdle);"
    )
    assert toggle_body.index("if (lesson_runtime_active_.load())") < toggle_body.index(
        "audio_service_.EnableAudioTesting(true);"
    )
    toggle_guard = toggle_body[
        toggle_body.index("if (lesson_runtime_active_.load())") :
        toggle_body.index("if (pending_tbot_claim_.active)")
    ]
    assert "return;" in toggle_guard

    start_listening_start = app_cc.index("void Application::HandleStartListeningEvent")
    start_listening_end = app_cc.index("void Application::HandleStopListeningEvent", start_listening_start)
    start_listening_body = app_cc[start_listening_start:start_listening_end]

    assert "const bool lesson_answer_turn =" in start_listening_body
    assert "if (lesson_runtime_active_.load() && !lesson_answer_turn)" in start_listening_body
    assert start_listening_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < start_listening_body.index(
        "SetDeviceState(kDeviceStateIdle);"
    )
    assert start_listening_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < start_listening_body.index(
        "audio_service_.EnableAudioTesting(true);"
    )
    start_guard = start_listening_body[
        start_listening_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") :
        start_listening_body.index("if (!protocol_)")
    ]
    assert "return;" in start_guard

def test_lesson_runtime_blocks_stale_wake_word_continuations():
    app_cc = read("main/application.cc")

    continue_start = app_cc.index("void Application::ContinueWakeWordInvoke")
    finish_start = app_cc.index("void Application::FinishWakeWordInvoke", continue_start)
    continue_body = app_cc[continue_start:finish_start]

    assert "lesson_runtime_active_.load()" in continue_body
    assert "lesson wake continue ignored" in continue_body
    assert continue_body.index("lesson_runtime_active_.load()") < continue_body.index(
        "StartOpenChannelWorker(ctx)"
    )

    finish_end = app_cc.index("// H3: localized screen copy", finish_start)
    finish_body = app_cc[finish_start:finish_end]

    assert "lesson_runtime_active_.load()" in finish_body
    assert "lesson wake finish ignored" in finish_body
    assert finish_body.index("lesson_runtime_active_.load()") < finish_body.index(
        "protocol_->SendWakeWordDetected(wake_word);"
    )
    assert finish_body.index("lesson_runtime_active_.load()") < finish_body.index(
        "SetListeningMode(kListeningModeAutoStop);"
    )

def test_lesson_runtime_blocks_stale_generic_open_continuations():
    app_cc = read("main/application.cc")

    continue_start = app_cc.index("void Application::ContinueOpenAudioChannel")
    task_start = app_cc.index("void Application::OpenChannelTask", continue_start)
    continue_body = app_cc[continue_start:task_start]

    assert "const bool lesson_answer_turn =" in continue_body
    assert "lesson_interactive_listen_pending_.load()" in continue_body
    assert "lesson_interactive_listening_active_.load()" in continue_body
    assert "if (lesson_runtime_active_.load() && !lesson_answer_turn)" in continue_body
    assert "lesson open channel ignored" in continue_body
    assert continue_body.index(
        "if (lesson_runtime_active_.load() && !lesson_answer_turn)"
    ) < continue_body.index("if (protocol_->IsAudioChannelOpened())")
    assert continue_body.index(
        "if (lesson_runtime_active_.load() && !lesson_answer_turn)"
    ) < continue_body.index("StartOpenChannelWorker(ctx)")

    task_end = app_cc.index("void Application::ArmConnectWatchdog", task_start)
    task_body = app_cc[task_start:task_end]
    callback_start = task_body.index("self->Schedule")
    wake_finish = task_body.index("self->FinishWakeWordInvoke(wake_word);", callback_start)
    generic_start = task_body.index("} else {", wake_finish)
    set_listening = task_body.index("self->SetListeningMode(mode);", generic_start)
    before_set = task_body[generic_start:set_listening]

    assert wake_finish < generic_start
    assert "lesson_interactive_listen_pending_.load()" in before_set
    assert "lesson_interactive_listening_active_.load()" in before_set
    assert "if (self->lesson_runtime_active_.load() && !lesson_answer_turn)" in before_set
    assert "lesson open worker ignored" in before_set
    assert "self->online_intent_.store(false);" in before_set
    assert before_set.index(
        "if (self->lesson_runtime_active_.load() && !lesson_answer_turn)"
    ) < before_set.index("self->online_intent_.store(false);")

def test_lesson_runtime_open_failure_clears_answer_turn_without_generic_reconnect():
    app_cc = read("main/application.cc")

    task_start = app_cc.index("void Application::OpenChannelTask")
    task_end = app_cc.index("void Application::ArmConnectWatchdog", task_start)
    task_body = app_cc[task_start:task_end]
    failure_start = task_body.index("} else {", task_body.index("if (ok)"))
    failure_body = task_body[failure_start: task_body.index("    });", failure_start)]

    lesson_start = failure_body.index("if (self->lesson_runtime_active_.load())")
    lesson_branch = failure_body[lesson_start: failure_body.index("return;", lesson_start)]

    assert "self->lesson_interactive_listen_generation_.fetch_add(1);" in lesson_branch
    assert "self->lesson_interactive_listen_pending_.store(false);" in lesson_branch
    assert "self->lesson_interactive_listening_active_.store(false);" in lesson_branch
    assert "self->online_intent_.store(false);" in lesson_branch
    assert "self->connect_attempt_active_.store(false);" in lesson_branch
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_branch
    assert "display->SetEmotion(" not in lesson_branch
    assert "self->ScheduleReconnect(mode);" not in lesson_branch
    assert lesson_branch.index("self->lesson_interactive_listen_generation_.fetch_add(1);") < lesson_branch.index(
        "self->lesson_interactive_listen_pending_.store(false);"
    )
    assert lesson_start < failure_body.index("if (passive_preconnect)")


def test_generic_open_worker_failure_cleans_context_and_recovers_idle():
    app_cc = read("main/application.cc")
    start = app_cc.index("void Application::ContinueOpenAudioChannel")
    end = app_cc.index("void Application::OpenChannelTask", start)
    body = app_cc[start:end]
    failure = body[body.index("if (!StartOpenChannelWorker(ctx))") :]

    assert "StartOpenChannelWorker(ctx)" in failure
    assert "delete ctx;" in failure
    assert "connect_in_flight_.store(false);" in failure
    assert "connect_attempt_active_.store(false);" in failure
    assert "CancelConnectWatchdog();" in failure
    assert 'ESP_LOGE(TAG, "ws_open worker unavailable -> idle");' in failure
    assert "SetDeviceState(kDeviceStateIdle);" in failure

def test_lesson_runtime_audio_open_callback_suppresses_stale_side_effects():
    app_cc = read("main/application.cc")

    opened_start = app_cc.index("protocol_->OnAudioChannelOpened")
    opened_end = app_cc.index("protocol_->OnAudioChannelClosed", opened_start)
    opened_body = app_cc[opened_start:opened_end]

    passive_branch = opened_body[
        opened_body.index("if (passive_ws_intent_.load())") :
        opened_body.index("} else {")
    ]
    claimed_lesson_guard = "if (IsDeviceClaimed() && !lesson_runtime_active_.load())"
    assert claimed_lesson_guard in passive_branch
    assert passive_branch.index(claimed_lesson_guard) < passive_branch.index(
        "audio_service_.EnableWakeWordDetection(true);"
    )

    generic_branch = opened_body[
        opened_body.index("} else {") :
        opened_body.index("backend_offline_.store(false);")
    ]
    assert "const bool lesson_answer_turn =" in generic_branch
    assert "lesson_interactive_listen_pending_.load()" in generic_branch
    assert "lesson_interactive_listening_active_.load()" in generic_branch
    assert "if (lesson_runtime_active_.load() && !lesson_answer_turn)" in generic_branch
    assert "lesson audio channel opened ignored" in generic_branch
    assert generic_branch.index(
        "if (lesson_runtime_active_.load() && !lesson_answer_turn)"
    ) < generic_branch.index("online_intent_.store(true);")
    guard_body = generic_branch[
        generic_branch.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") :
        generic_branch.index("online_intent_.store(true);")
    ]
    assert "online_intent_.store(false);" in guard_body
    assert "StopHeartbeat();" in guard_body
    assert "StartHeartbeat();" not in guard_body
    assert "DispatchDeviceHeartbeat();" not in guard_body

def test_lesson_runtime_protocol_connected_suppresses_stale_heartbeat_before_dispatch():
    app_cc = read("main/application.cc")

    connected_start = app_cc.index("protocol_->OnConnected")
    connected_end = app_cc.index("protocol_->OnNetworkError", connected_start)
    connected_body = app_cc[connected_start:connected_end]

    assert "const bool lesson_answer_turn =" in connected_body
    assert "lesson_interactive_listen_pending_.load()" in connected_body
    assert "lesson_interactive_listening_active_.load()" in connected_body
    assert "if (lesson_runtime_active_.load() && !lesson_answer_turn)" in connected_body
    assert "lesson protocol connected without heartbeat" in connected_body
    assert connected_body.index(
        "if (lesson_runtime_active_.load() && !lesson_answer_turn)"
    ) < connected_body.index("StartHeartbeat();")
    assert connected_body.index(
        "if (lesson_runtime_active_.load() && !lesson_answer_turn)"
    ) < connected_body.index("DispatchDeviceHeartbeat();")
    guard_body = connected_body[
        connected_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") :
        connected_body.index("StartHeartbeat();")
    ]
    assert "StopHeartbeat();" in guard_body
    assert "return;" in guard_body
    assert "StartHeartbeat();" not in guard_body
    assert "DispatchDeviceHeartbeat();" not in guard_body

def test_listening_state_uses_distinct_child_turn_face():
    app_cc = read("main/application.cc")

    listening_start = app_cc.index("case kDeviceStateListening:")
    speaking_start = app_cc.index("case kDeviceStateSpeaking:", listening_start)
    listening_body = app_cc[listening_start:speaking_start]

    assert "display->SetStatus(Lang::Strings::LISTENING);" in listening_body
    assert 'display->SetEmotion("thinking");' in listening_body
    assert 'display->SetEmotion("neutral");' not in listening_body


def test_lesson_active_child_listening_repaint_preserves_child_turn_cue():
    app_cc = read("main/application.cc")

    listening_start = app_cc.index("case kDeviceStateListening:")
    speaking_start = app_cc.index("case kDeviceStateSpeaking:", listening_start)
    listening_body = app_cc[listening_start:speaking_start]

    assert "const bool lesson_interactive_active = lesson_interactive_listening_active_.load();" in listening_body
    assert "if (lesson_interactive_listen || lesson_interactive_active)" in listening_body
    child_cue = listening_body[
        listening_body.index("if (lesson_interactive_listen || lesson_interactive_active)") :
        listening_body.index("} else {", listening_body.index("if (lesson_interactive_listen || lesson_interactive_active)"))
    ]
    assert 'display->SetStatus("Con nói nhé...");' in child_cue
    assert 'display->SetChatMessage("system", "Con nói nhé.");' in child_cue
    assert 'display->SetEmotion("thinking");' not in child_cue


def test_speaking_state_uses_default_child_friendly_face():
    app_cc = read("main/application.cc")

    speaking_start = app_cc.index("case kDeviceStateSpeaking:")
    wifi_config_start = app_cc.index("case kDeviceStateWifiConfiguring:", speaking_start)
    speaking_body = app_cc[speaking_start:wifi_config_start]

    assert "display->SetStatus(Lang::Strings::SPEAKING);" in speaking_body
    assert 'display->SetEmotion("happy");' in speaking_body

    llm_start = app_cc.index('strcmp(type->valuestring, "llm") == 0')
    mcp_start = app_cc.index('strcmp(type->valuestring, "mcp") == 0', llm_start)
    llm_body = app_cc[llm_start:mcp_start]
    assert "display->SetEmotion(emotion_str.c_str());" in llm_body


def test_lesson_runtime_speaking_state_preserves_authored_lesson_face():
    app_cc = read("main/application.cc")

    speaking_start = app_cc.index("case kDeviceStateSpeaking:")
    wifi_config_start = app_cc.index("case kDeviceStateWifiConfiguring:", speaking_start)
    speaking_body = app_cc[speaking_start:wifi_config_start]

    assert "display->SetStatus(Lang::Strings::SPEAKING);" in speaking_body
    assert "if (!lesson_runtime_active_.load())" in speaking_body
    assert speaking_body.index("if (!lesson_runtime_active_.load())") < speaking_body.index(
        'display->SetEmotion("happy");'
    )

def test_lesson_runtime_dismiss_alert_preserves_lesson_ui():
    app_cc = read("main/application.cc")

    dismiss_start = app_cc.index("void Application::DismissAlert()")
    dismiss_end = app_cc.index("void Application::ToggleChatState()", dismiss_start)
    dismiss_body = app_cc[dismiss_start:dismiss_end]

    assert "!lesson_runtime_active_.load()" in dismiss_body
    assert dismiss_body.index("!lesson_runtime_active_.load()") < dismiss_body.index(
        "display->SetStatus(Lang::Strings::STANDBY);"
    )
    assert dismiss_body.index("!lesson_runtime_active_.load()") < dismiss_body.index(
        'display->SetEmotion("neutral");'
    )
    assert dismiss_body.index("!lesson_runtime_active_.load()") < dismiss_body.index(
        'display->SetChatMessage("system", "");'
    )

def test_lesson_runtime_alert_preserves_lesson_ui_and_audio():
    app_cc = read("main/application.cc")

    alert_start = app_cc.index("void Application::Alert(")
    alert_end = app_cc.index("void Application::DismissAlert()", alert_start)
    alert_body = app_cc[alert_start:alert_end]

    assert "lesson_runtime_active_.load()" in alert_body
    assert "lesson alert suppressed" in alert_body
    assert alert_body.index("lesson_runtime_active_.load()") < alert_body.index("display->SetStatus(status);")
    assert alert_body.index("lesson_runtime_active_.load()") < alert_body.index("display->SetEmotion(emotion);")
    assert alert_body.index("lesson_runtime_active_.load()") < alert_body.index('display->SetChatMessage("system", message);')
    assert alert_body.index("lesson_runtime_active_.load()") < alert_body.index("audio_service_.PlaySound(sound);")
    guard = alert_body[
        alert_body.index("lesson_runtime_active_.load()") :
        alert_body.index("auto display = Board::GetInstance().GetDisplay();")
    ]
    assert "return;" in guard
    assert "SetStatus" not in guard
    assert "PlaySound" not in guard

def test_autostop_tts_stop_returns_to_idle_instead_of_reopening_mic_loop():
    app_cc = read("main/application.cc")

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    sentence_start = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop)
    stop_body = app_cc[stop:sentence_start]

    assert "listening_mode_ == kListeningModeAutoStop" in stop_body
    assert "SetDeviceState(kDeviceStateIdle);" in stop_body
    autostop_start = stop_body.index("listening_mode_ == kListeningModeAutoStop")
    autostop_body = stop_body[autostop_start:stop_body.index("} else", autostop_start)]
    assert "SetDeviceState(kDeviceStateListening)" not in autostop_body
    assert "mic_loop_resumed" not in autostop_body


def test_autostop_tts_stop_drains_playback_before_returning_to_idle():
    app_cc = read("main/application.cc")

    assert "kTtsStopPlaybackDrainTimeoutMs" in app_cc

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    sentence_start = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop)
    stop_body = app_cc[stop:sentence_start]

    assert "audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs)" in stop_body
    assert "tts_stop_playback_drain_timeout" in stop_body

    autostop_start = stop_body.index("listening_mode_ == kListeningModeAutoStop")
    autostop_body = stop_body[autostop_start:stop_body.index("} else", autostop_start)]
    assert autostop_body.index("audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs)") < autostop_body.index(
        "SetDeviceState(kDeviceStateIdle);"
    )


def test_tts_stop_playback_drain_stays_below_main_task_watchdog_budget():
    app_cc = read("main/application.cc")
    timeout = re.search(
        r"kTtsStopPlaybackDrainTimeoutMs\s*=\s*(\d+)", app_cc
    )
    assert timeout is not None
    assert int(timeout.group(1)) < 10_000


def test_google_live_tts_stop_continue_listening_reopens_realtime_mic_loop():
    app_cc = read("main/application.cc")

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    sentence_start = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop)
    stop_body = app_cc[stop:sentence_start]

    assert 'cJSON_GetObjectItem(root, "continue_listening")' in stop_body
    assert "cJSON_IsTrue(continue_listening)" in stop_body
    assert 'cJSON_GetObjectItem(root, "listen_mode")' in stop_body
    assert 'strcmp(listen_mode->valuestring, "realtime") == 0' in stop_body
    assert "force_continue_listening" in stop_body
    continue_start = stop_body.index("force_continue_listening")
    continue_body = stop_body[continue_start:stop_body.index("if (GetDeviceState() == kDeviceStateSpeaking)", continue_start)]
    assert "listening_mode_ = kListeningModeRealtime;" in continue_body
    assert "SetDeviceState(kDeviceStateListening);" in continue_body
    assert "protocol_->SendStartListening(kListeningModeRealtime);" in continue_body
    assert "mic_loop_resumed" in continue_body


def test_google_live_manual_tts_stop_exits_stale_listening_state():
    app_cc = read("main/application.cc")

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    sentence_start = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop)
    stop_body = app_cc[stop:sentence_start]

    assert "explicit_stop_listening" in stop_body
    assert 'strcmp(listen_mode->valuestring, "manual") == 0' in stop_body
    explicit_start = stop_body.index("if (explicit_stop_listening")
    explicit_body = stop_body[explicit_start:]
    assert "GetDeviceState() == kDeviceStateListening" in explicit_body
    assert "audio_service_.EnableVoiceProcessing(false);" in explicit_body
    assert "listening_started_ms_.store(0);" in explicit_body
    assert "last_listening_activity_ms_.store(0);" in explicit_body
    assert "SetDeviceState(kDeviceStateIdle);" in explicit_body
    assert explicit_body.index("audio_service_.EnableVoiceProcessing(false);") < explicit_body.index(
        "SetDeviceState(kDeviceStateIdle);"
    )

def test_lesson_prompt_tts_stop_continue_listening_does_not_take_over_realtime():
    app_cc = read("main/application.cc")

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    sentence_start = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop)
    stop_body = app_cc[stop:sentence_start]
    schedule_body = stop_body[stop_body.index("Schedule([this") :]

    assert "const bool lesson_interactive_turn =" in schedule_body
    turn_decl_start = schedule_body.index("const bool lesson_interactive_turn =")
    turn_decl = schedule_body[turn_decl_start : schedule_body.index(";", turn_decl_start)]
    assert "lesson_interactive_listen_pending_.load()" in turn_decl
    assert "lesson_interactive_listening_active_.load()" in turn_decl
    assert "||" in turn_decl
    assert "if (force_continue_listening && !lesson_interactive_turn)" in schedule_body
    assert schedule_body.index("const bool lesson_interactive_turn") < schedule_body.index(
        "if (force_continue_listening"
    )

    force_branch = schedule_body[
        schedule_body.index("if (force_continue_listening") :
        schedule_body.index("if (GetDeviceState() == kDeviceStateSpeaking)")
    ]
    assert "protocol_->SendStartListening(kListeningModeRealtime);" in force_branch
    assert "protocol_->SendStartListening(kListeningModeManualStop);" not in force_branch


def test_lesson_runtime_tts_stop_continue_listening_ignores_non_answer_turn():
    app_cc = read("main/application.cc")

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    sentence_start = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop)
    stop_body = app_cc[stop:sentence_start]
    schedule_body = stop_body[stop_body.index("Schedule([this") :]

    assert "const bool lesson_interactive_turn =" in schedule_body
    turn_decl_start = schedule_body.index("const bool lesson_interactive_turn =")
    turn_decl = schedule_body[turn_decl_start : schedule_body.index(";", turn_decl_start)]
    assert "lesson_interactive_listen_pending_.load()" in turn_decl
    assert "lesson_interactive_listening_active_.load()" in turn_decl
    assert "||" in turn_decl
    assert "if (lesson_runtime_active_.load() && !lesson_interactive_turn)" in schedule_body
    assert schedule_body.index("if (lesson_runtime_active_.load() && !lesson_interactive_turn)") < schedule_body.index(
        "if (force_continue_listening && !lesson_interactive_turn)"
    )

    lesson_guard = schedule_body[
        schedule_body.index("if (lesson_runtime_active_.load() && !lesson_interactive_turn)") :
        schedule_body.index("if (force_continue_listening && !lesson_interactive_turn)")
    ]
    assert "lesson_idle_repaint_suppressed_.store(true);" in lesson_guard
    assert "SetDeviceState(kDeviceStateIdle);" in lesson_guard
    assert "protocol_->SendStartListening" not in lesson_guard
    assert "audio_service_.EnableVoiceProcessing(true);" not in lesson_guard


def test_lesson_terminal_stop_quarantines_only_matching_tts_generation():
    app_h = read("main/application.h")
    app_cc = read("main/application.cc")
    lesson_handler = read("main/lesson_handler.cc")

    assert "std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};" in app_h

    stop_branch = lesson_handler[
        lesson_handler.index('if (strcmp(type, "lesson_stop") == 0)') :
        lesson_handler.index('if (strcmp(type, "lesson_error") == 0)')
    ]
    assert stop_branch.index("BeginLessonTerminalAudioQuiet();") < stop_branch.index(
        "SetLessonRuntimeActive(false);"
    )

    begin = function_body(app_cc, "void Application::BeginLessonTerminalAudioQuiet")
    assert "lesson_terminal_audio_generation_.store(" in begin
    assert (
        "static_cast<std::uint64_t>(speaking_generation_.load()) + 1"
        in " ".join(begin.split())
    )

    tts_stop = app_cc[
        app_cc.index('strcmp(state->valuestring, "stop") == 0') :
        app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)')
    ]
    compact_stop = " ".join(tts_stop.split())
    assert "const std::uint64_t stopped_audio_generation" in tts_stop
    assert "static_cast<std::uint64_t>(speaking_generation_.load()) + 1" in compact_stop
    scheduled = tts_stop[tts_stop.index("Schedule([this") :]
    capture_list = scheduled[: scheduled.index("]()")]
    assert "stopped_audio_generation" in capture_list
    assert "lesson_terminal_audio_generation_.exchange(0)" in scheduled

    match_guard = scheduled[
        scheduled.index("if (terminal_audio_generation == stopped_audio_generation)") :
        scheduled.index("if (lesson_runtime_active_.load() && !lesson_interactive_turn)")
    ]
    assert "SetDeviceState(kDeviceStateIdle);" in match_guard
    assert "return;" in match_guard
    assert "terminal_audio_generation != 0" in match_guard
    assert "protocol_->SendStartListening" not in match_guard
    assert "audio_service_.EnableVoiceProcessing(true);" not in match_guard

    setter = function_body(app_cc, "void Application::SetLessonRuntimeActive")
    assert "lesson_terminal_audio_generation_.store(0);" in setter

def test_lesson_tts_stop_treats_active_child_listening_as_answer_turn():
    app_cc = read("main/application.cc")

    stop = app_cc.index('strcmp(state->valuestring, "stop") == 0')
    sentence_start = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop)
    stop_body = app_cc[stop:sentence_start]
    schedule_body = stop_body[stop_body.index("Schedule([this") :]

    turn_decl_start = schedule_body.index("const bool lesson_interactive_turn =")
    turn_decl = schedule_body[turn_decl_start : schedule_body.index(";", turn_decl_start)]
    assert "lesson_interactive_listen_pending_.load()" in turn_decl
    assert "lesson_interactive_listening_active_.load()" in turn_decl
    assert turn_decl.index("lesson_interactive_listen_pending_.load()") < turn_decl.index(
        "lesson_interactive_listening_active_.load()"
    )

    manual_start = schedule_body.index("if (listening_mode_ == kListeningModeManualStop)")
    manual_body = schedule_body[
        manual_start :
        schedule_body.index("} else if (listening_mode_ == kListeningModeAutoStop)", manual_start)
    ]
    assert "if (lesson_interactive_turn)" in manual_body
    assert "lesson_interactive_listen_pending_.load()" not in manual_body
    lesson_turn_body = manual_body[
        manual_body.index("if (lesson_interactive_turn)") :
        manual_body.index("} else {", manual_body.index("if (lesson_interactive_turn)"))
    ]
    assert "audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs)" in lesson_turn_body
    assert "tts_stop_playback_drain_timeout" in lesson_turn_body
    assert lesson_turn_body.index(
        "audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs)"
    ) < lesson_turn_body.index("SetDeviceState(kDeviceStateListening);")
    assert "SetDeviceState(kDeviceStateListening);" in manual_body
    assert "lesson prompt complete -> listening" in manual_body

def test_autostop_speaking_timeout_returns_to_idle_instead_of_reopening_mic_loop():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleSpeakingTimeout")
    end = app_cc.index("void Application::AbortSpeaking", start)
    body = app_cc[start:end]

    assert "listening_mode_ == kListeningModeAutoStop" in body
    autostop_start = body.index("listening_mode_ == kListeningModeAutoStop")
    autostop_body = body[autostop_start:body.index("} else", autostop_start)]
    assert "SetDeviceState(kDeviceStateIdle);" in autostop_body
    assert "SetDeviceState(kDeviceStateListening)" not in autostop_body
    assert "mic_loop_resumed" not in autostop_body


def test_finite_speaking_timeout_is_child_visible_before_idle():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleSpeakingTimeout")
    end = app_cc.index("void Application::AbortSpeaking", start)
    body = app_cc[start:end]

    assert "auto show_timeout_cue = [this]()" in body
    assert "display->SetStatus(Lang::Strings::SERVER_TIMEOUT);" in body
    assert 'display->SetEmotion("thinking");' in body
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);" in body

    manual_start = body.index("listening_mode_ == kListeningModeManualStop")
    manual_body = body[manual_start:body.index("} else if", manual_start)]
    assert "SetDeviceState(kDeviceStateIdle);" in manual_body
    assert "show_timeout_cue();" in manual_body
    assert manual_body.index("SetDeviceState(kDeviceStateIdle);") < manual_body.index("show_timeout_cue();")

    autostop_start = body.index("listening_mode_ == kListeningModeAutoStop")
    autostop_body = body[autostop_start:body.index("} else", autostop_start)]
    assert "SetDeviceState(kDeviceStateIdle);" in autostop_body
    assert "show_timeout_cue();" in autostop_body
    assert autostop_body.index("SetDeviceState(kDeviceStateIdle);") < autostop_body.index("show_timeout_cue();")

def test_lesson_runtime_speaking_timeout_keeps_child_wait_cue():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleSpeakingTimeout")
    end = app_cc.index("void Application::AbortSpeaking", start)
    body = app_cc[start:end]

    assert "if (lesson_runtime_active_.load())" in body
    assert body.index("if (lesson_runtime_active_.load())") < body.index(
        "display->SetStatus(Lang::Strings::SERVER_TIMEOUT);"
    )
    lesson_branch = body[
        body.index("if (lesson_runtime_active_.load())") :
        body.index("display->SetStatus(Lang::Strings::SERVER_TIMEOUT);")
    ]
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_branch
    assert "display->SetEmotion(" not in lesson_branch
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);" not in lesson_branch

def test_lesson_prompt_speaking_timeout_preserves_pending_child_turn():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleSpeakingTimeout")
    end = app_cc.index("void Application::AbortSpeaking", start)
    body = app_cc[start:end]

    assert "lesson_answer_turn" in body
    assert "lesson_runtime_active_.load() && lesson_interactive_listen_pending_.load()" in body
    assert body.index("lesson_answer_turn") < body.index("CancelLessonInteractiveListening();")
    cancel_guard = body[
        body.index("lesson_answer_turn") :
        body.index("CancelLessonInteractiveListening();")
    ]
    assert "if (!lesson_answer_turn)" in cancel_guard

    assert "CancelLessonInteractiveListening();" in body
    manual_start = body.index("if (listening_mode_ == kListeningModeManualStop)")
    manual_body = body[manual_start:body.index("} else if", manual_start)]
    assert "if (lesson_answer_turn)" in manual_body
    answer_turn = manual_body[manual_body.index("if (lesson_answer_turn)") :]
    assert "SetDeviceState(kDeviceStateListening);" in answer_turn
    assert answer_turn.index("SetDeviceState(kDeviceStateListening);") < answer_turn.index(
        "SetDeviceState(kDeviceStateIdle);"
    )
    assert answer_turn.index("SetDeviceState(kDeviceStateListening);") < answer_turn.index(
        "show_timeout_cue();"
    )


def test_listening_watchdog_exits_stale_turns_and_only_times_realtime_with_vad():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    assert "kListeningNoSpeechTimeoutMs" in app_cc
    assert "kListeningMaxTurnMs" in app_cc
    assert "last_listening_activity_ms_" in app_h
    assert "listening_started_ms_" in app_h
    assert "void Application::HandleListeningWatchdogTick" in app_cc
    assert "void HandleListeningWatchdogTick();" in app_h

    clock_start = app_cc.index("if (bits & MAIN_EVENT_CLOCK_TICK)")
    clock_end = app_cc.index("void Application::HandleNetworkConnectedEvent", clock_start)
    clock_body = app_cc[clock_start:clock_end]
    assert "HandleListeningWatchdogTick();" in clock_body

    watchdog_start = app_cc.index("void Application::HandleListeningWatchdogTick")
    watchdog_end = app_cc.index("void Application::HandleStateChangedEvent", watchdog_start)
    watchdog_body = app_cc[watchdog_start:watchdog_end]
    assert "GetDeviceState() != kDeviceStateListening" in watchdog_body
    assert "#if !CONFIG_USE_AUDIO_PROCESSOR || CONFIG_USE_DEVICE_AEC" in watchdog_body
    assert "listening_mode_ == kListeningModeRealtime" in watchdog_body
    assert "realtime_watchdog_disabled_without_vad" in watchdog_body
    assert "kListeningRealtimeNoSpeechTimeoutMs" in watchdog_body
    assert "kListeningRealtimeMaxTurnMs" not in app_cc
    assert "listening_mode_ != kListeningModeRealtime" in watchdog_body
    assert "turn_timed_out" in watchdog_body
    assert "listening_watchdog_timeout" in watchdog_body
    assert "protocol_->SendStopListening();" in watchdog_body
    assert "audio_service_.EnableVoiceProcessing(false);" in watchdog_body
    assert "audio_service_.PopPacketFromSendQueue()" in watchdog_body
    assert "IsVoiceDetected()" in watchdog_body
    audio_h = read("main/audio/audio_service.h")
    assert "std::atomic<bool> voice_detected_{false};" in audio_h
    assert "SetDeviceState(kDeviceStateIdle);" in watchdog_body



def test_microphone_uplink_requires_explicit_listening_authorization():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    assert "bool IsMicrophoneUplinkAuthorized() const;" in app_h
    assert "std::atomic<bool> microphone_uplink_authorized_{false};" in app_h
    assert "bool Application::IsMicrophoneUplinkAuthorized() const" in app_cc
    authorization_start = app_cc.index("bool Application::IsMicrophoneUplinkAuthorized() const")
    authorization_end = app_cc.index("void Application::HandleListeningWatchdogTick", authorization_start)
    authorization_body = app_cc[authorization_start:authorization_end]
    assert "const DeviceState state = GetDeviceState();" in authorization_body
    assert "state == kDeviceStateListening" in authorization_body
    assert "state == kDeviceStateSpeaking" in authorization_body
    assert "passive_ws_intent_.load()" in authorization_body
    assert "!online_intent_.load()" in authorization_body
    assert "lesson_runtime_active_.load()" in authorization_body
    assert "lesson_interactive_listening_active_.load()" in authorization_body
    assert "microphone_uplink_authorized_.load()" in authorization_body

    send_start = app_cc.index("if (bits & MAIN_EVENT_SEND_AUDIO)")
    send_end = app_cc.index("if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)", send_start)
    send_body = app_cc[send_start:send_end]
    assert "if (!IsMicrophoneUplinkAuthorized())" in send_body
    assert send_body.index("if (!IsMicrophoneUplinkAuthorized())") < send_body.index(
        "IsLessonNetworkRenderQuiet()"
    )
    unauthorized = send_body[
        send_body.index("if (!IsMicrophoneUplinkAuthorized())") :
        send_body.index("} else {", send_body.index("if (!IsMicrophoneUplinkAuthorized())"))
    ]
    assert "audio_service_.EnableVoiceProcessing(false);" in unauthorized
    assert "audio_service_.PopPacketFromSendQueue()" in unauthorized
    assert "microphone_uplink_blocked" in unauthorized

    state_start = app_cc.index("void Application::HandleStateChangedEvent")
    state_end = app_cc.index("void Application::Schedule", state_start)
    state_body = app_cc[state_start:state_end]
    assert "microphone_uplink_authorized_.store(true);" in state_body
    assert "microphone_uplink_authorized_.store(false);" in state_body

    close_start = app_cc.index("void Application::CloseAudioChannelByIntent")
    close_end = app_cc.index("void Application::DoResetProtocol", close_start)
    close_body = app_cc[close_start:close_end]
    assert "microphone_uplink_authorized_.store(false);" in close_body


def test_server_tts_stop_cannot_open_microphone_from_idle_or_passive_socket():
    app_cc = read("main/application.cc")
    tts_start = app_cc.index('if (strcmp(type->valuestring, "tts") == 0)')
    custom_start = app_cc.index('} else if (strcmp(type->valuestring, "stt") == 0)', tts_start)
    tts_body = app_cc[tts_start:custom_start]

    assert "const bool voice_turn_owned =" in tts_body
    ownership = tts_body[
        tts_body.index("const bool voice_turn_owned =") :
        tts_body.index("if (force_continue_listening", tts_body.index("const bool voice_turn_owned ="))
    ]
    assert "microphone_uplink_authorized_.load()" in ownership
    assert "!passive_ws_intent_.load()" in ownership
    assert "online_intent_.load()" in ownership
    assert "GetDeviceState() == kDeviceStateSpeaking" in ownership
    assert "GetDeviceState() == kDeviceStateListening" in ownership
    continue_branch = tts_body[tts_body.index("if (force_continue_listening") :]
    assert "if (!voice_turn_owned)" in continue_branch
    assert "tts_stop_continue_listening_rejected" in tts_body


def test_autostop_listening_has_shorter_hard_cap_than_manual_turns():
    app_cc = read("main/application.cc")

    assert "kListeningAutoStopMaxTurnMs" in app_cc
    assert "kListeningMaxTurnMs" in app_cc
    assert "kListeningAutoStopMaxTurnMs = 10000" in app_cc
    assert "kListeningMaxTurnMs = 60000" in app_cc

    watchdog_start = app_cc.index("void Application::HandleListeningWatchdogTick")
    watchdog_end = app_cc.index("void Application::HandleStateChangedEvent", watchdog_start)
    watchdog_body = app_cc[watchdog_start:watchdog_end]

    assert "turn_limit_ms" in watchdog_body
    assert "listening_mode_ == kListeningModeAutoStop" in watchdog_body
    assert "kListeningAutoStopMaxTurnMs" in watchdog_body
    assert "turn_ms >= turn_limit_ms" in watchdog_body
    assert "turn_timed_out" in watchdog_body


def test_wake_reopens_if_detect_frame_hits_stale_websocket():
    app_cc = read("main/application.cc")
    websocket_cc = read("main/protocols/websocket_protocol.cc")

    send_audio_start = websocket_cc.index("bool WebsocketProtocol::SendAudio")
    send_audio_end = websocket_cc.index("bool WebsocketProtocol::SendText", send_audio_start)
    send_audio_body = websocket_cc[send_audio_start:send_audio_end]
    assert "!IsAudioChannelOpened()" in send_audio_body
    assert send_audio_body.index("!IsAudioChannelOpened()") < send_audio_body.index(
        "const size_t payload_size"
    )

    send_text_start = websocket_cc.index("bool WebsocketProtocol::SendText")
    send_text_end = websocket_cc.index("bool WebsocketProtocol::IsAudioChannelOpened", send_text_start)
    send_text_body = websocket_cc[send_text_start:send_text_end]
    assert "!IsAudioChannelOpened()" in send_text_body
    assert send_text_body.index("!IsAudioChannelOpened()") < send_text_body.index(
        "websocket_->Send(text)"
    )

    finish_start = app_cc.index("void Application::FinishWakeWordInvoke")
    finish_end = app_cc.index("static const char* ConnectStateScreenCopy", finish_start)
    finish_body = app_cc[finish_start:finish_end]

    send_detect_idx = finish_body.index("protocol_->SendWakeWordDetected(wake_word);")
    reconnect_idx = finish_body.index("ContinueWakeWordInvoke(wake_word);", send_detect_idx)
    listen_idx = finish_body.index("SetListeningMode(kListeningModeAutoStop);")

    assert "wake_detect_send_failed -> reopen audio channel" in finish_body
    assert send_detect_idx < reconnect_idx < listen_idx
    assert "SetDeviceState(kDeviceStateConnecting);" in finish_body[send_detect_idx:reconnect_idx]


def test_listening_start_reconnects_if_listen_start_hits_stale_websocket():
    app_cc = read("main/application.cc")

    listening_start = app_cc.index("case kDeviceStateListening:")
    speaking_start = app_cc.index("case kDeviceStateSpeaking:", listening_start)
    listening_body = app_cc[listening_start:speaking_start]

    send_start_idx = listening_body.index("protocol_->SendStartListening(listening_mode_);")
    reconnect_idx = listening_body.index("ContinueOpenAudioChannel(mode);", send_start_idx)
    enable_vp_idx = listening_body.index("audio_service_.EnableVoiceProcessing(true);")

    assert "listen_start_send_failed -> reconnect" in listening_body
    assert send_start_idx < reconnect_idx < enable_vp_idx
    assert "SetDeviceState(kDeviceStateConnecting);" in listening_body[send_start_idx:reconnect_idx]
    assert "audio_service_.EnableVoiceProcessing(false);" in listening_body[send_start_idx:reconnect_idx]


def test_listening_watchdog_timeout_is_child_visible_before_idle():
    app_cc = read("main/application.cc")

    watchdog_start = app_cc.index("void Application::HandleListeningWatchdogTick")
    watchdog_end = app_cc.index("void Application::HandleStateChangedEvent", watchdog_start)
    watchdog_body = app_cc[watchdog_start:watchdog_end]

    assert "display->SetStatus(Lang::Strings::SERVER_TIMEOUT);" in watchdog_body
    assert 'display->SetEmotion("thinking");' in watchdog_body
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);" in watchdog_body
    assert watchdog_body.index("SetDeviceState(kDeviceStateIdle);") < watchdog_body.index(
        "display->SetStatus(Lang::Strings::SERVER_TIMEOUT);"
    )


def test_lesson_runtime_listening_watchdog_keeps_child_wait_cue():
    app_cc = read("main/application.cc")

    watchdog_start = app_cc.index("void Application::HandleListeningWatchdogTick")
    watchdog_end = app_cc.index("void Application::HandleStateChangedEvent", watchdog_start)
    watchdog_body = app_cc[watchdog_start:watchdog_end]

    assert "if (lesson_runtime_active_.load())" in watchdog_body
    assert watchdog_body.index("if (lesson_runtime_active_.load())") < watchdog_body.index(
        "display->SetStatus(Lang::Strings::SERVER_TIMEOUT);"
    )
    lesson_branch = watchdog_body[
        watchdog_body.index("if (lesson_runtime_active_.load())") :
        watchdog_body.index("display->SetStatus(Lang::Strings::SERVER_TIMEOUT);")
    ]
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_branch
    assert "display->SetEmotion(" not in lesson_branch
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);" not in lesson_branch

def test_listening_watchdog_clears_active_lesson_answer_turn():
    app_cc = read("main/application.cc")

    watchdog_start = app_cc.index("void Application::HandleListeningWatchdogTick")
    watchdog_end = app_cc.index("void Application::HandleStateChangedEvent", watchdog_start)
    watchdog_body = app_cc[watchdog_start:watchdog_end]

    assert "lesson_interactive_listening_active_.store(false);" in watchdog_body
    assert watchdog_body.index("lesson_interactive_listening_active_.store(false);") < watchdog_body.index(
        "SetDeviceState(kDeviceStateIdle);"
    )

def test_listening_watchdog_clears_pending_lesson_answer_turn_before_idle():
    app_cc = read("main/application.cc")

    watchdog_start = app_cc.index("void Application::HandleListeningWatchdogTick")
    watchdog_end = app_cc.index("void Application::HandleStateChangedEvent", watchdog_start)
    watchdog_body = app_cc[watchdog_start:watchdog_end]

    assert "lesson_interactive_listen_pending_.store(false);" in watchdog_body
    assert watchdog_body.index("lesson_interactive_listen_pending_.store(false);") < watchdog_body.index(
        "lesson_interactive_listening_active_.store(false);"
    )
    assert watchdog_body.index("lesson_interactive_listen_pending_.store(false);") < watchdog_body.index(
        "SetDeviceState(kDeviceStateIdle);"
    )

def test_listening_activity_is_refreshed_when_listening_starts_and_vad_fires():
    app_cc = read("main/application.cc")

    vad_start = app_cc.index("callbacks.on_vad_change")
    vad_end = app_cc.index("xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);", vad_start)
    vad_body = app_cc[vad_start:vad_end]
    assert "last_listening_activity_ms_.store" in vad_body
    assert "GetDeviceState() == kDeviceStateListening" in vad_body

    listening_start = app_cc.index("case kDeviceStateListening:")
    speaking_start = app_cc.index("case kDeviceStateSpeaking:", listening_start)
    listening_body = app_cc[listening_start:speaking_start]
    assert "listening_started_ms_.store" in listening_body
    assert "last_listening_activity_ms_.store" in listening_body


def test_cold_wake_word_open_uses_worker_instead_of_blocking_app_task():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    continue_start = app_cc.index("void Application::ContinueWakeWordInvoke")
    continue_end = app_cc.index("void Application::HandleStateChangedEvent", continue_start)
    continue_body = app_cc[continue_start:continue_end]

    # The wake phrase callback runs on the Application task. A cold WebSocket
    # open can block on TCP/TLS/server hello for seconds, so the cold path must
    # use the same worker/generation/watchdog machinery as button/reconnect open.
    assert "void FinishWakeWordInvoke(const std::string& wake_word);" in app_h
    assert "connect_in_flight_.load()" in continue_body
    assert "ArmConnectWatchdog();" in continue_body
    assert "StartOpenChannelWorker(ctx)" in continue_body
    assert "protocol_->OpenAudioChannel()" not in continue_body

    open_start = app_cc.index("void Application::OpenChannelTask")
    open_end = app_cc.index("void Application::ArmConnectWatchdog", open_start)
    open_body = app_cc[open_start:open_end]
    assert "ctx->wake_word" in open_body
    assert "kWakeWordAudioChannelOpenMaxAttempts" in open_body
    assert "FinishWakeWordInvoke(wake_word)" in open_body


def test_wake_word_open_worker_failure_cleans_context_and_rearms_detection():
    app_cc = read("main/application.cc")
    start = app_cc.index("void Application::ContinueWakeWordInvoke")
    end = app_cc.index("void Application::FinishWakeWordInvoke", start)
    body = app_cc[start:end]
    failure = body[body.index("if (!StartOpenChannelWorker(ctx))") :]

    assert "StartOpenChannelWorker(ctx)" in failure
    assert "delete ctx;" in failure
    assert "connect_in_flight_.store(false);" in failure
    assert "connect_attempt_active_.store(false);" in failure
    assert "CancelConnectWatchdog();" in failure
    assert 'ESP_LOGE(TAG, "wake_ws_open worker unavailable -> idle");' in failure
    assert "audio_service_.EnableWakeWordDetection(true);" in failure
    assert "SetDeviceState(kDeviceStateIdle);" in failure


def test_wake_word_connect_watchdog_allows_slow_websocket_open_retries():
    app_cc = read("main/application.cc")

    # A wake-triggered cold WebSocket open can spend time in TCP/TLS connect plus
    # the 10s server-hello wait. The watchdog must outlive the wake retry budget,
    # otherwise the first valid "Hi ESP" gets reset to Idle before the worker can
    # report success.
    assert "kConnectWatchdogTimeoutUs" in app_cc
    assert "35ULL * 1000000ULL" in app_cc
    assert "esp_timer_start_once(connect_watchdog_timer_, kConnectWatchdogTimeoutUs)" in app_cc
    assert "esp_timer_start_once(connect_watchdog_timer_, 12000000)" not in app_cc


def test_lesson_runtime_connect_watchdog_suppresses_generic_reconnect_and_idle_repaint():
    app_cc = read("main/application.cc")

    watchdog_start = app_cc.index("void Application::HandleConnectWatchdog")
    schedule_start = app_cc.index("void Application::ScheduleReconnect", watchdog_start)
    watchdog_body = app_cc[watchdog_start:schedule_start]

    assert "if (lesson_runtime_active_.load())" in watchdog_body
    passive_branch = watchdog_body[
        watchdog_body.index("if (passive_ws_intent_.load())") :
        watchdog_body.index("if (lesson_runtime_active_.load())")
    ]
    assert "SchedulePassiveLessonReconnect();" in passive_branch

    lesson_start = watchdog_body.index("if (lesson_runtime_active_.load())")
    assert lesson_start < watchdog_body.index("SetDeviceState(kDeviceStateIdle);", lesson_start)
    assert lesson_start < watchdog_body.index(
        "ScheduleReconnect(reconnect_mode_, reconnect_resume_listening_.load());",
        lesson_start,
    )
    lesson_branch = watchdog_body[
        lesson_start :
        watchdog_body.index('ESP_LOGW(TAG, "connect_watchdog_timeout -> idle + backoff"', lesson_start)
    ]
    assert "lesson connect watchdog timeout -> suppress generic reconnect" in lesson_branch
    assert "lesson_idle_repaint_suppressed_.store(true);" in lesson_branch
    assert "online_intent_.store(false);" in lesson_branch
    assert "connect_attempt_active_.store(false);" in lesson_branch
    assert "SetDeviceState(kDeviceStateIdle);" in lesson_branch
    assert "return;" in lesson_branch
    assert "ScheduleReconnect" not in lesson_branch


def test_reconnect_backoff_rolls_into_slow_periodic_retry_without_terminal_giveup():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    schedule_start = app_cc.index("void Application::ScheduleReconnect")
    schedule_end = app_cc.index("void Application::SchedulePassiveLessonReconnect", schedule_start)
    schedule_body = app_cc[schedule_start:schedule_end]

    assert "kFastReconnectAttempts" in schedule_body
    assert "kSlowReconnectRetryMs" in schedule_body
    assert "reconnect_slow_retry_scheduled" in schedule_body
    assert "reconnect_giveup" not in schedule_body
    assert "xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR)" not in schedule_body
    assert "connect_attempt_active_.store(false)" not in schedule_body

    tick_start = app_cc.index("void Application::HandleReconnectTick")
    tick_end = app_cc.index("void Application::HandleStartListeningEvent", tick_start)
    tick_body = app_cc[tick_start:tick_end]
    assert tick_body.index("SetDeviceState(kDeviceStateConnecting);") < tick_body.index(
        "ContinueOpenAudioChannel(reconnect_mode_);"
    )

    assert "long-horizon auto-reconnect" in app_h


def test_unexpected_online_websocket_close_is_child_visible_before_reconnect():
    app_cc = read("main/application.cc")

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]
    online_branch = closed_body[closed_body.index("if (online_intent_.load())") :]

    assert "audio_service_.ResetDecoder();" in online_branch
    assert "backend_offline_.store(true);" in online_branch
    assert "display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);" in online_branch
    assert 'display->SetEmotion("thinking");' in online_branch
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);" in online_branch
    assert online_branch.index("audio_service_.ResetDecoder();") < online_branch.index(
        "ScheduleReconnect(GetDefaultListeningMode(), false);"
    )

    idle_start = app_cc.index("case kDeviceStateIdle:")
    connecting_start = app_cc.index("case kDeviceStateConnecting:", idle_start)
    idle_body = app_cc[idle_start:connecting_start]
    assert 'backend_offline_.load() ? "thinking" : "neutral"' in idle_body

def test_unexpected_online_websocket_reconnect_does_not_resume_stale_listening():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    assert "reconnect_resume_listening_" in app_h
    assert "void ScheduleReconnect(ListeningMode mode, bool resume_listening = true);" in app_h

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]
    online_branch = closed_body[closed_body.index("if (online_intent_.load())") :]
    assert "ScheduleReconnect(GetDefaultListeningMode(), false);" in online_branch

    task_start = app_cc.index("void Application::OpenChannelTask")
    task_end = app_cc.index("void Application::ArmConnectWatchdog", task_start)
    task_body = app_cc[task_start:task_end]
    generic_success = task_body[task_body.index("} else {", task_body.index("wake_word_invoke")) :]
    assert "self->reconnect_resume_listening_.exchange(true)" in generic_success
    no_resume = generic_success[generic_success.index("else {", generic_success.index("reconnect_resume_listening_")) :]
    assert "self->SetDeviceState(kDeviceStateIdle);" in no_resume
    assert no_resume.index("self->SetDeviceState(kDeviceStateIdle);") < no_resume.index("}")

def test_lesson_runtime_audio_channel_close_preserves_lesson_chat():
    app_cc = read("main/application.cc")

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]

    assert "if (!lesson_runtime_active_.load())" in closed_body
    assert closed_body.index("if (!lesson_runtime_active_.load())") < closed_body.index(
        'display->SetChatMessage("system", "");'
    )


def test_lesson_runtime_passive_socket_close_reconnects_before_idle_repaint():
    app_cc = read("main/application.cc")

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]

    set_idle = closed_body.index("SetDeviceState(kDeviceStateIdle);")
    early_body = closed_body[:set_idle]

    assert "lesson passive ws dropped -> passive reconnect" in early_body
    assert "lesson_runtime_active_.load() && passive_ws_intent_.load()" in early_body
    assert early_body.index("lesson_runtime_active_.load() && passive_ws_intent_.load()") < set_idle
    guard = early_body[early_body.index("lesson_runtime_active_.load() && passive_ws_intent_.load()") :]
    assert "while (audio_service_.PopPacketFromSendQueue() != nullptr) {}" in guard
    assert "SchedulePassiveLessonReconnect();" in guard
    assert "StartPassiveLessonWebsocket();" not in guard
    assert "return;" in guard


def test_audio_channel_close_during_wake_reconnect_ignores_stale_close_before_idle_repaint():
    app_cc = read("main/application.cc")

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]

    set_idle = closed_body.index("SetDeviceState(kDeviceStateIdle);")
    early_body = closed_body[:set_idle]

    assert "connect_in_flight_.load()" in early_body
    guard = early_body[early_body.index("connect_in_flight_.load()") :]
    assert "ws_close_ignored_during_connect" in guard
    assert "return;" in guard


def test_lesson_runtime_suppresses_generic_reconnect_after_close():
    app_cc = read("main/application.cc")

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]
    online_branch = closed_body[closed_body.index("if (online_intent_.load())") :]

    assert "lesson_runtime_active_.load()" in online_branch
    assert "lesson ws dropped unexpected -> suppress generic reconnect" in online_branch
    assert online_branch.index("lesson_runtime_active_.load()") < online_branch.index(
        "ScheduleReconnect(GetDefaultListeningMode(), false);"
    )
    lesson_branch = online_branch[
        online_branch.index("lesson_runtime_active_.load()") :
        online_branch.index("ESP_LOGW(TAG, \"ws_dropped_unexpected -> auto-reconnect")
    ]
    assert "online_intent_.store(false);" in lesson_branch
    assert "backend_offline_.store(true);" in lesson_branch
    assert "audio_service_.ResetDecoder();" in lesson_branch
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_branch
    assert "ScheduleReconnect(GetDefaultListeningMode(), false);" not in lesson_branch

    tick_start = app_cc.index("void Application::HandleReconnectTick")
    tick_end = app_cc.index("void Application::HandleStartListeningEvent", tick_start)
    tick_body = app_cc[tick_start:tick_end]

    assert "lesson_runtime_active_.load()" in tick_body
    assert "lesson reconnect ignored" in tick_body
    assert tick_body.index("lesson_runtime_active_.load()") < tick_body.index(
        "SetDeviceState(kDeviceStateConnecting);"
    )


def test_lesson_runtime_audio_channel_close_clears_stale_answer_turn_flags():
    app_cc = read("main/application.cc")

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]
    online_branch = closed_body[closed_body.index("if (online_intent_.load())") :]
    lesson_branch = online_branch[
        online_branch.index("lesson_runtime_active_.load()") :
        online_branch.index("ESP_LOGW(TAG, \"ws_dropped_unexpected -> auto-reconnect")
    ]

    assert "lesson_interactive_listen_pending_.store(false);" in lesson_branch
    assert "lesson_interactive_listening_active_.store(false);" in lesson_branch
    assert lesson_branch.index("lesson_interactive_listen_pending_.store(false);") < lesson_branch.index(
        "display->SetStatus(Lang::Strings::PLEASE_WAIT);"
    )
    assert lesson_branch.index("lesson_interactive_listening_active_.store(false);") < lesson_branch.index(
        "display->SetStatus(Lang::Strings::PLEASE_WAIT);"
    )


def test_backend_offline_connecting_state_keeps_reconnect_face():
    app_cc = read("main/application.cc")

    connecting_start = app_cc.index("case kDeviceStateConnecting:")
    listening_start = app_cc.index("case kDeviceStateListening:", connecting_start)
    connecting_body = app_cc[connecting_start:listening_start]

    assert 'backend_offline_.load() ? "thinking" : "neutral"' in connecting_body


def test_lesson_runtime_connecting_state_keeps_child_wait_cue():
    app_cc = read("main/application.cc")

    connecting_start = app_cc.index("case kDeviceStateConnecting:")
    listening_start = app_cc.index("case kDeviceStateListening:", connecting_start)
    connecting_body = app_cc[connecting_start:listening_start]

    assert "if (lesson_runtime_active_.load())" in connecting_body
    assert connecting_body.index("if (lesson_runtime_active_.load())") < connecting_body.index(
        "display->SetStatus(connect_copy);"
    )

    lesson_connecting = connecting_body[
        connecting_body.index("if (lesson_runtime_active_.load())") :
        connecting_body.index("// BACKEND_CONNECTING")
    ]
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_connecting
    assert "display->SetEmotion(" not in lesson_connecting
    assert 'display->SetChatMessage("system", "");' not in lesson_connecting

def test_active_network_disconnect_is_child_visible_and_stops_stale_audio():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleNetworkDisconnectedEvent()")
    end = app_cc.index("void Application::HandleActivationDoneEvent()", start)
    body = app_cc[start:end]
    active_branch = body[body.index("if (state == kDeviceStateConnecting") :]

    assert "backend_offline_.store(true);" in active_branch
    assert "audio_service_.ResetDecoder();" in active_branch
    assert "CloseAudioChannelByIntent();" in active_branch
    assert "display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);" in active_branch
    assert 'display->SetEmotion("thinking");' in active_branch
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);" in active_branch
    assert active_branch.index("audio_service_.ResetDecoder();") < active_branch.index(
        "CloseAudioChannelByIntent();"
    )

def test_lesson_runtime_network_disconnect_keeps_child_wait_cue():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleNetworkDisconnectedEvent()")
    end = app_cc.index("void Application::HandleActivationDoneEvent()", start)
    body = app_cc[start:end]
    active_branch = body[body.index("if (state == kDeviceStateConnecting") :]

    assert "if (lesson_runtime_active_.load())" in active_branch
    assert active_branch.index("if (lesson_runtime_active_.load())") < active_branch.index(
        "display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);"
    )
    lesson_branch = active_branch[
        active_branch.index("if (lesson_runtime_active_.load())") :
        active_branch.index("display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);")
    ]
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_branch
    assert "display->SetEmotion(" not in lesson_branch
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);" not in lesson_branch

def test_lesson_runtime_network_disconnect_clears_stale_answer_turn_flags():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleNetworkDisconnectedEvent()")
    end = app_cc.index("void Application::HandleActivationDoneEvent()", start)
    body = app_cc[start:end]
    active_branch = body[body.index("if (state == kDeviceStateConnecting") :]
    lesson_branch = active_branch[
        active_branch.index("if (lesson_runtime_active_.load())") :
        active_branch.index("} else {", active_branch.index("if (lesson_runtime_active_.load())"))
    ]

    assert "lesson_interactive_listen_pending_.store(false);" in lesson_branch
    assert "lesson_interactive_listening_active_.store(false);" in lesson_branch
    assert lesson_branch.index("lesson_interactive_listen_pending_.store(false);") < lesson_branch.index(
        "display->SetStatus(Lang::Strings::PLEASE_WAIT);"
    )
    assert lesson_branch.index("lesson_interactive_listening_active_.store(false);") < lesson_branch.index(
        "display->SetStatus(Lang::Strings::PLEASE_WAIT);"
    )

def test_lesson_runtime_connect_watchdog_clears_answer_turn_before_idle():
    app_cc = read("main/application.cc")

    watchdog_start = app_cc.index("void Application::HandleConnectWatchdog")
    watchdog_end = app_cc.index("void Application::ScheduleReconnect", watchdog_start)
    watchdog_body = app_cc[watchdog_start:watchdog_end]

    lesson_start = watchdog_body.index("if (lesson_runtime_active_.load())")
    lesson_branch = watchdog_body[lesson_start: watchdog_body.index("return;", lesson_start)]

    assert "lesson_interactive_listen_generation_.fetch_add(1);" in lesson_branch
    assert "lesson_interactive_listen_pending_.store(false);" in lesson_branch
    assert "lesson_interactive_listening_active_.store(false);" in lesson_branch
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_branch
    assert "display->SetEmotion(" not in lesson_branch
    assert lesson_branch.index("lesson_interactive_listen_generation_.fetch_add(1);") < lesson_branch.index(
        "lesson_interactive_listen_pending_.store(false);"
    )
    assert lesson_branch.index("lesson_interactive_listen_pending_.store(false);") < lesson_branch.index(
        "SetDeviceState(kDeviceStateIdle);"
    )

def test_lesson_runtime_network_event_callback_suppresses_generic_ui_but_keeps_events():
    app_cc = read("main/application.cc")

    start = app_cc.index("board.SetNetworkEventCallback")
    end = app_cc.index("    // Start network asynchronously", start)
    body = app_cc[start:end]

    assert "const bool lesson_active = lesson_runtime_active_.load();" in body

    scanning = body[
        body.index("case NetworkEvent::Scanning:") :
        body.index("case NetworkEvent::Connecting:")
    ]
    assert scanning.index("if (!lesson_active)") < scanning.index("display->ShowNotification")
    assert scanning.index("if (!lesson_active)") < scanning.index("xEventGroupSetBits")

    connecting = body[
        body.index("case NetworkEvent::Connecting:") :
        body.index("case NetworkEvent::Connected:")
    ]
    assert connecting.index("if (lesson_active)") < connecting.index("display->SetStatus")
    assert connecting.index("if (lesson_active)") < connecting.index("display->ShowNotification")
    assert "break;" in connecting[connecting.index("if (lesson_active)") : connecting.index("if (data.empty())")]

    connected = body[
        body.index("case NetworkEvent::Connected:") :
        body.index("case NetworkEvent::Disconnected:")
    ]
    assert connected.index("if (!lesson_active)") < connected.index("display->ShowNotification")
    assert connected.index("xEventGroupSetBits") > connected.index("display->ShowNotification")

    modem_detecting = body[
        body.index("case NetworkEvent::ModemDetecting:") :
        body.index("case NetworkEvent::ModemErrorNoSim:")
    ]
    assert modem_detecting.index("if (!lesson_active)") < modem_detecting.index("display->SetStatus")

    modem_timeout = body[
        body.index("case NetworkEvent::ModemErrorTimeout:") :
        body.index("        }", body.index("case NetworkEvent::ModemErrorTimeout:"))
    ]
    assert modem_timeout.index("if (!lesson_active)") < modem_timeout.index("display->SetStatus")

def test_lesson_runtime_main_error_keeps_child_wait_cue():
    app_cc = read("main/application.cc")

    error_start = app_cc.index("if (bits & MAIN_EVENT_ERROR)")
    error_end = app_cc.index("if (bits & MAIN_EVENT_NETWORK_CONNECTED)", error_start)
    error_body = app_cc[error_start:error_end]

    assert "lesson_runtime_active_.load()" in error_body
    assert error_body.index("lesson_runtime_active_.load()") < error_body.index(
        "connect_attempt_active_.load()"
    )
    assert error_body.index("lesson_runtime_active_.load()") < error_body.index(
        "Alert(status, last_error_message_.c_str()"
    )

    lesson_branch = error_body[
        error_body.index("if (lesson_runtime_active_.load())") :
        error_body.index("} else if (connect_attempt_active_.load()")
    ]
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_branch
    assert "display->SetEmotion(" not in lesson_branch
    assert "Alert(" not in lesson_branch
    assert "OGG_EXCLAMATION" not in lesson_branch

def test_lesson_runtime_main_error_clears_stale_answer_turn_flags():
    app_cc = read("main/application.cc")

    error_start = app_cc.index("if (bits & MAIN_EVENT_ERROR)")
    error_end = app_cc.index("if (bits & MAIN_EVENT_NETWORK_CONNECTED)", error_start)
    error_body = app_cc[error_start:error_end]
    lesson_branch = error_body[
        error_body.index("if (lesson_runtime_active_.load())") :
        error_body.index("} else if (connect_attempt_active_.load()")
    ]

    assert "lesson_interactive_listen_pending_.store(false);" in lesson_branch
    assert "lesson_interactive_listening_active_.store(false);" in lesson_branch
    assert lesson_branch.index("lesson_interactive_listen_pending_.store(false);") < lesson_branch.index(
        "display->SetStatus(Lang::Strings::PLEASE_WAIT);"
    )
    assert lesson_branch.index("lesson_interactive_listening_active_.store(false);") < lesson_branch.index(
        "display->SetStatus(Lang::Strings::PLEASE_WAIT);"
    )

def test_lesson_runtime_failure_boundaries_invalidate_stale_answer_turn_generation():
    app_cc = read("main/application.cc")

    error_start = app_cc.index("if (bits & MAIN_EVENT_ERROR)")
    error_end = app_cc.index("if (bits & MAIN_EVENT_NETWORK_CONNECTED)", error_start)
    error_body = app_cc[error_start:error_end]
    error_lesson_branch = error_body[
        error_body.index("if (lesson_runtime_active_.load())") :
        error_body.index("} else if (connect_attempt_active_.load()")
    ]

    disconnect_start = app_cc.index("void Application::HandleNetworkDisconnectedEvent()")
    disconnect_end = app_cc.index("void Application::HandleActivationDoneEvent()", disconnect_start)
    disconnect_body = app_cc[disconnect_start:disconnect_end]
    active_branch = disconnect_body[disconnect_body.index("if (state == kDeviceStateConnecting") :]
    disconnect_lesson_branch = active_branch[
        active_branch.index("if (lesson_runtime_active_.load())") :
        active_branch.index("} else {", active_branch.index("if (lesson_runtime_active_.load())"))
    ]

    closed_start = app_cc.index("protocol_->OnAudioChannelClosed")
    incoming_json = app_cc.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app_cc[closed_start:incoming_json]
    online_branch = closed_body[closed_body.index("if (online_intent_.load())") :]
    closed_lesson_branch = online_branch[
        online_branch.index("lesson_runtime_active_.load()") :
        online_branch.index("ESP_LOGW(TAG, \"ws_dropped_unexpected -> auto-reconnect")
    ]

    for branch in (error_lesson_branch, disconnect_lesson_branch, closed_lesson_branch):
        assert "lesson_interactive_listen_generation_.fetch_add(1);" in branch
        assert branch.index("lesson_interactive_listen_generation_.fetch_add(1);") < branch.index(
            "lesson_interactive_listen_pending_.store(false);"
        )
        assert branch.index("lesson_interactive_listen_generation_.fetch_add(1);") < branch.index(
            "display->SetStatus(Lang::Strings::PLEASE_WAIT);"
        )

def test_reconnect_slow_retry_log_is_distinguishable_and_attempt_capped():
    app_cc = read("main/application.cc")

    schedule_start = app_cc.index("void Application::ScheduleReconnect")
    schedule_end = app_cc.index("void Application::SchedulePassiveLessonReconnect", schedule_start)
    schedule_body = app_cc[schedule_start:schedule_end]
    slow_branch = schedule_body[schedule_body.index("} else {") :]

    assert "phase=slow" in slow_branch
    assert "kSlowReconnectRetryMs + jitter_ms" in slow_branch
    assert "reconnect_attempt_ = kFastReconnectAttempts;" in slow_branch
    assert "reconnect_attempt_++" not in slow_branch


def test_reconnect_suppression_flag_clears_when_retry_chain_is_abandoned():
    app_cc = read("main/application.cc")

    tick_start = app_cc.index("void Application::HandleReconnectTick")
    tick_end = app_cc.index("void Application::HandleStartListeningEvent", tick_start)
    tick_body = app_cc[tick_start:tick_end]

    no_protocol = tick_body[tick_body.index("if (protocol_ == nullptr)") : tick_body.index("if (reconnect_passive_")]
    assert "connect_attempt_active_.store(false);" in no_protocol

    user_moved_on = tick_body[tick_body.index("if (GetDeviceState() != kDeviceStateIdle)") :]
    user_moved_on = user_moved_on[: user_moved_on.index("if (protocol_->IsAudioChannelOpened())")]
    assert "connect_attempt_active_.store(false);" in user_moved_on

    close_start = app_cc.index("void Application::CloseAudioChannelByIntent")
    close_end = app_cc.index("void Application::ResetProtocol", close_start)
    close_body = app_cc[close_start:close_end]
    assert close_body.index("connect_attempt_active_.store(false);") < close_body.index("protocol_->CloseAudioChannel();")


def test_wake_word_open_finishes_after_stale_passive_socket_close_returns_idle():
    app_cc = read("main/application.cc")

    open_start = app_cc.index("void Application::OpenChannelTask")
    open_end = app_cc.index("void Application::ArmConnectWatchdog", open_start)
    open_body = app_cc[open_start:open_end]
    callback_start = open_body.index("self->Schedule")
    state_guard = open_body[
        open_body.index("if (!passive_preconnect", callback_start) :
        open_body.index("if (ok)", callback_start)
    ]

    # A timed-out passive WebSocket can emit OnAudioChannelClosed after the wake
    # path already moved Idle -> Connecting. That stale close returns the FSM to
    # Idle while the wake worker is still opening a fresh channel. The wake worker
    # must still finish the original wake turn from Idle, not discard it and make
    # the user say "Hi ESP" again.
    assert "wake_word_invoke" in state_guard
    assert "kDeviceStateIdle" in state_guard
    assert "FinishWakeWordInvoke(wake_word)" in open_body


def test_afe_audio_loops_yield_to_avoid_watchdog_starvation():
    audio_service = read("main/audio/audio_service.cc")
    wake_word = read("main/audio/wake_words/afe_wake_word.cc")
    processor = read("main/audio/processors/afe_audio_processor.cc")

    feed_start = audio_service.index("/* Feed the wake word and/or audio processor */")
    feed_end = audio_service.index("// Read timeout/error should not terminate", feed_start)
    feed_body = audio_service[feed_start:feed_end]
    assert "vTaskDelay(pdMS_TO_TICKS(1));" in feed_body

    wake_start = wake_word.index("void AfeWakeWord::AudioDetectionTask()")
    wake_end = wake_word.index("void AfeWakeWord::StoreWakeWordData", wake_start)
    assert "vTaskDelay(pdMS_TO_TICKS(1));" in wake_word[wake_start:wake_end]

    processor_start = processor.index("void AfeAudioProcessor::AudioProcessorTask()")
    processor_end = processor.index("void AfeAudioProcessor::EnableDeviceAec", processor_start)
    assert "vTaskDelay(pdMS_TO_TICKS(1));" in processor[processor_start:processor_end]


def test_afe_background_tasks_keep_fetch_below_feed_but_above_idle():
    wake_word = read("main/audio/wake_words/afe_wake_word.cc")
    processor = read("main/audio/processors/afe_audio_processor.cc")
    audio_service = read("main/audio/audio_service.cc")

    wake_task = wake_word[wake_word.index('"audio_detection"') - 180:wake_word.index('"audio_detection"') + 120]
    processor_task = processor[processor.index('"audio_communication"') - 180:processor.index('"audio_communication"') + 120]
    input_task = audio_service[audio_service.index('"audio_input"') - 180:audio_service.index('"audio_input"') + 120]

    assert '"audio_detection", 4096, this, tskIDLE_PRIORITY + 1' in wake_task
    assert '"audio_detection", 4096, this, tskIDLE_PRIORITY,' not in wake_task
    assert '"audio_input", 2048 * 5, this, 8' in input_task
    assert '"audio_communication", 4096, this, tskIDLE_PRIORITY + 9' in processor_task

def test_main_loop_bounds_audio_send_work_and_feeds_watchdog_between_packets():
    app_cc = read("main/application.cc")

    assert "kMaxAudioPacketsPerMainLoop" in app_cc
    send_start = app_cc.index("if (bits & MAIN_EVENT_SEND_AUDIO)")
    send_end = app_cc.index("if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)", send_start)
    send_body = app_cc[send_start:send_end]

    assert "sent_packets < kMaxAudioPacketsPerMainLoop" in send_body
    assert "esp_task_wdt_reset();" in send_body
    assert "vTaskDelay(pdMS_TO_TICKS(1));" in send_body
    assert "xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);" in send_body

def test_main_loop_runs_scheduled_state_changes_before_audio_uplink_batch():
    app_cc = read("main/application.cc")
    loop_start = app_cc.index("while (true) {")
    send_start = app_cc.index("if (bits & MAIN_EVENT_SEND_AUDIO)", loop_start)
    schedule_start = app_cc.index("if (bits & MAIN_EVENT_SCHEDULE)", loop_start)

    assert schedule_start < send_start


def test_main_loop_reruns_scheduled_state_changes_after_audio_uplink_batch():
    app_cc = read("main/application.cc")
    loop_start = app_cc.index("while (true) {")
    send_start = app_cc.index("if (bits & MAIN_EVENT_SEND_AUDIO)", loop_start)
    wake_start = app_cc.index("if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)", send_start)
    send_body = app_cc[send_start:wake_start]

    assert send_body.count("RunScheduledTasks();") >= 1
    assert send_body.rindex("RunScheduledTasks();") > send_body.index("vTaskDelay(pdMS_TO_TICKS(1));")


def test_audio_uplink_pipeline_has_send_boundary_diagnostics():
    app_cc = read("main/application.cc")
    audio_cc = read("main/audio/audio_service.cc")
    ws_cc = read("main/protocols/websocket_protocol.cc")

    assert "audio_uplink_packet_queued" in audio_cc
    assert "MAIN_EVENT_SEND_AUDIO protocol_unavailable" in app_cc
    assert "MAIN_EVENT_SEND_AUDIO packet" in app_cc
    assert "Websocket SendAudio" in ws_cc

def test_lesson_image_render_quiets_authorized_audio_without_dropping_packets():
    app_h = read("main/application.h")
    app_cc = read("main/application.cc")
    lesson_cc = read("main/lesson_handler.cc")

    assert "BeginLessonNetworkRenderQuiet" in app_h
    assert "EndLessonNetworkRenderQuiet" in app_h
    assert "IsLessonNetworkRenderQuiet" in app_h
    assert "lesson_network_render_quiet_" in app_h

    send_start = app_cc.index("if (bits & MAIN_EVENT_SEND_AUDIO)")
    send_end = app_cc.index("if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)", send_start)
    send_body = app_cc[send_start:send_end]
    authorization_idx = send_body.index("if (!IsMicrophoneUplinkAuthorized())")
    quiet_idx = send_body.index("IsLessonNetworkRenderQuiet()")
    authorized_pop_idx = send_body.index("audio_service_.PopPacketFromSendQueue()", quiet_idx)
    assert authorization_idx < quiet_idx < authorized_pop_idx
    assert "MAIN_EVENT_SEND_AUDIO deferred_for_lesson_render" in send_body
    assert "xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);" in send_body
    assert "vTaskDelay(pdMS_TO_TICKS(20));" in send_body

    step_start = lesson_cc.index('const cJSON* scene = Obj(body, "scene")')
    step_end = lesson_cc.index('ESP_LOGI(TAG, "lesson_step rendered', step_start)
    step_body = lesson_cc[step_start:step_end]
    begin_idx = step_body.index("BeginLessonNetworkRenderQuiet()")
    fetch_idx = step_body.index("FetchLessonImage(poster_src)")
    ack_idx = step_body.index("emit_ack(root, sequence")
    assert begin_idx < fetch_idx < ack_idx
    assert "EndLessonNetworkRenderQuiet()" in step_body

def test_firmware_exposes_sample_lesson_asset_sync_to_sd():
    mcp_server = read("main/mcp_server.cc")

    assert "self.lesson_assets.sync_sample_to_sd" in mcp_server
    assert "/sdcard/tbot/lesson-assets/sample-barn" in mcp_server
    assert "barn-round-field-poster.jpg" in mcp_server
    assert "bright-listening.png" in mcp_server
    assert "downloadedCount" in mcp_server

def test_firmware_exposes_generic_lesson_asset_pack_sync_to_sd():
    mcp_server = read("main/mcp_server.cc")

    assert "self.lesson_assets.sync_to_sd" in mcp_server
    assert 'Property("assetPack", kPropertyTypeObject' in mcp_server
    assert '"localPath"' in mcp_server
    assert '"url"' in mcp_server
    assert '"sha256"' in mcp_server
    assert "VerifyLessonAssetSha256" in mcp_server
    assert "ValidateLessonAssetSyncPath" in mcp_server
    assert "ValidateLessonAssetSyncPackOrThrow" in mcp_server
    assert '"downloadedCount"' in mcp_server
    assert '"skippedCount"' in mcp_server
    assert '"failedCount"' in mcp_server

def test_lesson_asset_pack_sync_http_download_does_not_starve_main_watchdog():
    mcp_server = read("main/mcp_server.cc")
    declaration = mcp_server.index("bool DownloadLessonAssetToFile")
    start = mcp_server.index("bool DownloadLessonAssetToFile", declaration + 1)
    end = mcp_server.index("\n    return true;\n}", start)
    body = mcp_server[start:end]

    assert "auto http = network->CreateHttp(1);" in body
    assert "http->SetTimeout(6000);" in body
    assert "http->SetTimeout(10000);" not in body
    assert "http->SetTimeout(4000);" not in body
    assert "http->GetStatusCode()" not in body
    transfer = read("main/lesson_asset_http_transfer.cc")
    assert "ResetCurrentTaskWatchdogIfSubscribed();" in transfer
    assert transfer.index("ResetCurrentTaskWatchdogIfSubscribed();") < transfer.index(
        'http.Open("GET", url)'
    )

def test_lesson_asset_pack_sync_reuses_verified_duplicate_bytes_before_network_download():
    mcp_server = read("main/mcp_server.cc")
    start = mcp_server.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
    end = mcp_server.index('AddUserOnlyTool("self.assets.set_download_url"', start)
    body = mcp_server[start:end]

    assert "verified_asset_files" in body
    assert "CopyVerifiedLessonAssetFile" in body
    assert body.index("CopyVerifiedLessonAssetFile") < body.index(
        "DownloadLessonAssetToVerifiedFile"
    )
    reuse_start = body.index("if (reusable != nullptr)")
    reuse_end = body.index("} else {", reuse_start)
    reuse_body = body[reuse_start:reuse_end]
    assert "reused += 1;" in reuse_body
    assert "skipped += 1;" not in reuse_body
    assert '"reusedCount"' in body

def test_sample_lesson_asset_sync_does_not_mkdir_sd_mount_point():
    mcp_server = read("main/mcp_server.cc")

    ensure_start = mcp_server.index("void EnsureSampleLessonAssetDir(")
    ensure_end = mcp_server.index("bool DownloadLessonAssetToFile", ensure_start)
    ensure_body = mcp_server[ensure_start:ensure_end]

    assert 'EnsureDirOrThrow(mutation, "/sdcard/tbot")' in ensure_body
    assert 'EnsureDirOrThrow(mutation, "/sdcard/tbot/lesson-assets")' in ensure_body
    assert 'DirectoryExists("/sdcard")' not in ensure_body
    assert 'EnsureDir("/sdcard")' not in ensure_body
    assert "lesson asset storage write failed" in mcp_server
    assert "if (!EnsureSampleLessonAssetDir())" not in mcp_server
    assert "EnsureSampleLessonAssetDir(mutation);" in mcp_server

def test_start_listening_rearms_when_already_listening():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    marker = "state == kDeviceStateListening"
    assert marker in body
    listening = body[body.index(marker) :]

    assert "lesson/manual listening rearm" in listening
    assert "listening_mode_ = kListeningModeManualStop;" in listening
    assert "protocol_->SendStartListening(kListeningModeManualStop);" in listening
    assert "audio_service_.EnableVoiceProcessing(true);" in listening


def test_lesson_prompt_tts_stop_rearms_interactive_listening_instead_of_idling():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")
    lesson_handler = read("main/lesson_handler.cc")

    assert "lesson_interactive_listen_pending_" in app_h
    assert "PrepareLessonInteractiveListening" in lesson_handler

    stop_start = app_cc.index('} else if (strcmp(state->valuestring, "stop") == 0)')
    stop_end = app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)', stop_start)
    stop_body = app_cc[stop_start:stop_end]
    manual_body = stop_body[stop_body.index("listening_mode_ == kListeningModeManualStop") :]

    assert "if (lesson_interactive_turn)" in manual_body
    lesson_turn_body = manual_body[
        manual_body.index("if (lesson_interactive_turn)") :
        manual_body.index("} else {", manual_body.index("if (lesson_interactive_turn)"))
    ]
    assert "audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs)" in lesson_turn_body
    assert lesson_turn_body.index(
        "audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs)"
    ) < lesson_turn_body.index("SetDeviceState(kDeviceStateListening);")
    assert "lesson_interactive_listen_pending_.exchange(false)" not in manual_body
    assert "SetDeviceState(kDeviceStateListening);" in manual_body
    assert "protocol_->SendStartListening(kListeningModeManualStop);" not in manual_body
    assert "audio_service_.EnableVoiceProcessing(true);" not in manual_body
    assert "SetDeviceState(kDeviceStateIdle);" in manual_body
    assert manual_body.index("SetDeviceState(kDeviceStateListening);") < manual_body.index("SetDeviceState(kDeviceStateIdle);")
    assert 'display->SetStatus("Con nói nhé...");' not in manual_body
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);" not in manual_body


def test_lesson_prompt_start_listening_event_does_not_abort_speaking_prompt():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    speaking = body[body.index("state == kDeviceStateSpeaking") : body.index("state == kDeviceStateListening")]

    assert "lesson_interactive_listen_pending_.load()" in speaking
    assert "lesson prompt still speaking; defer listening" in speaking
    assert speaking.index("lesson_interactive_listen_pending_.load()") < speaking.index("AbortSpeaking(kAbortReasonNone);")


def test_lesson_prompt_start_listening_defer_shows_waiting_turn_cue():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    speaking = body[body.index("state == kDeviceStateSpeaking") : body.index("state == kDeviceStateListening")]

    assert "lesson_interactive_listen_pending_.load()" in speaking
    assert "display->ClearChatMessages();" in speaking
    assert 'display->SetStatus("Sắp đến lượt con...");' in speaking
    assert 'display->SetEmotion("thinking");' not in speaking
    assert speaking.index("display->ClearChatMessages();") < speaking.index(
        'display->SetStatus("Sắp đến lượt con...");'
    )
    assert speaking.index('display->SetStatus("Sắp đến lượt con...");') < speaking.index("return;")
    assert "protocol_->SendStartListening" not in speaking
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);" not in speaking

def test_lesson_start_listening_child_cues_guard_missing_display():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    speaking = body[body.index("state == kDeviceStateSpeaking") : body.index("state == kDeviceStateListening")]
    listening = body[body.index("state == kDeviceStateListening") :]

    speaking_display = speaking[speaking.index("auto display = Board::GetInstance().GetDisplay();") :]
    assert "if (display)" in speaking_display[: speaking_display.index("return;")]
    assert speaking_display.index("if (display)") < speaking_display.index("display->ClearChatMessages();")

    listening_display = listening[listening.index("auto display = Board::GetInstance().GetDisplay();") :]
    assert "if (display)" in listening_display[: listening_display.index("audio_service_.PlaySound")]
    assert listening_display.index("if (display)") < listening_display.index("display->ClearChatMessages();")


def test_lesson_active_child_start_listening_duplicate_ignored_before_resend():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    listening = body[body.index("state == kDeviceStateListening") :]

    guard = "if (lesson_interactive_listening_active_.load() && !lesson_interactive_listen_pending_.load())"
    assert guard in listening
    guard_body = listening[
        listening.index(guard) :
        listening.index("if (lesson_interactive_listen_pending_.exchange(false))")
    ]
    assert "return;" in guard_body
    assert "protocol_->SendStartListening" not in guard_body
    assert guard_body.index("return;") < listening.index("protocol_->SendStartListening")


def test_lesson_runtime_start_listening_ignores_non_answer_press():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]

    assert "const bool lesson_answer_turn =" in body
    assert "lesson_interactive_listen_pending_.load()" in body
    assert "lesson_interactive_listening_active_.load()" in body
    assert "if (lesson_runtime_active_.load() && !lesson_answer_turn)" in body
    assert "lesson start listening ignored" in body
    assert body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < body.index(
        "if (state == kDeviceStateIdle)"
    )
    assert body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < body.index(
        "AbortSpeaking(kAbortReasonNone);"
    )

def test_lesson_runtime_stop_listening_ignores_non_answer_release():
    app_cc = read("main/application.cc")

    stop_start = app_cc.index("void Application::StopListening()")
    stop_end = app_cc.index("void Application::HandleToggleChatEvent", stop_start)
    stop_body = app_cc[stop_start:stop_end]

    assert "const bool lesson_answer_turn =" in stop_body
    assert "lesson_interactive_listen_pending_.load()" in stop_body
    assert "lesson_interactive_listening_active_.load()" in stop_body
    assert "if (lesson_runtime_active_.load() && !lesson_answer_turn)" in stop_body
    assert "lesson stop listening ignored" in stop_body
    assert stop_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < stop_body.index(
        "CancelLessonInteractiveListening();"
    )
    assert stop_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < stop_body.index(
        "xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);"
    )

    handle_start = app_cc.index("void Application::HandleStopListeningEvent")
    handle_end = app_cc.index("void Application::HandleWakeWordDetectedEvent", handle_start)
    handle_body = app_cc[handle_start:handle_end]

    assert "const bool lesson_answer_turn =" in handle_body
    assert "lesson_interactive_listen_pending_.load()" in handle_body
    assert "lesson_interactive_listening_active_.load()" in handle_body
    assert "if (lesson_runtime_active_.load() && !lesson_answer_turn)" in handle_body
    assert "lesson stop listening ignored" in handle_body
    assert handle_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < handle_body.index(
        "audio_service_.EnableAudioTesting(false);"
    )
    assert handle_body.index("if (lesson_runtime_active_.load() && !lesson_answer_turn)") < handle_body.index(
        "protocol_->SendStopListening();"
    )


def test_lesson_stop_listening_clears_pending_child_turn_before_idle():
    app_cc = read("main/application.cc")

    handle_start = app_cc.index("void Application::HandleStopListeningEvent")
    handle_end = app_cc.index("void Application::HandleWakeWordDetectedEvent", handle_start)
    handle_body = app_cc[handle_start:handle_end]
    listening = handle_body[handle_body.index("if (state == kDeviceStateListening)") :]

    assert "lesson_interactive_listen_pending_.store(false);" in listening
    assert listening.index("lesson_interactive_listen_pending_.store(false);") < listening.index(
        "lesson_interactive_listening_active_.store(false);"
    )
    assert listening.index("lesson_interactive_listen_pending_.store(false);") < listening.index(
        "SetDeviceState(kDeviceStateIdle);"
    )

def test_lesson_stop_listening_invalidates_pending_child_turn_generation():
    app_cc = read("main/application.cc")

    handle_start = app_cc.index("void Application::HandleStopListeningEvent")
    handle_end = app_cc.index("void Application::HandleWakeWordDetectedEvent", handle_start)
    handle_body = app_cc[handle_start:handle_end]
    listening = handle_body[handle_body.index("if (state == kDeviceStateListening)") :]

    assert "lesson_interactive_listen_generation_.fetch_add(1);" in listening
    assert listening.index("lesson_interactive_listen_generation_.fetch_add(1);") < listening.index(
        "lesson_interactive_listen_pending_.store(false);"
    )
    assert listening.index("lesson_interactive_listen_generation_.fetch_add(1);") < listening.index(
        "SetDeviceState(kDeviceStateIdle);"
    )


def test_stop_listening_release_preserves_pending_lesson_prompt_while_speaking():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::StopListening()")
    end = app_cc.index("void Application::HandleToggleChatEvent", start)
    body = app_cc[start:end]

    assert "GetDeviceState() == kDeviceStateSpeaking" in body
    assert "lesson_interactive_listen_pending_.load()" in body
    assert "CancelLessonInteractiveListening();" in body
    assert body.index("lesson_interactive_listen_pending_.load()") < body.index(
        "CancelLessonInteractiveListening();"
    )

def test_lesson_runtime_toggle_click_does_not_hijack_lesson():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleToggleChatEvent()")
    end = app_cc.index("namespace {", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson toggle ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index(
        "if (state == kDeviceStateIdle)"
    )


def test_lesson_runtime_toggle_guard_runs_before_claim_recovery_side_effects():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleToggleChatEvent()")
    end = app_cc.index("namespace {", start)
    body = app_cc[start:end]

    assert "lesson_runtime_active_.load()" in body
    assert "lesson toggle ignored" in body
    assert body.index("lesson_runtime_active_.load()") < body.index("pending_tbot_claim_.active")
    assert body.index("lesson_runtime_active_.load()") < body.index(
        "claim_substate_ == TbotClaimSubstate::ConfirmTimeout"
    )
    assert body.index("lesson_runtime_active_.load()") < body.index("!IsDeviceClaimed()")
    guard = body[
        body.index("lesson_runtime_active_.load()") :
        body.index("pending_tbot_claim_.active")
    ]
    assert "return;" in guard
    assert "ConfirmPendingTbotClaim" not in guard


def test_lesson_runtime_idle_after_child_answer_shows_processing_cue():
    app_cc = read("main/application.cc")

    state_start = app_cc.index("void Application::HandleStateChangedEvent()")
    idle_start = app_cc.index("if (lesson_runtime_active_.load())", state_start)
    idle_end = app_cc.index("// ONLINE", idle_start)
    lesson_idle = app_cc[idle_start:idle_end]

    assert 'display->SetLessonCaption("");' in lesson_idle
    assert "display->ClearChatMessages();" in lesson_idle
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_idle
    assert "display->SetEmotion(" not in lesson_idle
    assert lesson_idle.index('display->SetLessonCaption("");') < lesson_idle.index(
        "display->ClearChatMessages();"
    )

def test_lesson_runtime_suppresses_generic_transcript_chat_updates():
    app_cc = read("main/application.cc")

    sentence_start = app_cc.index('strcmp(state->valuestring, "sentence_start") == 0')
    stt_start = app_cc.index('strcmp(type->valuestring, "stt") == 0', sentence_start)
    sentence_body = app_cc[sentence_start:stt_start]

    assert "if (!lesson_runtime_active_.load())" in sentence_body
    assert sentence_body.index("if (!lesson_runtime_active_.load())") < sentence_body.index(
        'display->SetChatMessage("assistant", message.c_str());'
    )

    llm_start = app_cc.index('strcmp(type->valuestring, "llm") == 0', stt_start)
    stt_body = app_cc[stt_start:llm_start]

    assert "if (!lesson_runtime_active_.load())" in stt_body
    assert stt_body.index("if (!lesson_runtime_active_.load())") < stt_body.index(
        'display->SetChatMessage("user", message.c_str());'
    )

def test_lesson_runtime_suppresses_generic_llm_emotion_updates():
    app_cc = read("main/application.cc")

    llm_start = app_cc.index('strcmp(type->valuestring, "llm") == 0')
    mcp_start = app_cc.index('strcmp(type->valuestring, "mcp") == 0', llm_start)
    llm_body = app_cc[llm_start:mcp_start]

    assert "if (!lesson_runtime_active_.load())" in llm_body
    assert llm_body.index("if (!lesson_runtime_active_.load())") < llm_body.index(
        "display->SetEmotion(emotion_str.c_str());"
    )
    assert llm_body.index("if (!lesson_runtime_active_.load())") < llm_body.index(
        "HandleEmotionGesture(emotion_str.c_str());"
    )

def test_lesson_runtime_suppresses_generic_custom_payload_chat():
    app_cc = read("main/application.cc")

    custom_start = app_cc.index('strcmp(type->valuestring, "custom") == 0')
    lesson_start = app_cc.index('strncmp(type->valuestring, "lesson_", 7) == 0', custom_start)
    custom_body = app_cc[custom_start:lesson_start]

    assert "HandleRobotActionMessage(payload)" in custom_body
    assert "if (!lesson_runtime_active_.load())" in custom_body
    assert custom_body.index("HandleRobotActionMessage(payload)") < custom_body.index(
        "if (!lesson_runtime_active_.load())"
    )
    assert custom_body.index("if (!lesson_runtime_active_.load())") < custom_body.index(
        'display->SetChatMessage("system", payload_str.c_str());'
    )

def test_lesson_runtime_suppresses_generic_alert_ui():
    app_cc = read("main/application.cc")

    alert_start = app_cc.index('strcmp(type->valuestring, "alert") == 0')
    robot_action_start = app_cc.index('strcmp(type->valuestring, "robot_action") == 0', alert_start)
    alert_body = app_cc[alert_start:robot_action_start]

    assert "if (!lesson_runtime_active_.load())" in alert_body
    assert alert_body.index("if (!lesson_runtime_active_.load())") < alert_body.index(
        "Alert(status->valuestring, message->valuestring, emotion->valuestring"
    )

def test_lesson_runtime_suppresses_generic_robot_action_uart():
    app_cc = read("main/application.cc")

    handler_start = app_cc.index("bool Application::HandleRobotActionMessage")
    handler_end = app_cc.index("void Application::HandleEmotionGesture", handler_start)
    handler_body = app_cc[handler_start:handler_end]

    assert "if (lesson_runtime_active_.load())" in handler_body
    assert handler_body.index("if (lesson_runtime_active_.load())") < handler_body.index(
        'cJSON_GetObjectItem(root, "action")'
    )
    guard = handler_body[
        handler_body.index("if (lesson_runtime_active_.load())") :
        handler_body.index('cJSON_GetObjectItem(root, "action")')
    ]
    assert "return false;" in guard
    assert "Schedule(" not in guard
    assert "robot_uart_" not in guard


def test_lesson_runtime_blocks_late_robot_uart_method_execution():
    app_cc = read("main/application.cc")

    methods = [
        "SendLeftArmRaise",
        "SendRightArmRaise",
        "SendLeftArmLower",
        "SendRightArmLower",
        "SendBothArmsRaise",
        "SendBothArmsLower",
        "SendLeftArmSetPercent",
        "SendRightArmSetPercent",
        "SendBothArmsSetPercent",
        "SendHeadTurnLeft",
        "SendHeadTurnRight",
        "SendHeadCenter",
        "SendHeadSetAngle",
        "SendHeadSetPercent",
    ]

    show_activation = app_cc.index("void Application::ShowActivationCode")
    for method in methods:
        start = app_cc.index(f"bool Application::{method}")
        next_bool = app_cc.find("\nbool Application::", start + 1)
        end = next_bool if next_bool != -1 and next_bool < show_activation else show_activation
        body = app_cc[start:end]

        assert "lesson_runtime_active_.load()" in body, method
        assert "lesson robot uart action ignored" in body, method
        assert body.index("lesson_runtime_active_.load()") < body.index("robot_uart_.")
        guard = body[
            body.index("lesson_runtime_active_.load()") :
            body.index("robot_uart_.")
        ]
        assert "return false;" in guard


def test_lesson_interactive_listening_surfaces_visible_turn_cue():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    listening = body[body.index("state == kDeviceStateListening") :]

    assert "lesson_interactive_listen_pending_.exchange(false)" in listening
    lesson_cue = listening[listening.index("lesson_interactive_listen_pending_.exchange(false)") :]
    assert "display->ClearChatMessages();" in lesson_cue
    assert 'display->SetStatus("Con nói nhé...");' in lesson_cue
    assert 'display->SetEmotion("thinking");' not in lesson_cue
    assert 'display->SetChatMessage("system", "Con nói nhé.");' in lesson_cue
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);" in listening
    assert lesson_cue.index("display->ClearChatMessages();") < lesson_cue.index(
        'display->SetChatMessage("system", "Con nói nhé.");'
    )


def test_lesson_interactive_listening_cue_survives_cold_channel_open():
    app_cc = read("main/application.cc")

    open_start = app_cc.index("void Application::OpenChannelTask")
    open_end = app_cc.index("void Application::ArmConnectWatchdog", open_start)
    open_body = app_cc[open_start:open_end]
    assert "self->SetListeningMode(mode);" in open_body

    listening_start = app_cc.index("case kDeviceStateListening:")
    speaking_start = app_cc.index("case kDeviceStateSpeaking:", listening_start)
    listening_body = app_cc[listening_start:speaking_start]

    assert "lesson_interactive_listen_pending_.exchange(false)" in listening_body
    lesson_cue_start = listening_body.index("if (lesson_interactive_listen || lesson_interactive_active)")
    lesson_cue = listening_body[
        lesson_cue_start :
        listening_body.index("} else {", lesson_cue_start)
    ]
    assert "display->ClearChatMessages();" in lesson_cue
    assert 'display->SetStatus("Con nói nhé...");' in lesson_cue
    assert 'display->SetEmotion("thinking");' not in lesson_cue
    assert 'display->SetChatMessage("system", "Con nói nhé.");' in lesson_cue
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);" in listening_body
    assert lesson_cue.index("display->ClearChatMessages();") < lesson_cue.index(
        'display->SetChatMessage("system", "Con nói nhé.");'
    )
    assert listening_body.index('display->SetStatus("Con nói nhé...");') < listening_body.index(
        "protocol_->SendStartListening(listening_mode_);"
    )


def test_listening_state_always_sends_listen_start_even_if_audio_processor_is_running():
    app_cc = read("main/application.cc")

    listening_start = app_cc.index("case kDeviceStateListening:")
    speaking_start = app_cc.index("case kDeviceStateSpeaking:", listening_start)
    listening_body = app_cc[listening_start:speaking_start]

    start_idx = listening_body.index("protocol_->SendStartListening(listening_mode_);")
    processor_guard_idx = listening_body.index("if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning())")

    assert start_idx < processor_guard_idx


def test_set_listening_mode_rearms_listen_start_when_already_listening():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::SetListeningMode")
    end = app_cc.index("ListeningMode Application::GetDefaultListeningMode", start)
    body = app_cc[start:end]

    assert "const bool already_listening = GetDeviceState() == kDeviceStateListening;" in body
    assert "if (already_listening)" in body
    rearm = body[body.index("if (already_listening)") :]
    assert "MAIN_EVENT_STATE_CHANGED" in rearm
    assert body.index("SetDeviceState(kDeviceStateListening);") < body.index("MAIN_EVENT_STATE_CHANGED")

def test_lesson_interactive_listening_pending_is_cleared_on_cancel_paths():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")
    lesson_handler = read("main/lesson_handler.cc")

    assert "CancelLessonInteractiveListening" in app_h
    assert "void Application::CancelLessonInteractiveListening()" in app_cc
    assert "lesson_interactive_listen_pending_.exchange(false)" in app_cc

    stop_listening = app_cc[app_cc.index("void Application::StopListening") : app_cc.index("void Application::HandleToggleChatEvent")]
    assert "CancelLessonInteractiveListening();" in stop_listening

    lesson_stop = lesson_handler[lesson_handler.index('strcmp(type, "lesson_stop") == 0') : lesson_handler.index('strcmp(type, "lesson_error") == 0')]
    assert "Application::GetInstance().CancelLessonInteractiveListening();" in lesson_stop

    failure_helper = lesson_handler[lesson_handler.index("auto end_lesson_after_failure") : lesson_handler.index("const bool is_prepare")]
    lesson_error = lesson_handler[lesson_handler.index('strcmp(type, "lesson_error") == 0') : lesson_handler.index('strcmp(type, "lesson_step") != 0')]
    assert "Application::GetInstance().CancelLessonInteractiveListening();" in failure_helper
    assert "end_lesson_after_failure();" in lesson_error

def test_stale_scheduled_lesson_listen_prepare_does_not_open_mic_after_lesson_end():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::PrepareLessonInteractiveListening()")
    end = app_cc.index("void Application::CancelLessonInteractiveListening()", start)
    body = app_cc[start:end]

    assert "if (!lesson_runtime_active_.load())" in body
    assert body.index("if (!lesson_runtime_active_.load())") < body.index(
        "lesson_interactive_listen_pending_.store(true);"
    )
    assert body.index("if (!lesson_runtime_active_.load())") < body.index("StartListening();")
    inactive_guard = body[
        body.index("if (!lesson_runtime_active_.load())"):
        body.index("lesson_interactive_listen_pending_.store(true);")
    ]
    assert "return;" in inactive_guard
    assert "StartListening" not in inactive_guard

def test_lesson_interactive_listen_prepare_shows_pending_turn_cue():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::PrepareLessonInteractiveListening()")
    end = app_cc.index("void Application::CancelLessonInteractiveListening()", start)
    body = app_cc[start:end]

    assert 'display->SetStatus("Sắp đến lượt con...");' in body
    assert 'display->SetEmotion("thinking");' not in body
    assert "display->ClearChatMessages();" in body
    assert body.index("lesson_interactive_listen_pending_.store(true);") < body.index(
        "display->ClearChatMessages();"
    ) < body.index(
        'display->SetStatus("Sắp đến lượt con...");'
    ) < body.index("StartListening();")

def test_lesson_pending_turn_cue_survives_cold_channel_connecting_state():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStateChangedEvent()")
    end = app_cc.index("void Application::Schedule(std::function", start)
    body = app_cc[start:end]
    connecting = body[
        body.index("case kDeviceStateConnecting:") :
        body.index("case kDeviceStateListening:")
    ]
    lesson_connecting = connecting[
        connecting.index("if (lesson_runtime_active_.load())") :
        connecting.index("break;", connecting.index("if (lesson_runtime_active_.load())"))
    ]

    assert "lesson_interactive_listen_pending_.load()" in lesson_connecting
    assert "display->ClearChatMessages();" in lesson_connecting
    assert 'display->SetStatus("Sắp đến lượt con...");' in lesson_connecting
    assert 'display->SetStatus(Lang::Strings::PLEASE_WAIT);' in lesson_connecting
    assert lesson_connecting.index("lesson_interactive_listen_pending_.load()") < lesson_connecting.index(
        "display->ClearChatMessages();"
    ) < lesson_connecting.index(
        'display->SetStatus("Sắp đến lượt con...");'
    ) < lesson_connecting.index('display->SetStatus(Lang::Strings::PLEASE_WAIT);')

def test_lesson_interactive_cancel_stops_active_child_mic_without_idle_repaint():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")

    assert "lesson_interactive_listening_active_" in app_h
    assert "lesson_idle_repaint_suppressed_" in app_h

    cancel = app_cc[
        app_cc.index("void Application::CancelLessonInteractiveListening()") :
        app_cc.index("void Application::SetLessonRuntimeActive", app_cc.index("void Application::CancelLessonInteractiveListening()"))
    ]
    assert "lesson_interactive_listen_pending_.exchange(false)" in cancel
    assert "lesson_interactive_listening_active_.exchange(false)" in cancel
    assert "lesson_runtime_active_.load()" in cancel
    assert "!lesson_runtime_cancel && !had_lesson_listen" in cancel
    assert "state != kDeviceStateListening" in cancel
    assert "lesson_idle_repaint_suppressed_.store(true);" in cancel
    assert "protocol_->SendStopListening();" in cancel
    assert "listening_started_ms_.store(0);" in cancel
    assert "last_listening_activity_ms_.store(0);" in cancel
    assert "audio_service_.EnableVoiceProcessing(false);" in cancel
    assert "audio_service_.EnableWakeWordDetection(false);" in cancel
    assert "SetDeviceState(kDeviceStateIdle);" in cancel
    listening_cancel = cancel[cancel.index("protocol_->SendStopListening();") :]
    assert listening_cancel.index("protocol_->SendStopListening();") < listening_cancel.index("SetDeviceState(kDeviceStateIdle);")

    state_changed = app_cc[
        app_cc.index("void Application::HandleStateChangedEvent()") :
        app_cc.index("void Application::Schedule(std::function", app_cc.index("void Application::HandleStateChangedEvent()"))
    ]
    idle = state_changed[state_changed.index("case kDeviceStateIdle:") : state_changed.index("case kDeviceStateConnecting:")]
    assert "lesson_idle_repaint_suppressed_.exchange(false)" in idle
    lesson_idle = idle[idle.index("if (lesson_runtime_active_.load())") :]
    assert "if (!suppress_lesson_idle_repaint)" in lesson_idle
    assert lesson_idle.index("if (!suppress_lesson_idle_repaint)") < lesson_idle.index("audio_service_.EnableVoiceProcessing(false);")


def test_terminal_lesson_idle_suppression_rearms_wake_without_repainting_face():
    app_cc = read("main/application.cc")

    state_changed = app_cc[
        app_cc.index("void Application::HandleStateChangedEvent()") :
        app_cc.index("void Application::Schedule(std::function", app_cc.index("void Application::HandleStateChangedEvent()"))
    ]
    idle = state_changed[state_changed.index("case kDeviceStateIdle:") : state_changed.index("case kDeviceStateConnecting:")]
    suppressed = idle[
        idle.index("if (suppress_lesson_idle_repaint)") :
        idle.index("// ONLINE", idle.index("if (suppress_lesson_idle_repaint)"))
    ]

    assert "display->SetEmotion" not in suppressed
    assert "IsDeviceClaimed()" in suppressed
    assert "!connect_in_flight_.load()" in suppressed
    assert "!lesson_asset_sync_quiet_.load()" in suppressed
    assert "audio_service_.EnableWakeWordDetection(true);" in suppressed


def test_lesson_interactive_cancel_recovers_cold_open_connecting_state():
    app_cc = read("main/application.cc")

    cancel = app_cc[
        app_cc.index("void Application::CancelLessonInteractiveListening()") :
        app_cc.index("void Application::SetLessonRuntimeActive", app_cc.index("void Application::CancelLessonInteractiveListening()"))
    ]

    assert "const DeviceState state = GetDeviceState();" in cancel
    assert "state == kDeviceStateConnecting" in cancel
    connecting = cancel[cancel.index("state == kDeviceStateConnecting") : cancel.index("if (state != kDeviceStateListening)")]
    assert "lesson_idle_repaint_suppressed_.store(true);" in connecting
    assert "++connect_generation_;" in connecting
    assert "connect_attempt_active_.store(false);" in connecting
    assert "passive_ws_intent_.store(false);" in connecting
    assert "online_intent_.store(false);" in connecting
    assert "CancelConnectWatchdog();" in connecting
    assert "SetDeviceState(kDeviceStateIdle);" in connecting
    assert "protocol_->SendStopListening();" not in connecting

def test_lesson_start_takes_audio_surface_before_loading_cue():
    lesson_handler = read("main/lesson_handler.cc")

    start_branch = lesson_handler[
        lesson_handler.index('if (strcmp(type, "lesson_start") == 0)') :
        lesson_handler.index('if (strcmp(type, "lesson_pause") == 0)')
    ]
    assert "abort_speaking_if_needed();" in start_branch
    assert "Application::GetInstance().CancelLessonInteractiveListening();" in start_branch
    assert start_branch.index("Application::GetInstance().SetLessonRuntimeActive(true);") < start_branch.index(
        "Application::GetInstance().CancelLessonInteractiveListening();"
    )
    assert start_branch.index("Application::GetInstance().CancelLessonInteractiveListening();") < start_branch.index(
        "start_display->SetStatus(Lang::Strings::PLEASE_WAIT);"
    )

def test_lesson_resume_takes_audio_surface_before_active_cue():
    lesson_handler = read("main/lesson_handler.cc")

    resume_branch = lesson_handler[
        lesson_handler.index('if (strcmp(type, "lesson_resume") == 0)') :
        lesson_handler.index('if (strcmp(type, "lesson_stop") == 0)')
    ]
    assert "Application::GetInstance().SetLessonRuntimeActive(true);" in resume_branch
    assert "abort_speaking_if_needed();" in resume_branch
    assert "Application::GetInstance().CancelLessonInteractiveListening();" in resume_branch
    assert resume_branch.index("Application::GetInstance().SetLessonRuntimeActive(true);") < resume_branch.index(
        "abort_speaking_if_needed();"
    )
    assert resume_branch.index("abort_speaking_if_needed();") < resume_branch.index(
        "Application::GetInstance().CancelLessonInteractiveListening();"
    )
    assert resume_branch.index("Application::GetInstance().CancelLessonInteractiveListening();") < resume_branch.index(
        "display->ClearChatMessages();"
    )

def test_wake_word_afe_uses_low_cost_processing_to_keep_feed_and_fetch_balanced():
    wake_word = read("main/audio/wake_words/afe_wake_word.cc")

    init_start = wake_word.index("afe_config_t* afe_config")
    init_end = wake_word.index("afe_iface_ = esp_afe_handle_from_config", init_start)
    init_body = wake_word[init_start:init_end]

    assert "AFE_MODE_LOW_COST" in init_body
    assert "AFE_MODE_HIGH_PERF" not in init_body
    assert "AEC_MODE_SR_LOW_COST" in init_body
    assert "AEC_MODE_SR_HIGH_PERF" not in init_body
    assert "afe_config->ns_init = false;" in init_body


def test_s3_jpeg_header_does_not_import_esp_video_ioctl_macros():
    header = read("main/display/lvgl_display/jpg/image_to_jpeg.h")

    assert "defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S3)" not in header
    assert "defined(CONFIG_IDF_TARGET_ESP32P4)" in header
    assert "#ifndef V4L2_PIX_FMT_RGB565" in header
    assert "#ifndef V4L2_PIX_FMT_JPEG" in header
