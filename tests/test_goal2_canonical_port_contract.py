from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_goal2_storage_and_build_identity_modules_are_registered():
    root_cmake = read("CMakeLists.txt")
    main_cmake = read("main/CMakeLists.txt")

    assert "set(TBOT_HIL_PROFILE" not in root_cmake
    assert '"esp_build_identity.cc"' in main_cmake
    assert '"sd_fat_session_guard.cc"' in main_cmake
    hil_sources = main_cmake.split("if(CONFIG_TBOT_HIL_STORAGE_FAULTS)", 1)[1].split(
        "endif()", 1
    )[0]
    assert '"physical_sd_identity.cc"' in hil_sources
    assert '"lesson_storage_hil_controller.cc"' in hil_sources
    assert '"lesson_storage_hil_fixture.cc"' in hil_sources
    assert '"lesson_storage_hil_hooks.cc"' in hil_sources
    assert '"lesson_storage_hil_mcp_tools.cc"' in hil_sources
    identity = read("main/esp_build_identity.cc")
    assert "#ifdef TBOT_HIL_PROFILE" in identity
    assert "TBOT_EMBEDDED_PROFILE" in identity


def test_goal2_runtime_integration_points_exist_on_canonical_architecture():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")
    websocket = read("main/protocols/websocket_protocol.cc")
    mcp_server = read("main/mcp_server.cc")
    application = read("main/application.cc")

    assert '#include "physical_sd_identity.h"' in board
    assert "identity_registry.ObserveMountedCard" in board
    assert '#include "esp_build_identity.h"' in websocket
    identity = websocket.index("ReadRunningEspBuildIdentity")
    set_header = websocket.index("websocket_->SetHeader", identity)
    connect = websocket.index("websocket_->Connect", set_header)
    assert identity < set_header < connect
    assert "RegisterLessonStorageHilMcpTools" in mcp_server
    assert "TBOT_HIL_STORAGE_FAULTS_ENABLED non-production-image" in application


def test_goal2_hil_status_is_controller_first_and_nonblocking_for_schema_v2():
    source = read("main/lesson_storage_hil_mcp_tools.cc")
    body = source.split("std::string CallStatusForSchema", 1)[1].split(
        "std::string CallStatus(", 1
    )[0]

    assert body.index("LessonStorageHilController::GetInstance().Status()") < body.index(
        "TryAcquire()"
    )
    assert "if (schema_version == 2)" in body
    assert "AddUnavailablePhysicalSdIdentity" in body


def test_canonical_lesson_session_holds_and_releases_storage_reservation():
    handler = read("main/lesson_handler.cc")
    runner = read("scripts/run_host_native_lesson_handler_test.sh")

    assert '#include "lesson_asset_storage_coordinator.h"' in handler
    assert "lesson_asset_generation" in handler
    assert "TryBeginLessonSession" in handler
    assert "EndLessonSession" in handler
    assert "main/lesson_asset_storage_coordinator.cc" in runner
    assert "main/sd_fat_session_guard.cc" in runner


def test_transport_teardown_abandons_only_the_current_lesson_owner():
    app = read("main/application.cc")
    header = read("main/application.h")
    handler = read("main/lesson_handler.cc")

    assert "void RequestLessonStorageAbandonment();" in header
    assert "bool Application::AbandonLessonStorageSession()" in handler
    network_error = app[app.index("protocol_->OnNetworkError(") :]
    network_error = network_error[: network_error.index("protocol_->OnIncomingAudio(")]
    audio_closed = app[app.index("protocol_->OnAudioChannelClosed(") :]
    audio_closed = audio_closed[: audio_closed.index("protocol_->OnIncomingJson(")]
    reset = app[app.index("void Application::DoResetProtocol()") :]
    reset = reset[: reset.index("void Application::ResetProtocol()")]
    disconnected = app[app.index("void Application::HandleNetworkDisconnectedEvent()") :]
    disconnected = disconnected[: disconnected.index("void Application::HandleActivationDoneEvent()")]
    assert "AbandonLessonStorageSession" not in network_error
    assert "callback_protocol" in audio_closed
    passive = audio_closed[audio_closed.index("lesson_runtime_active_.load() && passive_ws_intent_.load()") :]
    passive = passive[: passive.index("connect_in_flight_.load()")]
    assert "StartPassiveLessonWebsocket" in passive
    assert "RequestLessonStorageAbandonment" not in passive
    assert "RequestLessonStorageAbandonment" in audio_closed
    assert "RequestLessonStorageAbandonment" in reset
    assert "RequestLessonStorageAbandonment" in disconnected
    worker = app[app.index("void Application::LessonMessageTask") : app.index("bool Application::SetDeviceState")]
    assert "AbandonLessonStorageSession" in worker
    assert "WorkerAcceptFrame" in worker and "WorkerApplyTerminal" in worker
    assert "EndLessonSession" in handler


