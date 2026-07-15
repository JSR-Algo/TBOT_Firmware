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
    assert detection.index("audio_detection_task_handle_ = nullptr") < detection_ack
    assert "DETECTION_EXITED_EVENT | ENCODE_EXITED_EVENT" in afe


def test_wifi_provisioning_rearms_only_after_ble_deinit():
    source = read("main/boards/common/wifi_board.cc")
    start = source.index("void WifiBoard::StartWifiConfigMode(")
    start_body = source[start:source.index("void WifiBoard::EnterWifiConfigMode()", start)]
    assert start_body.index("BeginWifiProvisioning()") < start_body.index("blufi.init();")

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
