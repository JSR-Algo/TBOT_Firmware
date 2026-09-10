"""Contracts keeping long lesson SD sync work off the Application task."""

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


def test_sync_to_sd_dispatches_to_single_flight_low_priority_worker():
    header = read("main/mcp_server.h")
    source = read("main/mcp_server.cc")
    dispatch = function_body(source, "void McpServer::DoToolCall")

    worker_branch = dispatch.index('tool_name == "self.lesson_assets.sync_to_sd"')
    generic_schedule = dispatch.index("app.Schedule", worker_branch)
    sync_dispatch = dispatch[worker_branch:generic_schedule]
    assert "HasLessonSession()" in sync_dispatch
    assert "HasMutation()" in sync_dispatch
    assert "ReplyError" in sync_dispatch
    assert sync_dispatch.index("HasLessonSession()") < sync_dispatch.index(
        "StartLessonAssetSyncTask"
    )
    assert sync_dispatch.index("HasMutation()") < sync_dispatch.index(
        "StartLessonAssetSyncTask"
    )
    assert "StartLessonAssetSyncTask" in sync_dispatch
    assert "return;" in dispatch[worker_branch:generic_schedule]

    assert "std::atomic<bool> lesson_asset_sync_in_flight_" in header
    assert "static void LessonAssetSyncTaskEntry(void* arg) noexcept" in header
    assert "static void LessonAssetSyncTaskBody(void* arg) noexcept" in header
    assert "__attribute__((noinline))" in header[
        header.index("LessonAssetSyncTaskBody") : header.index("LessonAssetSyncTaskBody") + 100
    ]

    starter = function_body(source, "bool McpServer::StartLessonAssetSyncTask")
    assert "lesson_asset_sync_in_flight_.exchange(true)" in starter
    assert "xTaskCreateWithCaps" in starter
    assert "&McpServer::LessonAssetSyncTaskEntry" in starter
    assert "&McpServer::LessonAssetSyncTaskBody" not in starter
    assert "tskIDLE_PRIORITY + 1" in starter
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in starter
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" not in starter
    assert starter.count("xTaskCreateWithCaps") == 1
    assert "lesson_asset_sync_in_flight_.store(false)" in starter

    entry = function_body(source, "void McpServer::LessonAssetSyncTaskEntry")
    assert "LessonAssetSyncTaskBody(arg);" in entry
    assert "abort();" in entry
    assert entry.index("LessonAssetSyncTaskBody(arg);") < entry.index("abort();")
    assert "tool->Call" not in entry
    assert "catch" not in entry

    worker = function_body(source, "void McpServer::LessonAssetSyncTaskBody")
    assert "tool->Call" in worker
    completion = function_body(source, "void McpServer::PollLessonAssetSyncCompletion")
    assert "ReplyResult" in completion
    assert "ReplyError" in completion
    assert "lesson_asset_sync_in_flight_.store(false" in completion
    assert "PublishLessonAssetSyncCompletion" in worker
    assert "vTaskDeleteWithCaps(nullptr)" not in worker
    assert entry.index("LessonAssetSyncTaskBody(arg);") < entry.index("vTaskDeleteWithCaps(nullptr)") < entry.index("abort();")


def test_sync_worker_body_has_nonthrowing_failsafe_cleanup_boundary():
    source = read("main/mcp_server.cc")
    worker = function_body(source, "void McpServer::LessonAssetSyncTaskBody")

    assert "lesson asset sync worker failed outside tool boundary" in worker
    assert "lesson_asset_sync_in_flight_.store(false" not in worker
    assert worker.count("esp_task_wdt_delete(nullptr)") == 2
    assert "vTaskDeleteWithCaps(nullptr)" not in worker
    assert worker.count("PublishLessonAssetSyncCompletion") == 2

    failsafe = worker[worker.index("lesson asset sync worker failed outside tool boundary") :]
    publication = failsafe.index("PublishLessonAssetSyncCompletion")
    watchdog_delete = failsafe.index("esp_task_wdt_delete(nullptr)")
    assert publication < watchdog_delete
    assert "request_context" in failsafe[:watchdog_delete]
    assert "Schedule" not in worker


