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


def test_preconnected_wake_word_can_continue_from_idle_state():
    app_cc = read("main/application.cc")

    start = app_cc.index("void Application::ContinueWakeWordInvoke")
    end = app_cc.index("void Application::HandleStateChangedEvent", start)
    body = app_cc[start:end]
    assert "state != kDeviceStateConnecting && state != kDeviceStateIdle" in body
    assert "kWakeWordAudioChannelOpenMaxAttempts" in body
    assert "wake_audio_channel_open_failed attempt=%d max=%d" in body
    assert "vTaskDelay(pdMS_TO_TICKS(kWakeWordAudioChannelRetryDelayMs));" in body
    assert "SetDeviceState(kDeviceStateIdle);" in body
    assert "protocol_->SendWakeWordDetected(wake_word);" in body
    assert "SetListeningMode(GetDefaultListeningMode());" in body


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
    assert '"audio_communication", 4096, this, tskIDLE_PRIORITY' in processor_task


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
