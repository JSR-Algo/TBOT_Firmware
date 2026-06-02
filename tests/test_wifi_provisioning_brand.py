from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = ROOT.parent
FORBIDDEN_BRAND_TEXT = ("Xiaozhi", "XiaoZhi", "小智")


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_wifi_provisioning_uses_tbot_brand_names():
    wifi_board = read("main/boards/common/wifi_board.cc")
    blufi = read("main/boards/common/blufi.cpp")

    assert 'config.ssid_prefix = "TBot";' in wifi_board
    assert 'config.ssid_prefix = "Xiaozhi";' not in wifi_board
    assert 'static std::string GetBlufiDeviceName()' in blufi
    assert 'esp_ble_gap_set_device_name(device_name.c_str())' in blufi
    assert 'TBOT-%02X%02X%02X%02X%02X%02X' in blufi
    assert '#define BLUFI_DEVICE_NAME "TBot-Blufi"' not in blufi
    assert '#define BLUFI_DEVICE_NAME "Xiaozhi-Blufi"' not in blufi


def test_blufi_never_logs_wifi_password_values():
    blufi = read("main/boards/common/blufi.cpp")

    assert 'ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD : %s"' not in blufi
    assert 'm_sta_config.sta.password' not in [
        line.strip()
        for line in blufi.splitlines()
        if 'ESP_LOG' in line and 'password' in line.lower()
    ]


def test_blufi_config_mode_is_wired_into_firmware():
    wifi_board = read("main/boards/common/wifi_board.cc")
    cmake = read("main/CMakeLists.txt")
    kconfig = read("main/Kconfig.projbuild")
    defaults = read("sdkconfig.defaults")

    assert '#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING' in wifi_board
    assert 'auto &blufi = Blufi::GetInstance();' in wifi_board
    assert 'blufi.init();' in wifi_board
    assert 'if (CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)' in cmake
    assert 'list(APPEND SOURCES "boards/common/blufi.cpp")' in cmake
    assert 'config USE_ESP_BLUFI_WIFI_PROVISIONING' in kconfig
    assert 'select BT_BLE_BLUFI_ENABLE' in kconfig
    assert '# CONFIG_USE_HOTSPOT_WIFI_PROVISIONING is not set' in defaults
    assert 'CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING=y' in defaults
    assert 'CONFIG_BT_BLUEDROID_ENABLED=y' in defaults


def test_system_info_reports_tbot_application_name():
    board = read("main/boards/common/board.cc")

    assert 'static constexpr const char* TBOT_APPLICATION_NAME = "TBot";' in board
    assert '"name":")" + std::string(TBOT_APPLICATION_NAME)' in board
    assert '"name":")" + std::string(app_desc->project_name)' not in board


def test_user_facing_brand_text_uses_tbot():
    text_suffixes = {".md", ".vue", ".html", ".tex"}
    ignored_dirs = {".git", "build", "managed_components", "node_modules", "target"}
    files = [
        path
        for path in PROJECT_ROOT.rglob("*")
        if path.is_file()
        and path.suffix in text_suffixes
        and ignored_dirs.isdisjoint(path.relative_to(PROJECT_ROOT).parts)
    ]
    files.append(ROOT / "main" / "Kconfig.projbuild")

    offenders = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        for forbidden in FORBIDDEN_BRAND_TEXT:
            if forbidden in text:
                offenders.append(f"{path.relative_to(PROJECT_ROOT)} contains {forbidden}")

    assert offenders == []
