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


def websocket_callback_body(source: str, callback: str) -> str:
    open_start = source.index("bool WebsocketProtocol::OpenAudioChannel")
    for receiver in ("candidate_websocket", "websocket_"):
        needle = f"{receiver}->{callback}"
        start = source.find(needle, open_start)
        if start >= 0:
            break
    else:
        raise AssertionError(f"missing websocket {callback} callback")
    end = source.find("});", start)
    if end < 0:
        raise AssertionError(f"unterminated websocket {callback} callback")
    return source[start : end + len("});")]


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

def test_passive_lesson_socket_success_rearms_wake_word_after_connect_worker_finishes():
    source = read("main/application.cc")
    open_task = function_body(source, "void Application::OpenChannelTask")
    passive_success = open_task[
        open_task.index("if (passive_preconnect)") :
        open_task.index("} else if (wake_word_invoke)", open_task.index("if (passive_preconnect)"))
    ]

    assert "self->connect_in_flight_.store(false);" in open_task
    assert "else if (self->IsDeviceClaimed() && !self->lesson_runtime_active_.load())" in passive_success
    rearm = passive_success[
        passive_success.index("else if (self->IsDeviceClaimed() && !self->lesson_runtime_active_.load())") :
    ]
    assert "self->audio_service_.EnableWakeWordDetection(true);" in rearm
    assert "passive_lesson_wake_word_rearmed" in rearm
    assert "self->audio_service_.IsWakeWordRunning()" in rearm

def test_passive_lesson_socket_success_finishes_deferred_wake_before_rearming():
    source = read("main/application.cc")
    header = read("main/application.h")
    open_task = function_body(source, "void Application::OpenChannelTask")
    continue_wake = function_body(source, "void Application::ContinueWakeWordInvoke")
    passive_success = open_task[
        open_task.index("if (passive_preconnect)") :
        open_task.index("} else if (wake_word_invoke)", open_task.index("if (passive_preconnect)"))
    ]

    assert "std::string deferred_wake_word_;" in header
    assert "passive_ws_intent_.load()" in continue_wake
    assert "deferred_wake_word_ = wake_word;" in continue_wake
    assert "const std::string deferred_wake_word = self->deferred_wake_word_;" in passive_success
    assert "self->deferred_wake_word_.clear();" in passive_success

    finish = passive_success[
        passive_success.index("const std::string deferred_wake_word = self->deferred_wake_word_;") :
        passive_success.index("self->audio_service_.EnableWakeWordDetection(true);")
    ]
    assert "self->FinishWakeWordInvoke(deferred_wake_word);" in finish
    assert finish.index("self->deferred_wake_word_.clear();") < finish.index(
        "self->FinishWakeWordInvoke(deferred_wake_word);"
    )

def test_claimed_idle_clock_tick_reopens_missing_passive_socket():
    source = read("main/application.cc")
    clock_start = source.index("if (bits & MAIN_EVENT_CLOCK_TICK)")
    clock_end = source.index("void Application::HandleNetworkConnectedEvent", clock_start)
    clock_body = source[clock_start:clock_end]

    assert "passive_lesson_idle_socket_missing -> passive reconnect" in clock_body
    reconnect = clock_body[
        clock_body.index("passive_lesson_idle_socket_missing -> passive reconnect") :
    ]
    assert "IsDeviceClaimed()" in clock_body
    assert "GetDeviceState() == kDeviceStateIdle" in clock_body
    assert "protocol_ != nullptr" in clock_body
    assert "!protocol_->IsAudioChannelOpened()" in clock_body
    assert "!connect_in_flight_.load()" in clock_body
    assert "StartPassiveLessonWebsocket();" in reconnect


