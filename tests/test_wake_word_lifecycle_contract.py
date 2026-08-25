from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_audio_service_routes_every_wake_word_access_through_controller_or_feed_lease():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")

    assert "WakeWordLifecycleController wake_word_lifecycle_;" in header
    assert "std::atomic<WakeWord*> wake_word_feed_target_" in header
    assert "wake_word_lifecycle_.TryAcquireFeed()" in source
    assert "wake_word_lifecycle_.TryAcquireAccess()" in source
    assert "wake_word_lifecycle_.TryAcquirePrewarm(token)" in source
    assert "wake_word_lifecycle_.BeginProvisioningAndQuiesce" in source
    assert "const std::string& GetLastWakeWord()" not in header
    enable = source[source.index("void AudioService::EnableWakeWordDetection"):source.index("AudioService::WakeWordPrewarmToken")]
    accepted = enable.index("if (wake_word_lifecycle_.SetRunning(true, lease.generation()))")
    publish = enable.index("wake_word_feed_target_.store(wake_word_.get()", accepted)
    event = enable.index("xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING)", publish)
    assert accepted < publish < event

    provision = source[source.index("AudioService::WifiProvisioningBeginResult AudioService::BeginWifiProvisioning"):source.index("bool AudioService::EndWifiProvisioningAndRearm")]
    quiesced = provision.index("BeginProvisioningAndQuiesce")
    assert provision.index("wake_word_feed_target_.store(nullptr", quiesced) > quiesced
    assert provision.index("xEventGroupClearBits", quiesced) > quiesced


def test_begin_failure_after_quiescence_stays_fail_closed_without_rearm():
    source = read("main/audio/audio_service.cc")
    wifi = read("main/boards/common/wifi_board.cc")
    provision = source[
        source.index("AudioService::WifiProvisioningBeginResult AudioService::BeginWifiProvisioning"):
        source.index("bool AudioService::EndWifiProvisioningAndRearm")
    ]

    timeout = provision[provision.index("if (!wake_word_->Shutdown(5000))"):]
    timeout = timeout[:timeout.index("wake_word_.reset();")]
    stale = provision[provision.index("if (!wake_word_lifecycle_.FinishProvisioningReset") :]
    stale = stale[:stale.index("return {provisioning_token")]
    for failure in (timeout, stale):
        assert "return {{}, false};" in failure
        assert "EndProvisioningAndRearm" not in failure

    start = wifi[wifi.index("void WifiBoard::StartWifiConfigMode("):]
    start = start[:wifi.index("void WifiBoard::EnterWifiConfigMode()") - wifi.index("void WifiBoard::StartWifiConfigMode(")]
    begin_failure = start[start.index("if (!begin_result)"):start.index("const auto provisioning_token")]
    assert "if (begin_result.rollback_complete)" in begin_failure
    assert "RollbackWifiConfigEntry(preparation)" in begin_failure


def test_concrete_wake_words_ack_shutdown_and_preserve_borrowed_models():
    for stem in ("afe_wake_word", "custom_wake_word", "esp_wake_word"):
        header = read(f"main/audio/wake_words/{stem}.h")
        source = read(f"main/audio/wake_words/{stem}.cc")
        assert "bool Shutdown(uint32_t timeout_ms)" in header
        assert "owns_models_" in header
        assert "owns_models_ = true" in source
        assert "owns_models_ = false" in source

    afe = read("main/audio/wake_words/afe_wake_word.cc")
    custom = read("main/audio/wake_words/custom_wake_word.cc")
    for source in (afe, custom):
        assert "shutting_down_" in source
        assert "encode_active_" in source
        assert "wake_word_cv_.notify_all()" in source
        encode_task = source[source.index('"encode_wake_word"') - 6500:source.index('"encode_wake_word"')]
        ack = encode_task.rindex("xEventGroupSetBits(exit_events, ENCODE_EXITED_EVENT)")
        assert encode_task.rfind("this_->", 0, ack) >= 0
        assert "this_->" not in encode_task[ack:]
        assert "vTaskDeleteWithCaps(nullptr)" in encode_task[ack:]
    detection = afe[afe.index("const BaseType_t detection_created"):afe.index('"audio_detection"')]
    detection_ack = detection.index("xEventGroupSetBits(exit_events, DETECTION_EXITED_EVENT)")
    assert "this_->" not in detection[detection_ack:]
    assert detection.index("audio_detection_task_handle_.store(nullptr") < detection_ack
    assert "DETECTION_EXITED_EVENT | ENCODE_EXITED_EVENT" in afe


