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
                return text[brace:index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_audio_start_returns_checked_complete_worker_result():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")
    start = function_body(source, "bool AudioService::Start()")
    checked = function_body(source, "bool AudioService::StartWorkers")

    assert "bool Start();" in header
    assert "bool StartWorkers(uint32_t attempt);" in header
    assert "bool HasCompleteWorkerSet();" in header
    assert "return StartWorkers(1);" in start
    assert "AudioWorkerStartTransaction::StartOnce" in checked
    assert "HasCompleteWorkerSet()" in checked
    complete = checked.index("const bool complete = HasCompleteWorkerSet();")
    incomplete = checked.index("if (!complete)", complete)
    timer = checked.index("if (esp_timer_start_periodic(", incomplete)
    timer_result = checked.index("!= ESP_OK", timer)
    publish = checked.index(
        "service_running_.store(true, std::memory_order_release)", timer_result
    )
    assert complete < incomplete < timer < timer_result < publish

    incomplete_branch = function_body(checked, "if (!complete)")
    assert "const bool result = RollbackWorkerStart();" in incomplete_branch
    assert "return result;" in incomplete_branch

    timer_failure = function_body(checked, "if (esp_timer_start_periodic(")
    assert "!= ESP_OK" in timer_failure
    assert "const bool result = RollbackWorkerStart();" in timer_failure
    assert "return result;" in timer_failure
    assert checked.index("RollbackWorkerStart();", timer) < publish


def test_each_audio_worker_creation_is_checked_and_opus_uses_psram_stack():
    source = read("main/audio/audio_service.cc")
    create = function_body(source, "bool AudioService::CreateAudioWorker")
    opus_start = create.index("case AudioWorker::kOpusCodec")
    opus_end = create.index("case AudioWorker::kAudioInput", opus_start)
    opus = create[opus_start:opus_end]

    assert create.count("created == pdPASS && task_handle != nullptr") == 3
    assert "xTaskCreateWithCaps" in opus
    assert '"opus_codec", kOpusCodecTaskStackBytes, this' in opus
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in opus
    assert "vTaskDeleteWithCaps(NULL);" in opus
    assert "vTaskDelete(NULL);" not in opus


def test_creation_failure_logs_safe_internal_heap_diagnostics():
    source = read("main/audio/audio_service.cc")
    failure = function_body(source, "void AudioService::LogWorkerCreateFailure")

    assert "worker_name" in failure
    assert "created" in failure
    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)" in failure
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)" in failure
    for forbidden in ("ssid", "password", "credential", "audio payload"):
        assert forbidden not in failure.lower()


def test_provisioning_keeps_internal_heap_free_for_ble_then_reserves_network_headroom():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")
    blufi = read("main/boards/common/blufi.cpp")

    assert "ReserveWifiStationAssociationStack" not in header
    assert "ReleaseWifiStationAssociationStack" not in header
    assert "wifi_station_stack_reservation_" not in header
    assert "bool ReserveWifiPostAssociationNetworkHeadroom(WifiProvisioningToken token);" in header
    assert "kPostProvisioningNetworkHeadroomBytes" in header
    assert "wifi_station_network_headroom_reservation_" in header
    assert "ReserveWifiStationAssociationStack" not in source
    assert "ReleaseWifiStationAssociationStack" not in source

    wifi_board = read("main/boards/common/wifi_board.cc")
    start_config = function_body(wifi_board, "WifiBoard::WifiConfigEntryResult WifiBoard::StartWifiConfigMode")
    begin = start_config.index("BeginWifiProvisioning")
    restart_ble = start_config.index("RestartForSetup", begin)
    assert begin < restart_ble
    assert "ReserveWifiStationAssociationStack" not in start_config

    connect = function_body(blufi, "void Blufi::StartStationConnectFromCredentials")
    release_ble = connect.index("ReleaseBleForStationAssociation(generation)")
    reserve_network = connect.index("ReserveWifiPostAssociationNetworkHeadroom", release_ble)
    start_station = connect.index("wifi.StartStationWithCredentialsIfScanIdle", reserve_network)
    assert release_ble < reserve_network < start_station