def test_incoming_lesson_frame_uses_originating_transport_epoch():
    app = read("main/application.cc")
    header = read("main/application.h")
    protocol_header = read("main/protocols/protocol.h")
    websocket = read("main/protocols/websocket_protocol.cc")
    initialize = app[app.index("void Application::InitializeProtocol()") :]
    incoming = initialize[initialize.index("protocol_->OnIncomingJson(") :]
    incoming = incoming[: incoming.index("protocol_->OnIncomingAudio(", 1)] if "protocol_->OnIncomingAudio(" in incoming[1:] else incoming
    open_task = app[app.index("void Application::OpenChannelTask") :]
    open_task = open_task[: open_task.index("self->Schedule(")]
    websocket_open = websocket[websocket.index("bool WebsocketProtocol::OpenAudioChannel()") :]
    websocket_open = websocket_open[: websocket_open.index("std::string WebsocketProtocol::GetHelloMessage")]

    assert "void EnqueueLessonMessage(const cJSON* root, std::uint64_t transport_epoch);" in header
    assert "SetIncomingJsonTransportEpoch" in protocol_header
    assert "std::uint64_t transport_epoch" in protocol_header
    open_attempt = open_task[open_task.index("for (int attempt = 1;") :]
    assert "SetIncomingJsonTransportEpoch(" in open_attempt
    assert "lesson_transport_epoch_gate_.PublishedEpoch()" in open_attempt
    assert open_attempt.index("SetIncomingJsonTransportEpoch(") < open_attempt.index(
        "OpenAudioChannel()"
    )
    assert "const std::uint64_t callback_transport_epoch =" in websocket_open
    assert "IncomingJsonTransportEpoch()" in websocket_open
    assert "[this, callback_transport_epoch]" in websocket_open
    assert "on_incoming_json_(root, callback_transport_epoch);" in websocket_open
    assert "[this, display](const cJSON* root, std::uint64_t callback_transport_epoch)" in incoming
    assert "EnqueueLessonMessage(root, callback_transport_epoch);" in incoming
    enqueue = app[app.index("void Application::EnqueueLessonMessage") :]
    enqueue = enqueue[: enqueue.index("void Application::RequestLessonStorageAbandonment")]
    assert "PublishedEpoch()" not in enqueue
    assert "transport_epoch" in enqueue


def test_terminal_lesson_connect_watchdog_enqueues_worker_abandonment():
    app = read("main/application.cc")
    watchdog = app[app.index("void Application::HandleConnectWatchdog") :]
    watchdog = watchdog[: watchdog.index("void Application::ScheduleReconnect")]
    terminal = watchdog[watchdog.index("if (lesson_runtime_active_.load())") :]
    terminal = terminal[: terminal.index("\n    if (GetDeviceState() == kDeviceStateConnecting)")]

    assert "RequestLessonStorageAbandonment();" in terminal
    assert terminal.index("RequestLessonStorageAbandonment();") < terminal.index("return;")


def test_distinct_terminal_generations_are_not_suppressed_by_pending_control():
    app = read("main/application.cc")
    header = read("main/application.h")
    request = app[app.index("void Application::RequestLessonStorageAbandonment()") :]
    request = request[: request.index("void Application::LessonMessageTask")]
    worker = app[app.index("void Application::LessonMessageTask") :]
    worker = worker[: worker.index("bool Application::SetDeviceState")]

    publish = request.index("PublishTerminalEpoch()")
    pending_check = request.index("lesson_terminal_control_.Publish")
    assert publish < pending_check
    assert "lesson_transport_epoch_gate_.PublishedEpoch()" in worker
    assert "lesson_terminal_control_.FinishWorkerDrain" in worker
    assert "for (;;)" in worker
    assert "LessonTransportTerminalControl lesson_terminal_control_;" in header


def test_goal2_offline_contract_and_native_runners_are_colocated():
    destinations = (
        "scripts/hil_storage_identity_contract.py",
        "tests/test_hil_storage_identity_contract.py",
        "scripts/run_host_native_esp_build_identity_test.sh",
        "scripts/run_host_native_physical_sd_identity_test.sh",
        "scripts/run_host_native_sd_fat_session_guard_test.sh",
        "scripts/run_host_native_sd_guard_registry_lifecycle_test.sh",
        "scripts/run_host_native_lesson_storage_hil_status_contention_test.sh",
        "tests/native/esp_build_identity_host_test.cc",
        "tests/native/physical_sd_identity_host_test.cc",
        "tests/native/sd_fat_session_guard_host_test.cc",
        "tests/native/sd_guard_registry_lifecycle_host_test.cc",
        "tests/native/lesson_storage_hil_status_contention_host_test.cc",
    )
    assert all((ROOT / destination).is_file() for destination in destinations)