def test_each_asset_sync_worker_owns_one_complete_quiet_interval():
    source = read("main/mcp_server.cc")
    starter = function_body(source, "bool McpServer::StartLessonAssetSyncTask")
    worker = function_body(source, "void McpServer::LessonAssetSyncTaskBody")

    assert "lesson_asset_sync_in_flight_.exchange(true)" in starter
    assert starter.count("BeginLessonAssetSyncQuiet") == 1
    quiet_begin = starter.index("BeginLessonAssetSyncQuiet")
    context_allocation = starter.index("auto* context = new (std::nothrow)")
    task_creation = starter.index("xTaskCreateWithCaps")
    assert quiet_begin < context_allocation < task_creation

    allocation_failure = starter[
        starter.index("if (context == nullptr)") : task_creation
    ]
    assert "PublishLessonAssetSyncCompletion" in allocation_failure
    assert "lesson_asset_sync_in_flight_.store(false" not in allocation_failure

    creation_failure = starter[
        task_creation : starter.index("return true;", task_creation)
    ]
    assert "PublishLessonAssetSyncCompletion" in creation_failure
    assert "lesson_asset_sync_in_flight_.store(false" not in creation_failure

    assert "context->tool->Call(context->arguments, request_context)" in worker
    assert worker.index("context->tool->Call") < worker.index("PublishLessonAssetSyncCompletion")


def test_sync_worker_owns_application_audio_quiet_lifecycle():
    app_header = read("main/application.h")
    app_source = read("main/application.cc")
    mcp_source = read("main/mcp_server.cc")

    assert "bool BeginLessonAssetSyncQuiet();" in app_header
    assert "void EndLessonAssetSyncQuiet();" in app_header
    assert "std::atomic<bool> lesson_asset_sync_quiet_" in app_header
    assert "esp_timer_handle_t lesson_asset_sync_wake_rearm_timer_" in app_header
    assert "void ScheduleLessonAssetSyncWakeRearm();" in app_header
    assert "void ScheduleLessonAssetSyncWakeRearm(uint64_t delay_us);" in app_header

    begin = function_body(app_source, "bool Application::BeginLessonAssetSyncQuiet")
    assert "esp_timer_stop(lesson_asset_sync_wake_rearm_timer_)" in begin
    for guard in (
        "lesson_runtime_active_.load()",
        "connect_in_flight_.load()",
        "reset_pending_.load()",
    ):
        assert guard in begin
    assert "tts_audio_accepting_.store(false)" in begin
    assert "audio_service_.EnableVoiceProcessing(false)" in begin
    assert "audio_service_.EnableWakeWordDetection(false)" in begin
    assert "audio_service_.ResetDecoder()" in begin

    end = function_body(app_source, "void Application::EndLessonAssetSyncQuiet")
    assert "lesson_asset_sync_quiet_.exchange(false)" in end
    assert "GetDeviceState() == kDeviceStateIdle" in end
    assert "IsDeviceClaimed()" in end
    assert "ScheduleLessonAssetSyncWakeRearm();" in end
    assert "RearmClaimedIdleWakeWord();" not in end
    assert "audio_service_.EnableWakeWordDetection(true)" not in end

    schedule = function_body(
        app_source, "void Application::ScheduleLessonAssetSyncWakeRearm"
    )
    assert "1500ULL * 1000ULL" in schedule

    delayed_schedule = function_body(
        app_source,
        "void Application::ScheduleLessonAssetSyncWakeRearm(uint64_t delay_us)",
    )
    assert "esp_timer_start_once" in delayed_schedule
    assert "delay_us" in delayed_schedule
    assert "self->Schedule" in delayed_schedule
    assert "self->RearmClaimedIdleWakeWord();" in delayed_schedule

    rearm = function_body(app_source, "void Application::RearmClaimedIdleWakeWord")
    # A connected passive lesson WebSocket keeps this intent true while the
    # robot is idle, so only an unopened/in-flight passive socket may suppress
    # the post-sync wake-word rearm.
    compact_rearm = "".join(rearm.split())
    assert (
        "passive_ws_intent_.load()&&"
        "(protocol_==nullptr||!protocol_->IsAudioChannelOpened())"
    ) in compact_rearm

    claim_finish = function_body(
        app_source, "void Application::CompleteClaimProtocolActivation"
    )
    claim_rearm = claim_finish[
        claim_finish.index("if (!audio_service_.Start())") :
        claim_finish.index("StartHeartbeat();")
    ]
    assert "lesson_asset_sync_quiet_.load()" in claim_rearm

    starter = function_body(mcp_source, "bool McpServer::StartLessonAssetSyncTask")
    assert "ScheduleAndWait" in starter
    assert "BeginLessonAssetSyncQuiet" in starter
    assert starter.index("BeginLessonAssetSyncQuiet") < starter.index("xTaskCreateWithCaps")
    assert starter.count("PublishLessonAssetSyncCompletion") == 2

    worker = function_body(mcp_source, "void McpServer::PollLessonAssetSyncCompletion")
    assert worker.count("EndLessonAssetSyncQuiet") == 1
    assert worker.index("EndLessonAssetSyncQuiet") < worker.index(
        "lesson_asset_sync_in_flight_.store(false"
    )
    assert worker.index("EndLessonAssetSyncQuiet") < worker.index("ReplyResult")
    assert worker.index("EndLessonAssetSyncQuiet") < worker.index("ReplyError")


