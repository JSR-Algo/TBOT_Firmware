"""Production provisioning logs must not expose local network or robot identifiers."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_boot_suppresses_esp_idf_components_that_log_raw_network_identifiers():
    source = read("main/main.cc")

    assert 'esp_log_level_set("wifi", ESP_LOG_WARN);' in source
    assert 'esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);' in source
    assert 'esp_log_level_set("BLE_INIT", ESP_LOG_WARN);' in source
    assert source.index('esp_log_level_set("wifi", ESP_LOG_WARN);') < source.index("nvs_flash_init()")
    assert source.index('esp_log_level_set("BLE_INIT", ESP_LOG_WARN);') < source.index("app.Initialize()")


def test_board_and_blufi_logs_do_not_print_full_robot_identifiers():
    board = read("main/boards/common/board.cc")
    blufi = read("main/boards/common/blufi.cpp")

    assert '"UUID=%s' not in board
    assert '"BD ADDR:' not in blufi
    assert '"BLUFI advertised name: %s"' not in blufi


def test_wifi_connection_logs_do_not_print_ssid_or_bssid_values():
    wifi_board = read("main/boards/common/wifi_board.cc")
    wifi_station = read("components/esp-wifi-connect/wifi_station.cc")
    afsk = read("main/boards/common/afsk_demod.cc")

    assert '"Connected to WiFi: %s"' not in wifi_board
    assert '"WiFi connecting to %s"' not in wifi_board
    assert '"Found AP: %s, BSSID:' not in wifi_station
    assert '"WiFi SSID: %s' not in afsk


def test_claim_logs_do_not_print_urls_that_embed_device_identifiers():
    source = read("main/provisioning/claim_confirmation_reporter.cc")

    assert '"Fetching device config via %s"' not in source
    assert '"Fetching backend api_url fallback via %s"' not in source
    assert '"Fetching runtime config via %s"' not in source
    assert '"Confirming claim via %s"' not in source


def test_wifi_storage_and_legacy_config_logs_never_print_credentials_or_clients():
    ssid_manager = read("components/esp-wifi-connect/ssid_manager.cc")
    wifi_station = read("components/esp-wifi-connect/wifi_station.cc")
    config_ap = read("components/esp-wifi-connect/wifi_configuration_ap.cc")

    assert "item.ssid.c_str()" not in ssid_manager
    assert "ssid.c_str(), ssid.size()" not in ssid_manager
    assert '"Reconnecting %s' not in wifi_station
    assert '"SmartConfig SSID: %s, Password: %s"' not in config_ap
    assert '"Access Point started with SSID %s"' not in config_ap
    assert '"Connecting to WiFi %s"' not in config_ap
    assert '"Connected to WiFi %s"' not in config_ap
    assert '"Failed to connect to WiFi %s"' not in config_ap
    assert '"Save SSID %s' not in config_ap
    assert '"Station " MACSTR' not in config_ap


def test_activation_and_runtime_identity_logs_expose_presence_or_lengths_only():
    ota = read("main/ota.cc")
    websocket = read("main/protocols/websocket_protocol.cc")

    assert '"Setup HTTP, User-Agent: %s, Serial-Number: %s"' not in ota
    assert '"Activation payload: %s"' not in ota
    assert '"Failed to activate, code: %d, body: %s"' not in ota
    assert '"Websocket auth identity: device-id=%s client-id=%s' not in websocket


def test_signed_operation_urls_are_not_written_to_production_logs():
    ota = read("main/ota.cc")
    application = read("main/application.cc")
    assets = read("main/assets.cc")
    mcp = read("main/mcp_server.cc")
    websocket = read("main/protocols/websocket_protocol.cc")

    assert '"Upgrading firmware from %s"' not in ota
    assert '"Starting firmware upgrade from URL: %s"' not in application
    assert '"Downloading new version of assets from %s"' not in assets
    assert '"User requested firmware upgrade from URL: %s"' not in mcp
    assert '"Upload snapshot %u bytes to %s"' not in mcp
    assert '"Connecting to websocket server: %s' not in websocket


def test_all_board_and_status_paths_redact_ssids_and_routing_urls():
    cardputer = read("main/boards/m5stack-cardputer-adv/m5stack_cardputer_adv.cc")
    cardputer_ui = read("main/boards/m5stack-cardputer-adv/wifi_config_ui.cc")
    reporter = read("main/provisioning/provisioning_status_reporter.cc")
    config_ap = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    application = read("main/application.cc")

    assert '"Attempting WiFi connection to: %s"' not in cardputer
    assert '"Saved WiFi credentials for: %s"' not in cardputer_ui
    assert "url.c_str()," not in reporter
    assert '"Saved settings: ota_url=%s' not in config_ap
    assert 'code=%d, url=%s' not in application
    assert 'code=%d, url=%s"' not in application


def test_provisioning_failure_extracts_only_a_safe_top_level_error_code():
    header = read("main/provisioning/provisioning_status_reporter.h")
    source = read("main/provisioning/provisioning_status_reporter.cc")

    assert "ExtractProvisioningErrorCode" in header
    assert "cJSON_Parse" in source
    assert 'cJSON_GetObjectItem(root, "code")' in source
    assert "cJSON_IsString(code)" in source
    assert "kMaxProvisioningErrorCodeLength" in source
    assert "std::isalnum" in source
    assert "ch != '_' && ch != '-'" in source
    assert "unknown" in source
    assert "error_code=%s" in source
    # The response body may reflect credentials; only the bounded code is logged.
    assert 'resp_body.c_str()' not in source
    assert 'body=%s' not in source
