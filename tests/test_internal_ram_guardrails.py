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


def test_websocket_open_workers_use_standard_internal_dram_stacks():
    source = read("main/application.cc")

    for signature, next_signature, task_name in [
        (
            "void Application::StartPassiveLessonWebsocket",
            "void Application::ContinueOpenAudioChannel",
            '"lesson_ws"',
        ),
        (
            "void Application::ContinueOpenAudioChannel",
            "void Application::OpenChannelTask",
            '"ws_open"',
        ),
        (
            "void Application::ContinueWakeWordInvoke",
            "void Application::FinishWakeWordInvoke",
            '"wake_ws_open"',
        ),
    ]:
        body = function_body(source, signature, next_signature)
        task_index = body.index(task_name)
        create_start = body.rfind("xTaskCreate", 0, task_index)
        create_call = body[create_start:body.index(";", task_index)]

        assert create_start != -1, task_name
        assert "xTaskCreate(&Application::OpenChannelTask" in create_call, task_name
        assert "xTaskCreateWithCaps" not in create_call, task_name
        assert "MALLOC_CAP_" not in create_call, task_name


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
        "void WebsocketProtocol::ParseServerHello",
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


def test_websocket_open_worker_uses_standard_task_self_delete():
    source = read("main/application.cc")
    worker_body = function_body(
        source,
        "void Application::OpenChannelTask",
        "void Application::ArmConnectWatchdog",
    )

    assert "vTaskDelete(nullptr);" in worker_body
    assert "vTaskDeleteWithCaps(nullptr);" not in worker_body


def test_passive_lesson_websocket_keeps_full_standard_internal_stack_size():
    source = read("main/application.cc")
    passive = function_body(
        source,
        "void Application::StartPassiveLessonWebsocket",
        "void Application::ContinueOpenAudioChannel",
    )

    assert '"lesson_ws", 8192' in passive
    assert '"lesson_ws", 6144' not in passive
    assert '"lesson_ws", 4096' not in passive
    assert "xTaskCreate(&Application::OpenChannelTask" in passive
    assert "xTaskCreateWithCaps(&Application::OpenChannelTask" not in passive
    assert "MALLOC_CAP_" not in passive[passive.index('"lesson_ws", 8192') :]


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
        ("void Application::StartHeartbeat", '"heartbeat_http"'),
    ]:
        start = source.index(signature)
        task_index = source.index(task_name, start)
        create_start = source.rfind("xTaskCreateWithCaps", start, task_index)
        create_end = source.index(") != pdPASS", task_index)
        create_call = source[create_start:create_end]

        assert create_start != -1, task_name
        assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in create_call, task_name
        assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" not in create_call, task_name


def test_speaking_timeout_uses_esp_timer_not_transient_task_stack():
    source = read("main/application.cc")
    header = read("main/application.h")

    assert "esp_timer_handle_t speaking_timeout_timer_" in header
    assert "static void SpeakingTimeoutTask" not in header
    assert "void Application::SpeakingTimeoutTask" not in source
    assert '"speaking_timeout"' not in source
    assert "esp_timer_start_once(speaking_timeout_timer_, kSpeakingTimeoutMs * 1000ULL)" in source
