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
    assert "if (passive_ws_intent_.load())" in opened
    passive_opened = opened[opened.index("if (passive_ws_intent_.load())") : opened.index("} else {")]
    assert "StartHeartbeat" not in passive_opened
    assert "DispatchDeviceHeartbeat" not in passive_opened
    assert "EnableWakeWordDetection(true)" in passive_opened
    assert "online_intent_.store(true)" in opened
    assert "online_intent_.store(true)" in set_listening
    assert "passive_ws_intent_.store(false)" in set_listening

def test_passive_lesson_socket_open_promotes_pending_answer_turn_to_listening():
    source = read("main/application.cc")
    open_task = function_body(source, "void Application::OpenChannelTask")
    passive_success = open_task[
        open_task.index("if (passive_preconnect)") :
        open_task.index("} else if (wake_word_invoke)", open_task.index("if (passive_preconnect)"))
    ]

    assert "const bool lesson_answer_turn =" in passive_success
    assert "self->lesson_interactive_listen_pending_.load()" in passive_success
    assert "self->lesson_interactive_listening_active_.load()" in passive_success
    assert "if (self->lesson_runtime_active_.load() && lesson_answer_turn)" in passive_success

    promote = passive_success[
        passive_success.index("if (self->lesson_runtime_active_.load() && lesson_answer_turn)") :
    ]
    assert "self->passive_ws_intent_.store(false);" in promote
    assert "self->StartHeartbeat();" in promote
    assert "self->DispatchDeviceHeartbeat();" in promote
    assert "self->SetListeningMode(kListeningModeManualStop);" in promote
    assert promote.index("self->passive_ws_intent_.store(false);") < promote.index(
        "self->SetListeningMode(kListeningModeManualStop);"
    )

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

def test_passive_lesson_socket_connect_failure_retries_passively():
    source = read("main/application.cc")
    header = read("main/application.h")
    open_task = function_body(source, "void Application::OpenChannelTask")
    passive_failure = open_task[
        open_task.index('ESP_LOGW(TAG, "passive_lesson_websocket_failed")') :
        open_task.index('} else if (wake_word_invoke)', open_task.index('ESP_LOGW(TAG, "passive_lesson_websocket_failed")'))
    ]
    reconnect_tick = function_body(source, "void Application::HandleReconnectTick")
    passive_scheduler = function_body(source, "void Application::SchedulePassiveLessonReconnect")

    assert "void SchedulePassiveLessonReconnect();" in header
    assert "std::atomic<bool> reconnect_passive_" in header
    assert "passive_reconnect_attempt_" in header
    assert "SchedulePassiveLessonReconnect();" in passive_failure
    assert "ScheduleReconnect" not in passive_failure
    assert "StartPassiveLessonWebsocket();" in reconnect_tick
    assert "SetDeviceState(kDeviceStateConnecting)" not in reconnect_tick[: reconnect_tick.index("StartPassiveLessonWebsocket();")]
    assert "passive_lesson_reconnect_scheduled" in passive_scheduler

def test_passive_lesson_socket_watchdog_timeout_retries_passively_from_idle():
    source = read("main/application.cc")
    watchdog = function_body(source, "void Application::HandleConnectWatchdog")

    passive_branch = watchdog[
        watchdog.index("if (passive_ws_intent_.load())") :
        watchdog.index("if (GetDeviceState() == kDeviceStateConnecting)")
    ]

    assert "passive_lesson_connect_watchdog_timeout -> passive backoff" in passive_branch
    assert "backend_offline_.store(true);" in passive_branch
    assert "SchedulePassiveLessonReconnect();" in passive_branch
    assert "ScheduleReconnect" not in passive_branch

def test_passive_lesson_reconnect_tick_defers_instead_of_abandoning_when_not_idle():
    source = read("main/application.cc")
    reconnect_tick = function_body(source, "void Application::HandleReconnectTick")
    passive_start = reconnect_tick.index("if (reconnect_passive_.exchange(false))")
    passive_branch = reconnect_tick[
        passive_start :
        reconnect_tick.index('ESP_LOGI(TAG, "passive_lesson_reconnect_tick attempt=%d"', passive_start)
    ]
    not_idle_branch = passive_branch[
        passive_branch.index("if (state != kDeviceStateIdle)") :
    ]

    assert "auto state = GetDeviceState();" in passive_branch
    assert "state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting" in passive_branch
    assert "SchedulePassiveLessonReconnect();" in not_idle_branch
    assert "passive_reconnect_attempt_ = 0" not in not_idle_branch
    assert "return;" in not_idle_branch

def test_idle_does_not_start_wake_word_while_passive_websocket_tls_is_connecting():
    source = read("main/application.cc")
    state_changed = function_body(source, "void Application::HandleStateChangedEvent")
    idle_case = state_changed[
        state_changed.index("case kDeviceStateIdle:") :
        state_changed.index("case kDeviceStateConnecting:")
    ]

    claimed_idx = idle_case.index("IsDeviceClaimed()")
    enable_idx = idle_case.index("audio_service_.EnableWakeWordDetection(true)", claimed_idx)
    guard_idx = idle_case.index("!connect_in_flight_.load()", claimed_idx, enable_idx)

    assert guard_idx < enable_idx
