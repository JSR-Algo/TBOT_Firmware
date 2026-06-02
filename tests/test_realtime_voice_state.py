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


def test_s3_jpeg_header_does_not_import_esp_video_ioctl_macros():
    header = read("main/display/lvgl_display/jpg/image_to_jpeg.h")

    assert "defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S3)" not in header
    assert "defined(CONFIG_IDF_TARGET_ESP32P4)" in header
    assert "#ifndef V4L2_PIX_FMT_RGB565" in header
    assert "#ifndef V4L2_PIX_FMT_JPEG" in header