def test_network_headroom_is_released_at_rearm_or_before_ble_restore():
    source = read("main/audio/audio_service.cc")
    blufi = read("main/boards/common/blufi.cpp")

    rearm = function_body(source, "bool AudioService::EndWifiProvisioningAndRearm")
    start = rearm.index("AudioWorkerStartTransaction::Rearm")
    release_network = rearm.index("ReleaseWifiPostAssociationNetworkHeadroom(token)", start)
    assert start < release_network
    assert "reserved_stack_released" not in rearm

    restore = function_body(blufi, "void Blufi::RestoreBleAfterStationFailure")
    release_network = restore.index(
        "ReleaseWifiPostAssociationNetworkHeadroom(provisioning_token)"
    )
    init = restore.index("InitWithLifecycleOwned()", release_network)
    assert release_network < init


def test_failed_start_stops_waits_and_leaves_service_stopped():
    source = read("main/audio/audio_service.cc")
    rollback = function_body(source, "bool AudioService::RollbackWorkerStart")

    assert rollback.index("Stop();") < rollback.index(
        "WaitForServiceWorkersStopped(kProvisioningWorkerStopTimeoutMs)"
    )
    assert "service_stopped_.store(true, std::memory_order_release)" in rollback
    assert "service_running_.store(false, std::memory_order_release)" in rollback
    assert "return false;" in rollback


def test_provisioning_rearm_consumes_once_then_retries_once_after_reclaim():
    source = read("main/audio/audio_service.cc")
    rearm = function_body(source, "bool AudioService::EndWifiProvisioningAndRearm")

    lifecycle = rearm.index("wake_word_lifecycle_.EndProvisioningAndRearm(token)")
    consume = rearm.index("provisioning_audio_workers_.Consume(token.generation)")
    rejected = rearm.index("if (!completion.accepted)", consume)
    stopped = rearm.index("if (!completion.restart_required)", rejected)
    retry = rearm.index("AudioWorkerStartTransaction::Rearm")
    assert lifecycle < consume < rejected < stopped < retry
    rejected_branch = function_body(rearm, "if (!completion.accepted)")
    stopped_branch = function_body(rearm, "if (!completion.restart_required)")
    assert "return false;" in rejected_branch
    assert "return true;" in stopped_branch
    for guard in (rejected_branch, stopped_branch):
        assert "AudioWorkerStartTransaction::Rearm" not in guard
        assert "vTaskDelay" not in guard
        assert "StartWorkers" not in guard
    assert "vTaskDelay(pdMS_TO_TICKS(delay_ms))" in rearm
    assert "return StartWorkers(attempt);" in rearm
    assert "return rearmed;" in rearm


def test_failed_afe_detection_task_creation_releases_partial_pipeline_before_retry():
    source = read("main/audio/wake_words/afe_wake_word.cc")
    initialize = function_body(source, "bool AfeWakeWord::Initialize")
    failure = function_body(initialize, "if (detection_created != pdPASS)")

    assert "if (afe_config == nullptr)" in initialize
    assert "if (afe_iface_ == nullptr)" in initialize
    assert "afe_config_free(afe_config);" in initialize
    assert "if (afe_data_ == nullptr)" in initialize
    assert "afe_iface_->destroy(afe_data_);" in failure
    assert "afe_data_ = nullptr;" in failure
    assert failure.index("afe_iface_->destroy(afe_data_);") < failure.index("return false;")


def test_psram_afe_detection_stack_uses_matching_delete_api():
    source = read("main/audio/wake_words/afe_wake_word.cc")
    initialize = function_body(source, "bool AfeWakeWord::Initialize")
    task_start = initialize.index("xTaskCreateWithCaps")
    task_end = initialize.index('}, "audio_detection"', task_start)
    task = initialize[task_start:task_end]

    assert "vTaskDeleteWithCaps(nullptr);" in task
    assert "vTaskDelete(NULL);" not in task


def test_ci_runs_audio_rearm_transaction_gates():
    workflow = read(".github/workflows/build.yml")
    assert "scripts/run_host_native_audio_worker_start_transaction_test.sh" in workflow
    assert "tests/test_audio_rearm_transaction_contract.py" in workflow
