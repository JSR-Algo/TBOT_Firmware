import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CURRENT_PRODUCTION_OTA_URL = "https://esp.tjbot.vn/tbot/ota/"
CURRENT_PRODUCTION_WEBSOCKET_URL = ""
CURRENT_OTA_BUILD_VERSION = "2.2.51"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def kconfig_default(text: str, config_name: str) -> str:
    match = re.search(
        rf"config\s+{re.escape(config_name)}\b(?P<body>.*?)(?=\nconfig\s+|\nchoice\b|\nmenu\b|\nendmenu\b)",
        text,
        re.DOTALL,
    )
    assert match, f"missing config {config_name}"
    default = re.search(r'^\s*default\s+"(?P<value>[^"]*)"', match.group("body"), re.MULTILINE)
    assert default, f"missing default for {config_name}"
    return default.group("value")


def test_firmware_compiles_no_ephemeral_websocket_endpoint_fallback():
    kconfig = read("main/Kconfig.projbuild")

    websocket_default = kconfig_default(kconfig, "WEBSOCKET_URL")

    assert websocket_default == CURRENT_PRODUCTION_WEBSOCKET_URL
    assert "trycloudflare.com" not in websocket_default
    assert "ngrok" not in websocket_default.lower()

def test_project_version_advances_past_current_production_lcdwiki_ota():
    cmake = read("CMakeLists.txt")

    assert f'set(PROJECT_VER "{CURRENT_OTA_BUILD_VERSION}")' in cmake


def test_local_firmware_build_configs_compile_no_ephemeral_websocket_seed():
    local_configs = sorted(
        path for path in ROOT.glob("sdkconfig*")
        if path.is_file() and not path.name.endswith((".bak", ".old"))
    )
    assert local_configs

    for sdkconfig in local_configs:
        contents = sdkconfig.read_text(encoding="utf-8")
        if "CONFIG_WEBSOCKET_URL=" not in contents:
            continue

        assert f'CONFIG_WEBSOCKET_URL="{CURRENT_PRODUCTION_WEBSOCKET_URL}"' in contents, sdkconfig.name
        assert "trycloudflare.com" not in contents, sdkconfig.name
        assert "ngrok" not in contents.lower(), sdkconfig.name


def test_local_firmware_build_configs_compile_only_current_production_ota_seed():
    local_configs = sorted(
        path for path in ROOT.glob("sdkconfig*")
        if path.is_file() and not path.name.endswith((".bak", ".old"))
    )
    assert local_configs

    for sdkconfig in local_configs:
        contents = sdkconfig.read_text(encoding="utf-8")
        match = re.search(r'^CONFIG_OTA_URL="(?P<value>[^"]*)"', contents, re.MULTILINE)
        if not match:
            continue

        ota_url = match.group("value")
        assert ota_url == CURRENT_PRODUCTION_OTA_URL, sdkconfig.name
        assert "trycloudflare.com" not in ota_url, sdkconfig.name
        assert "ngrok" not in ota_url, sdkconfig.name
        assert "loca.lt" not in ota_url, sdkconfig.name
        assert "serveo.net" not in ota_url, sdkconfig.name

def test_prod_flash_script_rejects_tiny_placeholder_artifacts():
    script = read("scripts/flash_prod_new_robot.sh")

    assert "MIN_ARTIFACT_BYTES" in script
    assert "stat -f%z" in script or "stat -c%s" in script
    assert "artifact too small" in script


def test_firmware_prefers_ota_returned_websocket_url_before_compile_fallback():
    ota_cc = read("main/ota.cc")
    websocket_cc = read("main/protocols/websocket_protocol.cc")

    websocket_parse_start = ota_cc.index('cJSON *websocket = cJSON_GetObjectItem(root, "websocket")')
    websocket_parse_end = ota_cc.index("has_server_time_ = false", websocket_parse_start)
    websocket_parse_body = ota_cc[websocket_parse_start:websocket_parse_end]

    assert 'Settings settings("websocket", true);' in websocket_parse_body
    assert "cJSON_ArrayForEach(item, websocket)" in websocket_parse_body
    assert "settings.SetString(item->string, item->valuestring);" in websocket_parse_body
    assert "has_websocket_config_ = true;" in websocket_parse_body

    open_start = websocket_cc.index("bool WebsocketProtocol::OpenAudioChannel()")
    open_end = websocket_cc.index("websocket_->SetHeader", open_start)
    open_body = websocket_cc[open_start:open_end]

    assert 'Settings settings("websocket", false);' in open_body
    assert 'settings.GetString("url", CONFIG_WEBSOCKET_URL)' in open_body

def test_firmware_persists_ota_returned_backend_api_url_for_device_config_polling():
    ota_cc = read("main/ota.cc")

    assert 'cJSON *api_url = cJSON_GetObjectItem(root, "api_url")' in ota_cc
    assert 'Settings settings("backend", true);' in ota_cc
    assert 'settings.SetString("api_url", api_url->valuestring);' in ota_cc

