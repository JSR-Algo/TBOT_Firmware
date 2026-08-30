from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_heap_stats_reports_largest_internal_free_block():
    source = read("main/system_info.cc")

    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)" in source
    assert "largest_free_block" in source


def test_esp32s3_malloc_prefers_psram_for_allocations_over_512_bytes():
    sdkconfig = read("sdkconfig.defaults.esp32s3")

    assert "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512" in sdkconfig
    assert "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048" not in sdkconfig


def test_websocket_open_requests_share_one_persistent_internal_dram_stack():
    source = read("main/application.cc")
    constructor = function_body(
        source,
        "Application::Application",
        "Application::~Application",
    )
    assert constructor.count("xTaskCreateStatic(") == 2
    assert "&Application::OpenChannelTask" in constructor
    assert '"lesson_ws"' in constructor
    assert "&Application::LessonMessageTask" in constructor
    assert '"lesson_worker"' in constructor
    assert "xQueueCreateStatic(1, sizeof(void*)" in " ".join(constructor.split())
    assert "DRAM_ATTR StaticQueue_t open_channel_queue_buffer;" in source
    assert "DRAM_ATTR void* open_channel_queue_storage[1];" in source
    assert "DRAM_ATTR StackType_t open_channel_task_stack[kOpenChannelWorkerStackDepth]" in source
    assert "xTaskCreateWithCaps" not in constructor

    for signature, next_signature in [
        (
            "void Application::StartPassiveLessonWebsocket",
            "void Application::ContinueOpenAudioChannel",
        ),
        (
            "void Application::ContinueOpenAudioChannel",
            "bool Application::StartOpenChannelWorker",
        ),
        (
            "void Application::ContinueWakeWordInvoke",
            "void Application::FinishWakeWordInvoke",
        ),
    ]:
        body = function_body(source, signature, next_signature)
        assert "StartOpenChannelWorker(ctx)" in body
        assert "xTaskCreate" not in body


def test_websocket_open_worker_stack_is_safe_for_indirect_nvs_reads():
    application = read("main/application.cc")
    websocket = read("main/protocols/websocket_protocol.cc")
    worker_body = function_body(
        application,
        "void Application::OpenChannelTask",
        "void Application::SetListeningMode",
    )
    open_body = function_body(
        websocket,
        "bool WebsocketProtocol::OpenAudioChannel",
        "bool WebsocketProtocol::ParseServerHello",
    )
    refresh_body = function_body(
        websocket,
        "void WebsocketProtocol::RefreshSettings",
        "bool WebsocketProtocol::IsAllowedUnclaimedPublicLessonMessage",
    )

    assert "protocol_->OpenAudioChannel()" in worker_body
    assert "RefreshSettings();" in open_body
    assert 'Settings settings("websocket", false);' in refresh_body
    assert "settings.GetString(" in refresh_body
    assert "settings.GetInt(" in refresh_body


def test_websocket_open_worker_stays_alive_for_reconnect_reuse():
    source = read("main/application.cc")
    worker_body = function_body(
        source,
        "void Application::OpenChannelTask",
        "void Application::ArmConnectWatchdog",
    )

    assert "for (;;)" in worker_body
    assert "xQueueReceive(open_channel_queue" in worker_body
    assert "portMAX_DELAY" in worker_body
    assert "vTaskDeleteWithCaps(nullptr);" not in worker_body


def test_passive_lesson_websocket_keeps_full_persistent_internal_stack_size():
    source = read("main/application.cc")
    constructor = function_body(
        source,
        "Application::Application",
        "Application::~Application",
    )

    assert "kOpenChannelWorkerStackDepth = 8192" in source
    assert "open_channel_task_stack[kOpenChannelWorkerStackDepth]" in source
    assert "kOpenChannelWorkerStackDepth = 6144" not in source
    assert "kOpenChannelWorkerStackDepth = 4096" not in source
    assert "xTaskCreateStatic(" in constructor
    assert "&Application::OpenChannelTask" in constructor


def test_transient_http_workers_use_internal_dram_stacks():
    """HTTP/NVS workers must NOT use SPIRAM stacks.

    Live BluFi claim crash (2026-07-11): claim_fetch on SPIRAM panicked with
    esp_task_stack_is_sane_cache_disabled when NVS/TLS disabled the flash cache.
    Stacks for these workers must stay in internal DRAM.
    """
    source = read("main/application.cc")

    for signature, task_name in [
        ("bool Application::DispatchPendingTbotClaimFetch", '"claim_fetch"'),
        ("void Application::MaybeDispatchDeferredCloudRelease", '"cloud_release"'),
    ]:
        start = source.index(signature)
        task_index = source.index(task_name, start)
        create_start = source.rfind("xTaskCreateWithCaps", start, task_index)
        create_end = source.index(") != pdPASS", task_index)
        create_call = source[create_start:create_end]

        assert create_start != -1, task_name
        assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in create_call, task_name
        assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" not in create_call, task_name

    # Heartbeat is also internal DRAM, but its stack is reserved statically so
    # wake-triggered reconnects cannot fail after TLS fragments the heap.
    assert "DRAM_ATTR StackType_t heartbeat_task_stack[kHeartbeatWorkerStackDepth]" in source
    heartbeat_start = function_body(
        source, "void Application::StartHeartbeat", "void Application::StopHeartbeat"
    )
    assert "xTaskCreateStatic(" in heartbeat_start
    assert "heartbeat_task_stack" in heartbeat_start


def test_websocket_close_callback_defers_nvs_dependent_work_to_application_task():
    """Transport callbacks can run on a PSRAM-backed network task stack."""
    source = read("main/application.cc")
    callback_start = source.index("protocol_->OnAudioChannelClosed")
    schedule_start = source.index("Schedule([", callback_start)
    callback_end = source.index("protocol_->OnIncomingJson", schedule_start)
    callback_prefix = source[callback_start:schedule_start]
    scheduled_close = source[schedule_start:callback_end]

    assert "tts_audio_accepting_.store(false);" in callback_prefix
    assert "ShouldKeepManagementHeartbeat()" not in callback_prefix
    assert "StartHeartbeat();" not in callback_prefix
    assert "DispatchDeviceHeartbeat();" not in callback_prefix
    assert "StopHeartbeat();" not in callback_prefix
    assert "SetPowerSaveLevel" not in callback_prefix

    generation_capture = source.rfind(
        "const uint64_t callback_protocol_generation", 0, callback_start
    )
    assert generation_capture != -1
    assert "callback_protocol_generation" in callback_prefix
    assert "ProtocolLifetimeMatches(" in scheduled_close
    guard = scheduled_close.index("ProtocolLifetimeMatches(")
    for effect in (
        "ShouldKeepManagementHeartbeat()",
        "StartHeartbeat();",
        "DispatchDeviceHeartbeat();",
        "StopHeartbeat();",
        "SetPowerSaveLevel(PowerSaveLevel::LOW_POWER)",
        "RequestLessonStorageAbandonment();",
    ):
        assert effect in scheduled_close
        assert guard < scheduled_close.index(effect)


def test_speaking_timeout_uses_esp_timer_not_transient_task_stack():
    source = read("main/application.cc")
    header = read("main/application.h")

    assert "esp_timer_handle_t speaking_timeout_timer_" in header
    assert "static void SpeakingTimeoutTask" not in header
    assert "void Application::SpeakingTimeoutTask" not in source
    assert '"speaking_timeout"' not in source
    assert "esp_timer_start_once(speaking_timeout_timer_, kSpeakingTimeoutMs * 1000ULL)" in source