def test_sync_quiet_admits_only_idle_or_voice_silent_passive_listening():
    source = read("main/application.cc")
    begin = function_body(source, "bool Application::BeginLessonAssetSyncQuiet")

    assert "const DeviceState state = GetDeviceState();" in begin
    compact_begin = "".join(begin.split())
    assert (
        "constboolpassive_listening="
        "state==kDeviceStateListening&&!chat_cleanup_enabled_.load()&&passive_ws_intent_.load()&&"
        "!online_intent_.load()&&!microphone_uplink_authorized_.load()&&!IsVoiceDetected();"
    ) in compact_begin
    assert "state != kDeviceStateIdle && !passive_listening" in begin

    passive = begin[
        begin.index("if (passive_listening)") :
        begin.index("tts_audio_accepting_.store(false)")
    ]
    required = (
        "lesson_idle_repaint_suppressed_.store(true)",
        "protocol_->SendStopListening()",
        "listening_started_ms_.store(0)",
        "last_listening_activity_ms_.store(0)",
        "audio_service_.EnableVoiceProcessing(false)",
        "audio_service_.EnableWakeWordDetection(false)",
        "SetDeviceState(kDeviceStateIdle)",
    )
    positions = tuple(passive.index(statement) for statement in required)
    assert positions == tuple(sorted(positions))


def test_sync_quiet_blocks_voice_transitions_but_not_mcp_dispatch():
    source = read("main/application.cc")

    start = function_body(source, "void Application::HandleStartListeningEvent")
    wake = function_body(source, "void Application::HandleWakeWordDetectedEvent")
    direct_wake = function_body(source, "void Application::WakeWordInvoke")
    assert "lesson_asset_sync_quiet_.load()" in start
    assert "lesson_asset_sync_quiet_.load()" in wake
    assert "lesson_asset_sync_quiet_.load()" in direct_wake

    incoming = function_body(source, "void Application::DispatchIncomingJson")
    assert "lesson_asset_sync_quiet_.load()" in incoming
    assert 'strcmp(type->valuestring, "tts") == 0' in incoming
    assert 'strcmp(type->valuestring, "stt") == 0' in incoming
    quiet_guard = incoming[
        incoming.index("lesson_asset_sync_quiet_.load()") :
        incoming.index('if (strcmp(type->valuestring, "tts") == 0)')
    ]
    assert "return;" in quiet_guard
    assert 'strcmp(type->valuestring, "mcp")' not in quiet_guard


def test_sync_quiet_defers_passive_and_general_reconnect_tls_work():
    source = read("main/application.cc")
    reconnect = function_body(source, "void Application::HandleReconnectTick")

    passive = reconnect[
        reconnect.index("if (reconnect_passive_.exchange(false))") :
        reconnect.index("if (lesson_runtime_active_.load())")
    ]
    assert "lesson_asset_sync_quiet_.load()" in passive
    assert "SchedulePassiveLessonReconnect();" in passive
    assert passive.index("lesson_asset_sync_quiet_.load()") < passive.index(
        "StartPassiveLessonWebsocket();"
    )

    assert reconnect.count("lesson_asset_sync_quiet_.load()") >= 2
    general_quiet = reconnect.rindex("lesson_asset_sync_quiet_.load()")
    assert "ScheduleReconnect(" in reconnect[general_quiet:]
    assert general_quiet < reconnect.index("ContinueOpenAudioChannel(reconnect_mode_)")

def test_generic_sync_keeps_response_and_verified_file_tracking_bounded():
    source = read("main/mcp_server.cc")
    sync_start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
    sync_end = source.index(
        '\n\n    AddUserOnlyTool("self.assets.set_download_url"', sync_start
    )
    sync_body = source[sync_start:sync_end]
    record_start = source.index("struct VerifiedLessonAssetFile")
    record_end = source.index("};", record_start)
    record = source[record_start:record_end]

    assert "MakeCheckedCJsonArray" not in sync_body
    assert '"files"' not in sync_body
    assert "const char* sha256" in record
    assert "const char* destination" in record
    compact = " ".join(sync_body.split())
    assert "std::strcmp( verified_file.sha256, asset.sha256) == 0" in compact


def test_download_buffer_prefers_psram_with_internal_fallback():
    source = read("main/lesson_asset_http_transfer.cc")
    allocation = function_body(source, "void* AllocateLessonAssetDownloadBuffer")

    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in allocation
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in allocation
    assert source.index("AllocateLessonAssetDownloadBuffer()") < source.index(
        "ScopedHeapAllocation buffer_allocation"
    )