def test_passive_lesson_socket_tick_sends_ws_only_liveness_without_voice_or_http_intent():
    source = read("main/application.cc")
    protocol = read("main/protocols/protocol.h")
    clock_start = source.index("if (bits & MAIN_EVENT_CLOCK_TICK)")
    clock_end = source.index("void Application::HandleNetworkConnectedEvent", clock_start)
    clock_body = source[clock_start:clock_end]

    assert "virtual bool MaintainPassiveLiveness()" in protocol
    assert "passive_ws_intent_.load()" in clock_body
    assert "protocol_->MaintainPassiveLiveness()" in clock_body
    liveness_start = clock_body.index("bool passive_liveness_failed")
    liveness_end = clock_body.index("if (!passive_liveness_failed", liveness_start)
    liveness = clock_body[liveness_start:liveness_end]
    assert "!connect_in_flight_.load()" in liveness
    assert "kDeviceStateWifiConfiguring" in liveness
    assert "kDeviceStateAudioTesting" in liveness
    assert "SchedulePassiveLessonReconnect();" in liveness
    assert "protocol_->CloseAudioChannel();" in liveness
    assert liveness.index("protocol_->CloseAudioChannel();") < liveness.index(
        "SchedulePassiveLessonReconnect();"
    )
    assert "StartHeartbeat" not in liveness
    assert "DispatchDeviceHeartbeat" not in liveness
    assert "SetListeningMode" not in liveness
    assert "online_intent_.store(true)" not in liveness


def test_passive_liveness_failure_cannot_bypass_backoff_on_same_clock_tick():
    source = read("main/application.cc")
    clock_start = source.index("if (bits & MAIN_EVENT_CLOCK_TICK)")
    clock_end = source.index("void Application::HandleNetworkConnectedEvent", clock_start)
    clock_body = source[clock_start:clock_end]

    failure = clock_body.index("passive_lesson_ws_liveness_failed -> passive backoff")
    missing = clock_body.index("passive_lesson_idle_socket_missing -> passive reconnect")
    assert failure < missing
    between = clock_body[failure:missing]
    assert "passive_liveness_failed = true;" in between
    missing_guard = clock_body[clock_body.rfind("if (", failure, missing):missing]
    assert "!passive_liveness_failed" in missing_guard
    assert "!reconnect_passive_.load()" in missing_guard


def test_passive_liveness_failure_has_one_reconnect_owner_even_if_disconnect_arrives():
    source = read("main/application.cc")
    clock_start = source.index("if (bits & MAIN_EVENT_CLOCK_TICK)")
    clock_end = source.index("void Application::HandleNetworkConnectedEvent", clock_start)
    clock_body = source[clock_start:clock_end]
    liveness_failure = clock_body[
        clock_body.index("passive_lesson_ws_liveness_failed -> passive backoff") - 200 :
        clock_body.index("passive_lesson_idle_socket_missing -> passive reconnect")
    ]
    assert liveness_failure.count("SchedulePassiveLessonReconnect();") == 1
    assert "StartPassiveLessonWebsocket();" not in liveness_failure

    closed = source[source.index("protocol_->OnAudioChannelClosed") : source.index("protocol_->OnIncomingJson")]
    assert closed.count("passive_liveness_reconnect_pending") >= 2
    for reconnect_log in (
        "lesson passive ws dropped -> passive reconnect",
        "passive_lesson_ws_dropped_unexpected -> passive reconnect",
    ):
        branch_end = closed.index(reconnect_log)
        branch = closed[closed.rfind("if (", 0, branch_end):branch_end]
        assert "reconnect_passive_.load()" in branch

    lesson_branch_start = closed.index("lesson_runtime_active_.load() && passive_ws_intent_.load()")
    lesson_branch_end = closed.index("if (connect_in_flight_.load())", lesson_branch_start)
    lesson_branch = closed[lesson_branch_start:lesson_branch_end]
    assert "PassiveReconnectHasOwner(" in lesson_branch
    assert "connect_in_flight_.load()" in lesson_branch


