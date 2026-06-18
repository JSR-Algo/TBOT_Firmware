"""Regression locks for lesson startup over an idle WebSocket.

The ESP server starts assigned lessons from the WebSocket connection hook. If a
claimed robot stays fully offline until wake-word/manual listen, backend/admin
lesson nudges and assigned lesson content have no path to the device.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_claimed_websocket_devices_open_passive_lesson_channel_at_boot():
    source = read("main/application.cc")
    header = read("main/application.h")
    initialize = function_body(source, "void Application::InitializeProtocol")
    passive = function_body(source, "void Application::StartPassiveLessonWebsocket")
    opened = source[source.index("protocol_->OnAudioChannelOpened") : source.index("protocol_->OnAudioChannelClosed")]
    set_listening = function_body(source, "void Application::SetListeningMode")

    assert "std::atomic<bool> passive_ws_intent_" in header
    assert "bool passive_preconnect" in source
    assert "wake_word_invoke, passive_preconnect]" in source
    assert "StartPassiveLessonWebsocket();" in initialize
    assert "IsDeviceClaimed()" in initialize
    assert "passive_ws_intent_.store(true)" in passive
    assert "SetDeviceState(kDeviceStateListening)" not in passive
    assert "if (!passive_ws_intent_.load())" in opened
    assert "online_intent_.store(true)" in set_listening
    assert "passive_ws_intent_.store(false)" in set_listening

def test_passive_lesson_socket_reconnects_without_entering_listening_after_idle_timeout():
    source = read("main/application.cc")
    closed = source[source.index("protocol_->OnAudioChannelClosed") : source.index("protocol_->OnIncomingJson")]
    close_by_intent = function_body(source, "void Application::CloseAudioChannelByIntent")

    passive_idx = closed.index("passive_ws_intent_.load()")
    online_idx = closed.index("online_intent_.load()")
    assert passive_idx < online_idx
    assert "passive_lesson_ws_dropped_unexpected -> passive reconnect" in closed
    assert "StartPassiveLessonWebsocket();" in closed[passive_idx:online_idx]
    assert "ScheduleReconnect" not in closed[passive_idx:online_idx]
    assert "passive_ws_intent_.store(false)" in close_by_intent
