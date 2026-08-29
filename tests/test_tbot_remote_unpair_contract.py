from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def app_source() -> str:
    return (ROOT / "main/application.cc").read_text(encoding="utf-8")


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
        "SsidManager::GetInstance().Clear();"
    )
    assert body.index("SsidManager::GetInstance().Clear();") < body.index("esp_restart();")