def test_afe_fetch_is_bounded_and_stop_acknowledges_before_reset():
    header = read("main/audio/wake_words/afe_wake_word.h")
    source = read("main/audio/wake_words/afe_wake_word.cc")

    assert "DETECTION_STOPPED_EVENT" in source
    assert "kFetchWaitMs = 100" in header
    assert "kStopAckTimeoutMs = 500" in header
    assert "fetch_with_delay(afe_data_, pdMS_TO_TICKS(kFetchWaitMs))" in source

    detection = source[source.index("void AfeWakeWord::AudioDetectionTask()") :]
    detection = detection[: detection.index("void AfeWakeWord::StoreWakeWordData")]
    assert "xEventGroupSetBits(event_group_, DETECTION_STOPPED_EVENT)" in detection

    stop = source[source.index("void AfeWakeWord::Stop()") :]
    stop = stop[: stop.index("void AfeWakeWord::Feed")]
    wait = stop.index("xEventGroupWaitBits")
    reset = stop.index("reset_buffer")
    assert wait < reset
    assert "BeginStopAndClear" in stop
    assert "WaitForStopAcknowledgement" in stop
    assert '"afe stop acknowledgement timeout' in stop
    assert "xTaskGetCurrentTaskHandle() == detection_task" in stop
    assert "pdTRUE, pdTRUE" in stop


def test_afe_discards_fetch_from_superseded_run_generation():
    header = read("main/audio/wake_words/afe_wake_word.h")
    source = read("main/audio/wake_words/afe_wake_word.cc")

    start = source[source.index("void AfeWakeWord::Start()") :]
    start = start[: start.index("void AfeWakeWord::Stop()")]
    assert "BeginStart(run_generation_)" in start

    detection = source[source.index("void AfeWakeWord::AudioDetectionTask()") :]
    detection = detection[: detection.index("void AfeWakeWord::StoreWakeWordData")]
    assert "const uint32_t fetch_generation" in detection
    generation_gate = detection.index("fetch_generation != run_generation_")
    assert generation_gate < detection.index("StoreWakeWordData")
    assert generation_gate < detection.index("wake_word_detected_callback_")
    assert "AcquireTransition" in source
    assert "TryAcquireTransition" in detection
    assert "if (!lifecycle_lock.owns_lock())" in detection
    assert detection.index("TryAcquireTransition") < detection.index("StoreWakeWordData")
    assert "AfeRunSynchronization run_synchronization_" in header
    assert "WaitForStopAcknowledgement" in source
    assert "std::atomic<TaskHandle_t> audio_detection_task_handle_" in header
    assert "audio_detection_task_handle_.load" in source
    assert "audio_detection_task_handle_.store" in source


def test_wifi_provisioning_rearms_only_after_ble_deinit():
    source = read("main/boards/common/wifi_board.cc")
    start = source.index("void WifiBoard::StartWifiConfigMode(")
    start_body = source[start:source.index("void WifiBoard::EnterWifiConfigMode()", start)]
    assert start_body.index("BeginWifiProvisioning()") < start_body.index("blufi.RestartForSetup();")

    connected = source[source.index("case NetworkEvent::Connected:"):]
    connected = connected[:connected.index("case NetworkEvent::Scanning:")]
    assert "CompleteSuccessfulProvisioningTeardown" not in connected
    blufi = read("main/boards/common/blufi.cpp")
    helper = blufi[blufi.index("bool Blufi::CompleteSuccessfulProvisioningTeardown"):]
    helper = helper[:helper.index("#ifdef CONFIG_BT_BLUEDROID_ENABLED")]
    rearm = helper.index("EndWifiProvisioningAndRearm(")
    assert helper.index("deinit()") < rearm
    assert helper.index("if (deinit_error != ESP_OK)") < rearm
    assert "provisioning_token" in helper[rearm:rearm + 120]


def test_ci_runs_deterministic_wake_word_lifecycle_gate():
    workflow = read(".github/workflows/build.yml")
    assert "host-tests:" in workflow
    assert "scripts/run_host_native_wake_word_lifecycle_test.sh" in workflow
    assert "scripts/run_host_native_blufi_transition_gate_test.sh" in workflow
    assert "tests/test_wake_word_lifecycle_contract.py" in workflow
    assert "tests/test_provisioning_success_teardown_contract.py" in workflow