def test_websocket_passive_liveness_uses_privacy_safe_json_ping_and_consumes_pong():
    header = read("main/protocols/websocket_protocol.h")
    source = read("main/protocols/websocket_protocol.cc")

    assert "bool MaintainPassiveLiveness() override;" in header
    maintain = function_body(source, "bool WebsocketProtocol::MaintainPassiveLiveness")
    assert 'websocket_->Send("{\\\"type\\\":\\\"ping\\\"}")' in maintain
    assert "PassiveWebsocketLiveness::Action::kTimedOut" in maintain
    assert "error_occurred_ = true" in maintain
    assert "SetError(" not in maintain
    assert "session_id_" not in maintain
    assert "token_" not in maintain
    assert "device_id" not in maintain

    on_data = websocket_callback_body(source, "OnData")
    assert 'strcmp(type->valuestring, "pong") == 0' in on_data
    assert "passive_liveness_.OnPong" in on_data
    pong_branch = on_data[on_data.index('strcmp(type->valuestring, "pong") == 0') :]
    assert "on_incoming_json_" not in pong_branch[: pong_branch.index("} else")]


def test_websocket_liveness_failure_drops_all_inbound_before_voice_lesson_or_config_dispatch():
    protocol = read("main/protocols/protocol.h")
    source = read("main/protocols/websocket_protocol.cc")
    on_data = websocket_callback_body(source, "OnData")

    assert "std::atomic<bool> error_occurred_" in protocol
    lease = on_data.index("inbound_gate_.Acquire(connection_epoch)")
    fail_closed = on_data.index("if (!inbound_lease")
    binary = on_data.index("if (binary)")
    parse = on_data.index("cJSON_ParseWithLength")
    audio = on_data.index("on_incoming_audio_")
    config_or_lesson = on_data.index("on_incoming_json_")
    assert lease < fail_closed < binary
    assert fail_closed < parse
    assert fail_closed < audio
    assert fail_closed < config_or_lesson
    gate = on_data[fail_closed:binary]
    assert "return;" in gate
    assert "ws_stale_inbound_dropped" in gate

    open_channel = function_body(source, "bool WebsocketProtocol::OpenAudioChannel")
    assert "inbound_gate_.BeginConnectionMutation()" in open_channel
    mutation = open_channel.index("inbound_gate_.BeginConnectionMutation()")
    on_data_install = open_channel.index("candidate_websocket->OnData")
    connect = open_channel.index("replacement_websocket->Connect(connect_url.c_str())")
    hello_wait = open_channel.index("xEventGroupWaitBits")
    replace = open_channel.index("websocket_ = std::move(replacement_websocket)")
    assert mutation < on_data_install < connect < hello_wait < replace
    assert "connection_mutation.epoch()" in open_channel[mutation:replace]
    assert "[this, connection_epoch, callback_transport_epoch, hello_signal]" in open_channel
    disconnect = websocket_callback_body(source, "OnDisconnected")
    assert "inbound_gate_.Acquire(connection_epoch)" in disconnect
    assert "disconnect_lease.IsCurrentEpoch()" in disconnect
    stale = disconnect[disconnect.index("const bool current_connection") :]
    assert "if (!current_connection)" in stale
    stale_return = stale[:stale.index("int err_code")]
    assert "return;" in stale_return
    assert "on_audio_channel_closed_" not in stale_return

    header = read("main/protocols/websocket_protocol.h")
    assert header.index("ConnectionInboundGate inbound_gate_;") < header.index(
        "std::unique_ptr<WebSocket> websocket_;"
    )
    close = function_body(source, "void WebsocketProtocol::CloseAudioChannel")
    assert "error_occurred_ = true;" in close
    callback_context = close[close.index("if (inbound_gate_.CurrentThreadHasLease())") :]
    assert callback_context.index("error_occurred_ = true;") < callback_context.index("return;")
    assert "CompleteCloseAndNotify();" in close
    detach = function_body(source, "void WebsocketProtocol::DetachAndResetWebsocket")
    assert "websocket_->OnData(nullptr);" not in detach
    assert "websocket_->OnDisconnected(nullptr);" not in detach
    assert "websocket_.reset();" in detach
    complete_close = function_body(source, "void WebsocketProtocol::CompleteCloseAndNotify")
    assert "}\n    DetachAndResetWebsocket();" in complete_close
    assert "websocket_.reset();" in detach
    destructor = function_body(source, "WebsocketProtocol::~WebsocketProtocol")
    assert "DetachAndResetWebsocket();" in destructor
    assert "NotifyAudioChannelClosedOnce" not in destructor

    assert "void WebsocketProtocol::CompleteDeferredClose(uint32_t connection_epoch)" in source
    complete = function_body(source, "void WebsocketProtocol::CompleteDeferredClose")
    assert "BeginFailureMutationIfCurrent(connection_epoch)" in complete
    assert "if (!failure_mutation.Matched())" in complete
    assert "close_state_.TakeDeferred(connection_epoch)" in complete
    assert "DetachAndResetWebsocket();" in complete
    assert "}\n    DetachAndResetWebsocket();\n    NotifyAudioChannelClosedOnce();" in complete
    complete_now = function_body(source, "void WebsocketProtocol::CompleteCloseAndNotify")
    assert "BeginFailureMutation()" in complete_now
    assert "DetachAndResetWebsocket();" in complete_now
    assert "}\n    DetachAndResetWebsocket();\n    NotifyAudioChannelClosedOnce();" in complete_now
    reentrant = close[close.index("if (inbound_gate_.CurrentThreadHasLease())") :]
    assert "const uint32_t connection_epoch = inbound_gate_.CurrentEpoch();" in reentrant
    assert "close_state_.MarkDeferred(connection_epoch)" in reentrant
    assert "ScheduleDeferredProtocolClose(this, connection_epoch);" in reentrant
    assert "websocket_->Close();" not in reentrant

    app_header = read("main/application.h")
    app_source = read("main/application.cc")
    assert "void ScheduleDeferredProtocolClose(Protocol* expected, uint32_t connection_epoch);" in app_header
    assert "std::atomic<uint64_t> protocol_generation_" in app_header
    deferred = function_body(app_source, "void Application::ScheduleDeferredProtocolClose")
    assert "const uint64_t expected_generation = protocol_generation_.load(" in deferred
    assert "ProtocolLifetimeMatches(" in deferred
    assert "protocol_->CompleteDeferredClose(connection_epoch);" in deferred
    initialize = function_body(app_source, "void Application::InitializeProtocol")
    assert "protocol_generation_.fetch_add(1," in initialize
    assert max(
        index for index in range(len(initialize))
        if initialize.startswith("protocol_ = std::make_unique", index)
    ) < initialize.index("protocol_generation_.fetch_add(1,")
    reset = function_body(app_source, "void Application::DoResetProtocol")
    assert reset.index("protocol_.reset()") < reset.index("protocol_generation_.fetch_add(1,")

    closed = app_source[
        app_source.index("protocol_->OnAudioChannelClosed") :
        app_source.index("protocol_->OnIncomingJson")
    ]
    for effect in (
        "tts_audio_accepting_.store(false)",
        "StopHeartbeat();",
        "board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER)",
        "audio_service_.PopPacketFromSendQueue()",
        "SetDeviceState(kDeviceStateIdle);",
    ):
        assert effect in closed
    maintain = function_body(source, "bool WebsocketProtocol::MaintainPassiveLiveness")
    assert maintain.count("inbound_gate_.FailCurrent();") >= 2

