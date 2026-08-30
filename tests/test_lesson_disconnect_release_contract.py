from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index(next_signature, start)]


def test_disconnect_and_replacement_use_bounded_worker_cleanup():
    app = read("main/application.cc")

    request = function_body(
        app,
        "void Application::RequestLessonStorageAbandonment()",
        "void Application::LessonMessageTask",
    )
    worker = function_body(
        app,
        "void Application::LessonMessageTask",
        "bool Application::SetDeviceState",
    )
    disconnect = function_body(
        app,
        "void Application::HandleNetworkDisconnectedEvent()",
        "void Application::HandleActivationDoneEvent()",
    )
    closed = app[app.index("protocol_->OnAudioChannelClosed") :]
    closed = closed[: closed.index("protocol_->OnIncomingJson")]
    main_error = app[app.index("if (bits & MAIN_EVENT_ERROR)") :]
    main_error = main_error[: main_error.index("if (bits & MAIN_EVENT_NETWORK_CONNECTED)")]

    assert "xQueueSendToFront(lesson_message_queue_" in request
    assert "LessonQueueItemKind::kAbandonTransport" in request
    assert "kLessonStorageAbandonMaxAttempts = 4" in worker
    assert "kLessonStorageAbandonRetryDelayMs = 10" in worker
    assert "AbandonLessonStorageSession()" in worker
    assert "vTaskDelay(pdMS_TO_TICKS(kLessonStorageAbandonRetryDelayMs));" in worker
    assert "ForceEndLessonSession();" in worker
    assert "RequestLessonStorageAbandonment();" in disconnect
    lesson_error = main_error[main_error.index("if (lesson_runtime_active_.load())") :]
    lesson_error = lesson_error[: lesson_error.index("} else if (connect_attempt_active_.load()")]
    assert "RequestLessonStorageAbandonment();" in lesson_error
    assert "ProtocolLifetimeMatches(" in closed
    current_transport = closed[closed.index("if (!ProtocolLifetimeMatches(") :]
    assert "RequestLessonStorageAbandonment();" in current_transport
    setup_guard_end = current_transport.index("if (lesson_runtime_active_.load() && passive_ws_intent_.load())")
    assert current_transport.index("RequestLessonStorageAbandonment();") < setup_guard_end
    passive_lesson = closed[closed.index("lesson_runtime_active_.load() && passive_ws_intent_.load()") :]
    passive_lesson = passive_lesson[: passive_lesson.index("if (connect_in_flight_.load())")]
    assert passive_lesson.index("RequestLessonStorageAbandonment();") < passive_lesson.index(
        "SchedulePassiveLessonReconnect();"
    )


def test_local_abandon_contract_clears_renderer_layers_and_runtime_ownership():
    handler = read("main/lesson_handler.cc")
    abandon = function_body(
        handler,
        "bool Application::AbandonLessonStorageSession()",
        "void Application::HandleLessonMessage",
    )

    assert "DiscardSession()" in abandon
    assert "g_layer_state.ClearAll();" in abandon
    assert "SetLessonBackground(nullptr)" in abandon
    assert "SetLessonObject(nullptr)" in abandon
    assert "SetLessonRobotOverlay(nullptr)" in abandon
    assert "SetLessonMode(false)" in abandon
    assert "SetLessonRuntimeActive(false);" in abandon
    assert abandon.index("DiscardSession()") < abandon.index("EndLessonSession(")
