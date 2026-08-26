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


def test_live_uplink_safety_contract_runs_in_host_ci():
    workflow = read(".github/workflows/build.yml")
    assert "tests/test_wake_word_live_uplink_contract.py" in workflow
