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


def test_claimed_boot_fails_closed_when_audio_workers_do_not_start():
    source = read("main/application.cc")
    initialize = function_body(source, "void Application::Initialize()")

    claimed = initialize.index("if (IsDeviceClaimed())")
    checked_start = initialize.index("if (!audio_service_.Start())", claimed)
    failure = function_body(initialize, "if (!audio_service_.Start())")
    uart = initialize.index("robot_uart_.Initialize()", checked_start)

    assert claimed < checked_start < uart
    assert "ESP_LOGE" in failure
    assert "return;" in failure
    assert "SetDeviceState(kDeviceStateIdle)" not in failure


def test_claim_completion_publishes_idle_only_after_checked_audio_start():
    source = read("main/application.cc")
    finish = function_body(
        source, "bool Application::FinishClaimActivationAfterLocalAssetsReady"
    )

    reload_protocol = finish.index("ReloadProtocolAfterClaimCredentials();")
    checked_start = finish.index("if (!audio_service_.Start())", reload_protocol)
    failure = function_body(finish, "if (!audio_service_.Start())")
    idle = finish.index("SetDeviceState(kDeviceStateIdle);", checked_start)
    wake = finish.index("audio_service_.EnableWakeWordDetection(true);", idle)
    heartbeat = finish.index("StartHeartbeat();", wake)
    success = finish.index("Lang::Strings::CONNECTED", heartbeat)

    assert reload_protocol < checked_start < idle < wake < heartbeat < success
    assert "ESP_LOGE" in failure
    assert "return false;" in failure
    for forbidden in (
        "SetDeviceState(kDeviceStateIdle)",
        "EnableWakeWordDetection",
        "StartHeartbeat",
        "Lang::Strings::CONNECTED",
    ):
        assert forbidden not in failure


def test_ota_failure_checks_audio_restart_without_hiding_upgrade_failure():
    source = read("main/application.cc")
    upgrade = function_body(source, "bool Application::UpgradeFirmware")
    failure = upgrade[upgrade.index("if (!upgrade_success)") :]

    checked_start = failure.index("if (!audio_service_.Start())")
    audio_failure = function_body(failure, "if (!audio_service_.Start())")
    power_restore = failure.index("board.SetPowerSaveLevel", checked_start)
    alert = failure.index("Lang::Strings::UPGRADE_FAILED", power_restore)
    result = failure.index("return false;", alert)

    assert checked_start < power_restore < alert < result
    assert "ESP_LOGE" in audio_failure
    assert "return" not in audio_failure
