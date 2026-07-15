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
    assert "detection_exited_" in afe


def test_wifi_provisioning_rearms_only_after_ble_deinit():
    source = read("main/boards/common/wifi_board.cc")
    start = source.index("void WifiBoard::StartWifiConfigMode()")
    start_body = source[start:source.index("void WifiBoard::EnterWifiConfigMode()", start)]
    assert start_body.index("BeginWifiProvisioning()") < start_body.index("blufi.init();")

    for deinit in (index for index in range(len(source)) if source.startswith("blufi.deinit();", index)):
        tail = source[deinit:deinit + 180]
        assert "EndWifiProvisioningAndRearm();" in tail


def test_ci_runs_deterministic_wake_word_lifecycle_gate():
    workflow = read(".github/workflows/build.yml")
    assert "host-tests:" in workflow
    assert "scripts/run_host_native_wake_word_lifecycle_test.sh" in workflow
    assert "tests/test_wake_word_lifecycle_contract.py" in workflow
