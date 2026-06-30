from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


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
    assert "vTaskDelay(pdMS_TO_TICKS(kSpeakingTimeoutMs))" in app_cc
    assert "HandleSpeakingTimeout(current_generation)" in app_cc
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


def test_listening_watchdog_exits_stale_autostop_turns():
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
    assert "listening_mode_ == kListeningModeRealtime" in watchdog_body
    assert "listening_watchdog_timeout" in watchdog_body
    assert "protocol_->SendStopListening();" in watchdog_body
    assert "audio_service_.EnableVoiceProcessing(false);" in watchdog_body
    assert "SetDeviceState(kDeviceStateIdle);" in watchdog_body


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
    assert "xTaskCreate(&Application::OpenChannelTask" in continue_body
    assert "protocol_->OpenAudioChannel()" not in continue_body

    open_start = app_cc.index("void Application::OpenChannelTask")
    open_end = app_cc.index("void Application::ArmConnectWatchdog", open_start)
    open_body = app_cc[open_start:open_end]
    assert "ctx->wake_word" in open_body
    assert "kWakeWordAudioChannelOpenMaxAttempts" in open_body
    assert "FinishWakeWordInvoke(wake_word)" in open_body


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


def test_afe_background_tasks_do_not_starve_idle_watchdog():
    wake_word = read("main/audio/wake_words/afe_wake_word.cc")
    processor = read("main/audio/processors/afe_audio_processor.cc")

    wake_task = wake_word[wake_word.index('"audio_detection"') - 180:wake_word.index('"audio_detection"') + 120]
    processor_task = processor[processor.index('"audio_communication"') - 180:processor.index('"audio_communication"') + 120]

    assert '"audio_detection", 4096, this, tskIDLE_PRIORITY' in wake_task
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

def test_audio_uplink_pipeline_has_send_boundary_diagnostics():
    app_cc = read("main/application.cc")
    audio_cc = read("main/audio/audio_service.cc")
    ws_cc = read("main/protocols/websocket_protocol.cc")

    assert "audio_uplink_packet_queued" in audio_cc
    assert "MAIN_EVENT_SEND_AUDIO protocol_unavailable" in app_cc
    assert "MAIN_EVENT_SEND_AUDIO packet" in app_cc
    assert "Websocket SendAudio" in ws_cc

def test_lesson_image_render_quiets_audio_uplink_without_dropping_packets():
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
    quiet_idx = send_body.index("IsLessonNetworkRenderQuiet()")
    pop_idx = send_body.index("audio_service_.PopPacketFromSendQueue()")
    assert quiet_idx < pop_idx
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

def test_sample_lesson_asset_sync_does_not_mkdir_sd_mount_point():
    mcp_server = read("main/mcp_server.cc")

    ensure_start = mcp_server.index("void EnsureSampleLessonAssetDir()")
    ensure_end = mcp_server.index("bool DownloadLessonAssetToFile", ensure_start)
    ensure_body = mcp_server[ensure_start:ensure_end]

    assert 'EnsureDirOrThrow("/sdcard/tbot")' in ensure_body
    assert 'EnsureDirOrThrow("/sdcard/tbot/lesson-assets")' in ensure_body
    assert 'DirectoryExists("/sdcard")' not in ensure_body
    assert 'EnsureDir("/sdcard")' not in ensure_body
    assert "failed to create SD directory" in mcp_server
    assert "if (!EnsureSampleLessonAssetDir())" not in mcp_server
    assert "EnsureSampleLessonAssetDir();" in mcp_server

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

    assert "lesson_interactive_listen_pending_.exchange(false)" in manual_body
    assert "SetDeviceState(kDeviceStateListening);" in manual_body
    assert "protocol_->SendStartListening(kListeningModeManualStop);" in manual_body
    assert "audio_service_.EnableVoiceProcessing(true);" in manual_body
    assert "SetDeviceState(kDeviceStateIdle);" in manual_body
    assert manual_body.index("SetDeviceState(kDeviceStateListening);") < manual_body.index("SetDeviceState(kDeviceStateIdle);")


def test_lesson_prompt_start_listening_event_does_not_abort_speaking_prompt():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    speaking = body[body.index("state == kDeviceStateSpeaking") : body.index("state == kDeviceStateListening")]

    assert "lesson_interactive_listen_pending_.load()" in speaking
    assert "lesson prompt still speaking; defer listening" in speaking
    assert speaking.index("lesson_interactive_listen_pending_.load()") < speaking.index("AbortSpeaking(kAbortReasonNone);")

def test_lesson_interactive_listening_surfaces_visible_turn_cue():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::HandleStartListeningEvent")
    end = app_cc.index("void Application::HandleStopListeningEvent", start)
    body = app_cc[start:end]
    listening = body[body.index("state == kDeviceStateListening") :]

    assert "lesson_interactive_listen_pending_.exchange(false)" in listening
    lesson_cue = listening[listening.index("lesson_interactive_listen_pending_.exchange(false)") :]
    assert 'display->SetStatus("Con nói nhé...");' in lesson_cue
    assert 'display->SetChatMessage("system", "Con nói nhé.");' in lesson_cue
    assert "audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);" in lesson_cue

def test_lesson_interactive_listening_pending_is_cleared_on_cancel_paths():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")
    lesson_handler = read("main/lesson_handler.cc")

    assert "CancelLessonInteractiveListening" in app_h
    assert "void Application::CancelLessonInteractiveListening()" in app_cc
    assert "lesson_interactive_listen_pending_.store(false);" in app_cc

    stop_listening = app_cc[app_cc.index("void Application::StopListening") : app_cc.index("void Application::HandleToggleChatEvent")]
    assert "CancelLessonInteractiveListening();" in stop_listening

    lesson_stop = lesson_handler[lesson_handler.index('strcmp(type, "lesson_stop") == 0') : lesson_handler.index('strcmp(type, "lesson_error") == 0')]
    assert "Application::GetInstance().CancelLessonInteractiveListening();" in lesson_stop

    lesson_error = lesson_handler[lesson_handler.index('strcmp(type, "lesson_error") == 0') : lesson_handler.index('strcmp(type, "lesson_step") != 0')]
    assert "Application::GetInstance().CancelLessonInteractiveListening();" in lesson_error

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
