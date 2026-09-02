from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def app_source() -> str:
    return (ROOT / "main/application.cc").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index]
    raise AssertionError(f"unterminated function: {signature}")


def system_command_body() -> str:
    source = app_source()
    start = source.index('strcmp(type->valuestring, "system") == 0')
    end = source.index('strcmp(type->valuestring, "alert") == 0', start)
    return source[start:end]


def test_cloud_unpair_command_reuses_the_proven_repair_pairing_flow():
    body = system_command_body()

    branch = body[body.index('strcmp(command->valuestring, "unpair") == 0') :]
    assert "EnterRepairPairingMode();" in branch
    assert branch.index("EnterRepairPairingMode();") < branch.index("Unknown system command")


def test_cloud_unpair_is_rejected_during_an_active_lesson():
    body = system_command_body()
    branch = body[body.index('strcmp(command->valuestring, "unpair") == 0') :]

    guard = branch[: branch.index("EnterRepairPairingMode();")]
    assert "lesson_runtime_active_.load()" in guard
    assert "return;" in guard


def test_cloud_wifi_setup_keeps_ownership_and_enters_wifi_config_without_boot():
    body = system_command_body()

    branch = body[body.index('strcmp(command->valuestring, "wifi_setup") == 0') :]
    guard = branch[: branch.index("EnterWifiConfigMode();")]
    assert "lesson_runtime_active_.load()" in guard
    assert "return;" in guard
    assert "static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();" in branch
    assert "EnterRepairPairingMode" not in branch[: branch.index("Unknown system command")]


def test_repair_flow_shows_initializing_before_wifi_is_cleared_and_robot_restarts():
    source = app_source()
    start = source.index("void Application::EnterRepairPairingMode()")
    end = source.index("namespace {", start)
    body = source[start:end]

    assert "display->SetStatus(Lang::Strings::INITIALIZING);" in body
    assert body.index("display->SetStatus(Lang::Strings::INITIALIZING);") < body.index(
        "SystemReset::ReleaseCloudOwnership()"
    )
    assert body.index("display->SetStatus(Lang::Strings::INITIALIZING);") < body.index(
        "SsidManager::GetInstance().ForceClearAndCancelTransaction()"
    )
    assert body.index("SsidManager::GetInstance().ForceClearAndCancelTransaction()") < body.index("esp_restart();")


def test_heartbeat_revocation_forgets_identity_and_wifi_then_reboots_into_setup():
    source = app_source()
    start = source.index("void Application::HandleHeartbeatAuthFailure")
    end = source.index("void Application::EnterRepairPairingMode", start)
    body = source[start:end]

    # A heartbeat 401/403 after mobile unpair is the robot's durable remote-unpair
    # signal. No stale identity or saved network may survive the automatic reboot.
    assert 'backend_settings.SetString("device_id", "");' in body
    assert 'backend_settings.SetString("device_secret", "");' in body
    assert 'websocket_settings.SetString("token", "");' in body
    assert 'websocket_settings.SetString("url", "");' in body
    assert "SsidManager::GetInstance().ForceClearAndCancelTransaction()" in body
    assert "esp_restart();" in body
    assert body.index('backend_settings.SetString("device_secret", "");') < body.index(
        "SsidManager::GetInstance().ForceClearAndCancelTransaction()"
    )
    assert body.index("SsidManager::GetInstance().ForceClearAndCancelTransaction()") < body.index("esp_restart();")


def test_destructive_wifi_clear_failure_blocks_reboot_at_both_boundaries():
    source = app_source()
    for signature in (
        "void Application::HandleHeartbeatAuthFailure",
        "void Application::EnterRepairPairingMode",
    ):
        body = function_body(source, signature)
        clear = body.index("ForceClearAndCancelTransaction")
        failure = body.index("SsidMutationResult::kApplied", clear)
        early_return = body.index("return;", failure)
        restart = body.index("esp_restart();", early_return)
        assert clear < failure < early_return < restart