def test_firmware_refreshes_websocket_url_from_authenticated_config_fetch_at_boot():
    reporter_cc = read("main/provisioning/claim_confirmation_reporter.cc")
    reporter_h = read("main/provisioning/claim_confirmation_reporter.h")
    application_cc = read("main/application.cc")

    assert "BuildTbotConfigFetchUrl" in reporter_h
    assert "RefreshWebsocketUrlFromConfigFetch" in reporter_h

    build_start = reporter_cc.index("std::string BuildTbotConfigFetchUrl")
    build_end = reporter_cc.index("bool RefreshWebsocketUrlFromConfigFetch", build_start)
    build_body = reporter_cc[build_start:build_end]
    assert 'return base + "/config/fetch";' in build_body
    assert 'if (base.find("/v1") == std::string::npos)' in build_body

    refresh_start = reporter_cc.index("bool RefreshWebsocketUrlFromConfigFetch")
    refresh_end = reporter_cc.index("bool PersistTbotClaimConfirmationResponse", refresh_start)
    refresh_body = reporter_cc[refresh_start:refresh_end]

    assert 'Settings backend_settings("backend", false);' in refresh_body
    assert 'backend_settings.GetString("api_url")' in refresh_body
    assert 'backend_settings.GetString("device_id")' in refresh_body
    assert 'backend_settings.GetString("device_secret")' in refresh_body
    assert "if (api_url.empty() || device_id.empty() || device_secret.empty())" in refresh_body
    assert 'http->SetHeader("X-Device-Id", device_id);' in refresh_body
    assert 'http->SetHeader("X-Device-Token", device_secret);' in refresh_body
    assert 'cJSON_GetObjectItem(root, "configBlob")' in refresh_body
    assert 'cJSON_GetObjectItem(config_blob, "websocket")' in refresh_body
    assert 'cJSON_GetObjectItem(websocket, "url")' in refresh_body
    assert 'Settings websocket_settings("websocket", true);' in refresh_body
    assert 'websocket_settings.SetString("url", ws_url->valuestring);' in refresh_body
    assert "response_body" not in "\n".join(
        line for line in refresh_body.splitlines() if "ESP_LOG" in line
    )

    activation_start = application_cc.index("void Application::ActivationTask()")
    activation_end = application_cc.index("void Application::CheckAssetsVersion()", activation_start)
    activation_body = application_cc[activation_start:activation_end]
    check_idx = activation_body.index("CheckNewVersion();")
    refresh_idx = activation_body.index("RefreshWebsocketUrlFromConfigFetch();")
    init_idx = activation_body.index("InitializeProtocol();")
    assert check_idx < refresh_idx < init_idx

def test_firmware_preserves_ota_returned_websocket_url_even_when_tunnel_hosts_differ():
    ota_cc = read("main/ota.cc")

    assert "NormalizeOtaWebsocketUrl" not in ota_cc
    assert "DeriveWebsocketUrlFromOtaUrl" not in ota_cc
    assert "normalizing stale OTA websocket URL" not in ota_cc

    websocket_parse_start = ota_cc.index('cJSON *websocket = cJSON_GetObjectItem(root, "websocket")')
    websocket_parse_end = ota_cc.index("has_server_time_ = false", websocket_parse_start)
    websocket_parse_body = ota_cc[websocket_parse_start:websocket_parse_end]

    assert "settings.SetString(item->string, item->valuestring);" in websocket_parse_body
    assert "NormalizeOtaWebsocketUrl" not in websocket_parse_body

def test_ble_setup_timeout_matches_contract_in_local_blufi_configs():
    for sdkconfig_name in ("sdkconfig.defaults.local",):
        contents = read(sdkconfig_name)
        assert "CONFIG_BLE_SETUP_TIMEOUT_SEC=300" in contents, sdkconfig_name

def test_websocket_protocol_sends_auth_token_as_header_not_query_param():
    websocket_cc = read("main/protocols/websocket_protocol.cc")

    assert 'websocket_->SetHeader("protocol-version", std::to_string(version_).c_str());' in websocket_cc
    assert 'websocket_->SetHeader("authorization", token.c_str());' in websocket_cc
    assert 'websocket_->SetHeader("device-id", device_id.c_str());' not in websocket_cc
    assert 'websocket_->SetHeader("client-id", client_id.c_str());' not in websocket_cc
    assert 'websocket_->SetHeader("Authorization", token.c_str());' not in websocket_cc
    assert 'websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());' not in websocket_cc
    assert 'websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());' not in websocket_cc

def test_websocket_protocol_sends_w3c_traceparent_header_on_connect():
    websocket_cc = read("main/protocols/websocket_protocol.cc")

    assert '#include <esp_random.h>' in websocket_cc
    assert "NewTraceParentHeader()" in websocket_cc
    assert 'websocket_->SetHeader("traceparent", traceparent.c_str());' in websocket_cc

def test_websocket_protocol_adds_identity_query_params_without_auth_query_or_logging_token():
    websocket_cc = read("main/protocols/websocket_protocol.cc")

    assert "UrlEncodeQueryValue" in websocket_cc
    assert "AppendWebsocketQueryParam" in websocket_cc
    assert 'AppendWebsocketQueryParam(connect_url, "device-id", device_id);' in websocket_cc
    assert 'AppendWebsocketQueryParam(connect_url, "client-id", client_id);' in websocket_cc
    assert 'AppendWebsocketQueryParam(connect_url, "authorization", token);' not in websocket_cc
    assert "websocket_->Connect(connect_url.c_str())" in websocket_cc
    assert 'ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_)' in websocket_cc
    assert 'ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", connect_url.c_str(), version_)' not in websocket_cc