def test_passive_lesson_socket_reconnects_without_entering_listening_after_idle_timeout():
    source = read("main/application.cc")
    closed = source[source.index("protocol_->OnAudioChannelClosed") : source.index("protocol_->OnIncomingJson")]
    close_by_intent = function_body(source, "void Application::CloseAudioChannelByIntent")

    passive_idx = closed.index("passive_ws_intent_.load()")
    online_idx = closed.index("online_intent_.load()")
    assert passive_idx < online_idx
    assert "passive_lesson_ws_dropped_unexpected -> passive reconnect" in closed
    assert "SchedulePassiveLessonReconnect();" in closed[passive_idx:online_idx]
    assert "StartPassiveLessonWebsocket();" not in closed[passive_idx:online_idx]
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

def test_passive_lesson_socket_worker_unavailable_retries_passively():
    source = read("main/application.cc")
    passive = function_body(source, "void Application::StartPassiveLessonWebsocket")
    failure = passive[
        passive.index("if (!StartOpenChannelWorker(ctx))") :
        passive.index("}", passive.index('ESP_LOGE(TAG, "lesson_ws worker unavailable")'))
    ]

    assert "StartOpenChannelWorker(ctx)" in failure
    assert "delete ctx;" in failure
    assert "connect_in_flight_.store(false);" in failure
    assert "passive_ws_intent_.store(false);" in failure
    assert "CancelConnectWatchdog();" in failure
    assert 'ESP_LOGE(TAG, "lesson_ws worker unavailable");' in failure
    assert "SchedulePassiveLessonReconnect();" in failure


