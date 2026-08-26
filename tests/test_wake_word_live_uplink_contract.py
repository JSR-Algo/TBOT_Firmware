from pathlib import Path


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


def test_wake_entry_points_do_not_start_secondary_opus_encoding():
    source = read("main/application.cc")
    detected = function_body(source, "void Application::HandleWakeWordDetectedEvent()")
    direct = function_body(source, "void Application::WakeWordInvoke(const std::string& wake_word)")

    assert "audio_service_.EncodeWakeWord()" not in detected
    assert "audio_service_.EncodeWakeWord()" not in direct
    assert "ContinueWakeWordInvoke(wake_word)" in detected
    assert "ContinueWakeWordInvoke(wake_word)" in direct
    assert "audio_service_.EncodeWakeWord()" not in source


def test_wake_finish_sends_label_then_starts_live_listening_without_preroll():
    source = read("main/application.cc")
    finish = function_body(source, "void Application::FinishWakeWordInvoke(const std::string& wake_word)")

    detected = finish.index("protocol_->SendWakeWordDetected(wake_word);")
    listening = finish.index("SetListeningMode(kListeningModeAutoStop);")
    assert detected < listening
    assert "audio_service_.PopWakeWordPacket()" not in finish
    assert "protocol_->SendAudio(std::move(packet))" not in finish
    assert "audio_service_.PopWakeWordPacket()" not in source


def test_wake_preroll_storage_and_secondary_encoder_are_structurally_absent():
    wake_files = [
        read("main/audio/wake_word.h"),
        read("main/audio/wake_words/afe_wake_word.h"),
        read("main/audio/wake_words/afe_wake_word.cc"),
        read("main/audio/wake_words/custom_wake_word.h"),
        read("main/audio/wake_words/custom_wake_word.cc"),
        read("main/audio/wake_words/esp_wake_word.h"),
        read("main/audio/wake_words/esp_wake_word.cc"),
    ]
    wake_sources = "\n".join(wake_files)
    for forbidden in (
        "EncodeWakeWordData",
        "GetWakeWordOpus",
        "StoreWakeWordData",
        "wake_word_pcm_",
        "wake_word_opus_",
        "wake_word_encode_task_",
        "wake_word_mutex_",
        "wake_word_cv_",
        "encode_active_",
        "ENCODE_EXITED_EVENT",
        '"encode_wake_word"',
        "esp_opus_enc_",
        "xTaskCreateWithCaps",
    ):
        assert forbidden not in wake_sources

    service_header = read("main/audio/audio_service.h")
    service_source = read("main/audio/audio_service.cc")
    assert "void EncodeWakeWord();" not in service_header
    assert "PopWakeWordPacket" not in service_header
    assert "void AudioService::EncodeWakeWord()" not in service_source
    assert "AudioService::PopWakeWordPacket" not in service_source


def test_live_audio_service_opus_encoder_remains_with_measured_stack_budget():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")

    assert "kOpusCodecTaskStackBytes = 28 * 1024" in header
    assert '"opus_codec", kOpusCodecTaskStackBytes, this' in source
    assert "esp_opus_enc_process(opus_encoder_, &in, &out)" in source


def test_live_uplink_safety_contract_runs_in_host_ci():
    workflow = read(".github/workflows/build.yml")
    assert "tests/test_wake_word_live_uplink_contract.py" in workflow
