"""Phase heap telemetry must distinguish local lows from the boot lifetime minimum."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SYSTEM_HEADER = ROOT / "main/system_info.h"
SYSTEM_SOURCE = ROOT / "main/system_info.cc"
APPLICATION_SOURCE = ROOT / "main/application.cc"
LESSON_SOURCE = ROOT / "main/lesson_handler.cc"


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


def assert_monitored_phase(body: str, operation: str, checkpoint: str) -> None:
    start = body.index("SystemInfo::StartHeapPhaseMonitor();")
    operation_index = body.index(operation, start)
    checkpoint_index = body.index(
        f'SystemInfo::PrintHeapCheckpoint("{checkpoint}");', operation_index
    )
    stop = body.index("SystemInfo::StopHeapPhaseMonitor();", checkpoint_index)
    assert start < operation_index < checkpoint_index < stop


def test_system_info_exposes_structured_phase_heap_checkpoint_without_replacing_legacy_stats():
    header = SYSTEM_HEADER.read_text(encoding="utf-8")
    source = SYSTEM_SOURCE.read_text(encoding="utf-8")
    checkpoint = function_body(source, "void SystemInfo::PrintHeapCheckpoint")

    assert "static void PrintHeapStats();" in header
    assert "void SystemInfo::PrintHeapStats()" in source
    assert "static void PrintHeapCheckpoint(const char* phase);" in header
    assert "static void StartHeapPhaseMonitor();" in header
    assert "static void StopHeapPhaseMonitor();" in header
    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)" in checkpoint
    assert "heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)" in checkpoint
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)" in checkpoint
    assert "heap_caps_get_free_size(MALLOC_CAP_SPIRAM)" in checkpoint
    for field in (
        "phase=%s",
        "internal_free=%u",
        "lifetime_min_internal=%u",
        "phase_min_internal=%u",
        "largest_internal=%u",
        "psram_free=%u",
    ):
        assert field in checkpoint


def test_activation_wraps_high_risk_boot_phases_with_local_minimum_monitors():
    source = APPLICATION_SOURCE.read_text(encoding="utf-8")
    activation = function_body(source, "void Application::ActivationTask")
    assets = function_body(source, "void Application::CheckAssetsVersion")

    assert 'SystemInfo::PrintHeapCheckpoint("activation.start");' in activation
    assert_monitored_phase(activation, "CheckNewVersion();", "ota_check.complete")
    assert_monitored_phase(
        activation, "RefreshWebsocketUrlFromConfigFetch();", "config_fetch.complete"
    )
    assert_monitored_phase(
        activation, "audio_service_.PrewarmWakeWord(wake_word_prewarm_token);", "afe_prewarm.complete"
    )
    assert_monitored_phase(activation, "InitializeProtocol();", "protocol_init.complete")
    assert "PrewarmWakeWord" not in assets
    assert activation.count('SystemInfo::PrintHeapCheckpoint("activation.complete");') == 2


def test_claimed_activation_finishes_boot_http_before_afe_prewarm_and_protocol():
    source = APPLICATION_SOURCE.read_text(encoding="utf-8")
    activation = function_body(source, "void Application::ActivationTask")

    assets = activation.index("CheckAssetsVersion();", activation.index("if (!IsDeviceClaimed())"))
    ota = activation.index("CheckNewVersion();", assets)
    config = activation.index("RefreshWebsocketUrlFromConfigFetch();", ota)
    prewarm = activation.index("audio_service_.PrewarmWakeWord(wake_word_prewarm_token);", config)
    protocol = activation.index("InitializeProtocol();", prewarm)
    activation_done = activation.index(
        "xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);", protocol
    )

    assert assets < ota < config < prewarm < protocol < activation_done


def test_unclaimed_activation_returns_before_afe_prewarm_and_protocol():
    source = APPLICATION_SOURCE.read_text(encoding="utf-8")
    activation = function_body(source, "void Application::ActivationTask")

    unclaimed_start = activation.index("if (!IsDeviceClaimed())")
    unclaimed_return = activation.index("return;", unclaimed_start)
    unclaimed_branch = activation[unclaimed_start:unclaimed_return]

    assert "CheckAssetsVersion();" in unclaimed_branch
    assert "PrewarmWakeWord" not in unclaimed_branch
    assert "InitializeProtocol" not in unclaimed_branch


def test_claimed_activation_interrupted_by_setup_or_audio_test_skips_afe_prewarm():
    source = APPLICATION_SOURCE.read_text(encoding="utf-8")
    activation = function_body(source, "void Application::ActivationTask")

    config = activation.index("RefreshWebsocketUrlFromConfigFetch();")
    state = activation.index("const auto activation_state = GetDeviceState();", config)
    prewarm = activation.index("audio_service_.PrewarmWakeWord(wake_word_prewarm_token);", state)
    protocol = activation.index("InitializeProtocol();", prewarm)
    prewarm_guard = activation[state:prewarm]

    assert state < prewarm < protocol
    assert "if (IsDeviceClaimed() &&" in prewarm_guard
    assert "activation_state != kDeviceStateWifiConfiguring" in prewarm_guard
    assert "activation_state != kDeviceStateAudioTesting" in prewarm_guard


def test_local_minimum_monitor_preserves_lifetime_snapshot_until_checkpoint_is_logged():
    source = SYSTEM_SOURCE.read_text(encoding="utf-8")
    start = function_body(source, "void SystemInfo::StartHeapPhaseMonitor")
    stop = function_body(source, "void SystemInfo::StopHeapPhaseMonitor")
    checkpoint = function_body(source, "void SystemInfo::PrintHeapCheckpoint")

    snapshot = "heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)"
    assert snapshot in start
    assert "heap_caps_monitor_local_minimum_free_size_start()" in start
    assert "heap_caps_monitor_local_minimum_free_size_stop()" in stop
    assert "phase_monitor_lifetime_min_internal" in checkpoint
    assert checkpoint.index("phase_monitor_lifetime_min_internal") < checkpoint.index("ESP_LOGI")


def test_lesson_render_records_a_local_minimum_before_emitting_the_step_ack():
    source = LESSON_SOURCE.read_text(encoding="utf-8")
    handler = function_body(source, "void Application::HandleLessonMessage")

    render_start = handler.index("const int64_t render_start_us")
    monitor_start = handler.index("SystemInfo::StartHeapPhaseMonitor();", render_start)
    fetch = handler.index("FetchLessonImage(poster_src)", monitor_start)
    checkpoint = handler.index(
        'SystemInfo::PrintHeapCheckpoint("lesson_render.complete");', fetch
    )
    monitor_stop = handler.index("SystemInfo::StopHeapPhaseMonitor();", checkpoint)
    ack = handler.index("emit_ack(root, sequence, rendered", monitor_stop)
    assert render_start < monitor_start < fetch < checkpoint < monitor_stop < ack