def test_passive_lesson_reconnect_uses_a_persistent_internal_worker_stack():
    source = read("main/application.cc")
    passive = function_body(source, "void Application::StartPassiveLessonWebsocket")
    starter = function_body(source, "bool Application::StartOpenChannelWorker")
    worker = function_body(source, "void Application::OpenChannelTask")
    constructor = function_body(source, "Application::Application")

    assert "DRAM_ATTR StaticTask_t open_channel_task_buffer;" in source
    assert "DRAM_ATTR StackType_t open_channel_task_stack[kOpenChannelWorkerStackDepth]" in source
    assert "DRAM_ATTR StaticQueue_t open_channel_queue_buffer;" in source
    assert '"lesson_ws"' in constructor
    assert "xTaskCreateWithCaps(" not in starter
    assert "xQueueSend(open_channel_queue" in starter
    assert "xTaskCreate" not in passive
    assert "StartOpenChannelWorker(ctx)" in passive
    assert "xQueueReceive(open_channel_queue" in worker
    assert "vTaskDeleteWithCaps(nullptr);" not in worker


def test_network_disconnect_defers_channel_close_until_connect_worker_exits():
    source = read("main/application.cc")
    header = read("main/application.h")
    close = function_body(source, "void Application::CloseAudioChannelByIntent")
    worker = function_body(source, "void Application::OpenChannelTask")
    network_drop = function_body(source, "void Application::HandleNetworkDisconnectedEvent")

    assert "ConnectCloseDeferral connect_close_deferral_;" in header
    assert "connect_close_deferral_.Request(connect_in_flight_.load())" in close
    defer = close[close.index("connect_close_deferral_.Request") :]
    assert "++connect_generation_;" in defer
    assert "channel_close_deferred_until_connect_worker_exit" in defer
    assert defer.index("return;") < defer.index("protocol_->CloseAudioChannel()")

    worker_done = worker.index("connect_in_flight_.store(false)")
    drain = worker.index("connect_close_deferral_.TakeAfterWorker()")
    generation_check = worker.index("gen != self->connect_generation_.load()")
    assert worker_done < drain < generation_check
    drain_body = worker[drain:generation_check]
    assert "protocol_->CloseAudioChannel();" in drain_body
    assert "return;" in drain_body

    assert "CloseAudioChannelByIntent();" in network_drop
    assert "protocol_->CloseAudioChannel();" not in network_drop


def test_connect_cancellation_suppresses_success_publication_and_defers_reboot():
    source = read("main/application.cc")
    header = read("main/application.h")
    initialize = function_body(source, "void Application::InitializeProtocol")
    worker = function_body(source, "void Application::OpenChannelTask")
    reboot = function_body(source, "void Application::Reboot")
    complete_reboot = function_body(source, "void Application::CompleteReboot")

    assert "std::atomic<bool> reboot_pending_" in header
    assert "bool IsConnectSuccessPublicationSuppressed() const;" in header
    assert "void CompleteReboot();" in header

    connected = initialize[
        initialize.index("protocol_->OnConnected") :
        initialize.index("protocol_->OnNetworkError")
    ]
    opened = initialize[
        initialize.index("protocol_->OnAudioChannelOpened") :
        initialize.index("protocol_->OnAudioChannelClosed")
    ]
    for callback in (connected, opened):
        guard = callback.index("IsConnectSuccessPublicationSuppressed()")
        heartbeat = callback.find("StartHeartbeat")
        online = callback.find("online_intent_.store(true)")
        assert guard >= 0
        assert heartbeat == -1 or guard < heartbeat
        assert online == -1 or guard < online
        suppressed = callback[guard : callback.index("return;", guard) + len("return;")]
        assert "online_intent_.store(false);" in suppressed
        assert "StopHeartbeat();" in suppressed

    assert "if (connect_in_flight_.load())" in reboot
    deferred_reboot = reboot[reboot.index("if (connect_in_flight_.load())") :]
    assert "CloseAudioChannelByIntent();" in deferred_reboot
    assert "reboot_pending_.store(true);" in deferred_reboot
    assert "reboot_deferred_until_connect_worker_exit" in deferred_reboot
    assert deferred_reboot.index("return;") < deferred_reboot.index("CompleteReboot();")
    assert "protocol_.reset();" in complete_reboot
    assert "audio_service_.Stop();" in complete_reboot
    assert "esp_restart();" in complete_reboot

    worker_done = worker.index("connect_in_flight_.store(false)")
    reboot_drain = worker.index("reboot_pending_.exchange(false)")
    reset_drain = worker.index("reset_pending_.exchange(false)")
    close_drain = worker.index("connect_close_deferral_.TakeAfterWorker()")
    assert worker_done < reboot_drain < reset_drain < close_drain
    reboot_branch = worker[reboot_drain:reset_drain]
    assert "connect_close_deferral_.Cancel();" in reboot_branch
    assert "CompleteReboot();" in reboot_branch
    assert "return;" in reboot_branch

def test_passive_lesson_socket_failure_during_answer_turn_retries_passively():
    source = read("main/application.cc")
    open_task = function_body(source, "void Application::OpenChannelTask")
    failure = open_task[
        open_task.index("} else {", open_task.index("if (ok)")) :
        open_task.index('ESP_LOGW(TAG, "passive_lesson_websocket_failed")')
    ]

    assert "const bool lesson_answer_turn =" in failure
    assert "self->lesson_interactive_listen_pending_.load()" in failure
    assert "self->lesson_interactive_listening_active_.load()" in failure
    assert "if (self->lesson_runtime_active_.load())" in failure
    assert "lesson_answer_turn || (!passive_preconnect && !wake_word_invoke)" in failure

    lesson_failure = failure[
        failure.index("lesson_answer_turn || (!passive_preconnect && !wake_word_invoke)") :
    ]
    assert "self->passive_ws_intent_.store(false);" in lesson_failure
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_failure
    assert "self->SchedulePassiveLessonReconnect();" in lesson_failure
    before_retry = lesson_failure[: lesson_failure.index("self->SchedulePassiveLessonReconnect();")]
    clear_guard = before_retry[
        before_retry.index("if (!lesson_answer_turn)") :
        before_retry.index("self->lesson_idle_repaint_suppressed_")
    ]
    assert "self->lesson_interactive_listen_generation_.fetch_add(1);" in clear_guard
    assert "self->lesson_interactive_listen_pending_.store(false);" in clear_guard
    assert "self->lesson_interactive_listening_active_.store(false);" in clear_guard
    assert "return;" in lesson_failure

def test_passive_lesson_socket_watchdog_timeout_retries_passively_from_idle():
    source = read("main/application.cc")
    watchdog = function_body(source, "void Application::HandleConnectWatchdog")

    passive_start = watchdog.index("if (passive_ws_intent_.load())")
    passive_branch = watchdog[
        passive_start :
        watchdog.index('ESP_LOGW(TAG, "lesson connect watchdog timeout -> suppress generic reconnect"', passive_start)
    ]

    assert "passive_lesson_connect_watchdog_timeout -> passive backoff" in passive_branch
    assert "backend_offline_.store(true);" in passive_branch
    assert "connect_in_flight_.store(false);" in passive_branch
    assert passive_branch.index("connect_in_flight_.store(false);") < passive_branch.index(
        "SchedulePassiveLessonReconnect();"
    )
    assert "SchedulePassiveLessonReconnect();" in passive_branch
    assert "ScheduleReconnect" not in passive_branch


def test_websocket_candidate_is_not_published_until_connect_and_hello_finish():
    source = read("main/protocols/websocket_protocol.cc")
    open_channel = function_body(source, "bool WebsocketProtocol::OpenAudioChannel")

    assert "replacement_websocket->Connect(connect_url.c_str())" in open_channel
    assert "replacement_websocket->Send(message)" in open_channel
    assert "SendText(message)" not in open_channel

    create = open_channel.index("auto replacement_websocket = network->CreateWebSocket(1);")
    connect = open_channel.index("replacement_websocket->Connect(connect_url.c_str())")
    hello_wait = open_channel.index("xEventGroupWaitBits")
    publish = open_channel.index("websocket_ = std::move(replacement_websocket);")
    opened_callback = open_channel.index("on_audio_channel_opened_()")

    assert create < connect < hello_wait < publish < opened_callback

def test_passive_lesson_socket_watchdog_during_answer_turn_retries_passively():
    source = read("main/application.cc")
    watchdog = function_body(source, "void Application::HandleConnectWatchdog")
    passive_branch = watchdog[
        watchdog.index("if (passive_ws_intent_.load())") :
        watchdog.index("if (lesson_runtime_active_.load())")
    ]

    assert "const bool lesson_answer_turn =" in passive_branch
    assert "lesson_interactive_listen_pending_.load()" in passive_branch
    assert "lesson_interactive_listening_active_.load()" in passive_branch
    assert "if (lesson_runtime_active_.load() && lesson_answer_turn)" in passive_branch

    lesson_timeout = passive_branch[
        passive_branch.index("if (lesson_runtime_active_.load() && lesson_answer_turn)") :
        passive_branch.index("return;", passive_branch.index("if (lesson_runtime_active_.load() && lesson_answer_turn)")) + len("return;")
    ]
    assert "passive_ws_intent_.store(false);" in lesson_timeout
    assert "display->SetStatus(Lang::Strings::PLEASE_WAIT);" in lesson_timeout
    assert "connect_in_flight_.store(false);" in lesson_timeout
    assert lesson_timeout.index("connect_in_flight_.store(false);") < lesson_timeout.index(
        "SchedulePassiveLessonReconnect();"
    )
    assert "SchedulePassiveLessonReconnect();" in lesson_timeout
    before_retry = lesson_timeout[: lesson_timeout.index("SchedulePassiveLessonReconnect();")]
    assert "lesson_interactive_listen_generation_.fetch_add(1);" not in before_retry
    assert "lesson_interactive_listen_pending_.store(false);" not in before_retry
    assert "lesson_interactive_listening_active_.store(false);" not in before_retry
    assert "return;" in lesson_timeout

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

def test_passive_lesson_reconnect_tick_reopens_during_answer_turn_not_idle():
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

    assert "const bool lesson_answer_turn =" in not_idle_branch
    assert "lesson_runtime_active_.load()" in not_idle_branch
    assert "lesson_interactive_listen_pending_.load()" in not_idle_branch
    assert "lesson_interactive_listening_active_.load()" in not_idle_branch
    assert "state == kDeviceStateSpeaking" in not_idle_branch
    assert "state == kDeviceStateListening" in not_idle_branch
    assert "state == kDeviceStateConnecting" in not_idle_branch
    answer_turn = not_idle_branch[
        not_idle_branch.index("if (lesson_answer_turn") :
        not_idle_branch.index("SchedulePassiveLessonReconnect();")
    ]
    assert "StartPassiveLessonWebsocket();" in answer_turn
    assert answer_turn.index("StartPassiveLessonWebsocket();") < answer_turn.index("return;")

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
