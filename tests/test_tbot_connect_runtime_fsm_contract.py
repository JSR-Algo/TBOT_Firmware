"""Contract tests for the P2/P3 firmware connection-flow work.

These text-scrape source (they do NOT compile firmware) and assert that the
required runtime behaviour EXISTS:
  * a periodic heartbeat POST /device/heartbeat sender (C5),
  * the BLE re-advertise attempt cap (C8),
  * the six missing copy strings wired into the language assets + render paths,
  * the runtime FSM mapper that references the claim states (LOCKED decision 3).

On-hardware / integration tests remain the only true verification.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


# ---------------------------------------------------------------------------
# C5 — heartbeat POST /device/heartbeat
# ---------------------------------------------------------------------------

def test_application_has_a_periodic_device_heartbeat_sender():
    header = read("main/application.h")
    source = read("main/application.cc")
    send_body = function_body(source, "bool Application::SendDeviceHeartbeat")
    start_body = function_body(source, "void Application::StartHeartbeat")

    # Periodic timer plumbing exists.
    assert "esp_timer_handle_t heartbeat_timer_" in header
    assert "kHeartbeatIntervalUs" in source
    assert "esp_timer_start_periodic(heartbeat_timer_, kHeartbeatIntervalUs)" in start_body

    # Cadence is in the 15-30s band (20s).
    assert "20ULL * 1000000ULL" in source

    # POSTs to /device/heartbeat carrying the backend heartbeat DTO. Radio and
    # temperature telemetry still come from the board status JSON.
    assert '"/device/heartbeat"' in send_body
    assert "Board::GetInstance().GetDeviceStatusJson()" in send_body
    assert 'cJSON_AddStringToObject(root, "device_id", Board::GetInstance().GetUuid().c_str())' in source
    assert 'cJSON_AddStringToObject(root, "firmware_version",' in source
    assert 'cJSON_AddNumberToObject(root, "battery_level", battery_level)' in source
    assert 'cJSON_AddItemToObject(root, "connectivity_metrics", connectivity)' in source
    assert 'cJSON_AddStringToObject(connectivity, "connectivity_state", "online")' in source
    assert 'cJSON_AddNumberToObject(connectivity, "wifi_rssi", wifi_rssi)' in source
    assert 'cJSON_AddStringToObject(root, "ble_state",' in source
    assert 'cJSON_AddStringToObject(root, "ap_state",' in source
    assert 'cJSON_AddNumberToObject(root, "temp", temp)' in source
    assert 'http->Open("POST", url)' in send_body

    # Claimed/online-gated: no backend device secret -> no heartbeat. The
    # realtime websocket token belongs to the ESP WS auth path, not backend API
    # heartbeat auth.
    assert 'backend_settings.GetString("device_secret")' in send_body
    assert "if (device_secret.empty())" in send_body
    assert 'websocket_settings.GetString("token")' not in send_body
    assert 'http->SetHeader("X-Device-Token", device_secret)' in send_body
    assert 'http->SetHeader("Authorization", "Bearer " + device_secret)' not in send_body
    assert 'ESP_LOGI(TAG, "Heartbeat accepted (HTTP %d)", status_code);' in send_body

    # Started when the device session comes up.
    assert "StartHeartbeat();" in source


def test_heartbeat_reuses_board_status_radio_fields_so_online_reports_live_radios():
    # GetDeviceStatusJson reports live radio state: BluFi builds expose BLE via
    # GetBleStateString(), while non-BluFi builds fall back to ble_state=off.
    # Assert heartbeat extracts those fields instead of hand-rolling divergent
    # radio state, so claimed ONLINE can report BLE advertising for reconnect.
    wifi_board = read("main/boards/common/wifi_board.cc")
    status_body = function_body(wifi_board, "std::string WifiBoard::GetDeviceStatusJson()")
    source = read("main/application.cc")
    assert 'cJSON_AddStringToObject(root, "ble_state", Blufi::GetInstance().GetBleStateString());' in status_body
    assert 'cJSON_AddStringToObject(root, "ble_state", "off");' in status_body
    assert 'cJSON_AddStringToObject(root, "ap_state", GetApStateString());' in status_body
    assert 'CopyStringField(status_root, "ble_state", "off")' in source
    assert 'CopyStringField(status_root, "ap_state", "off")' in source

def test_websocket_audio_channel_open_starts_and_close_stops_heartbeat():
    source = read("main/application.cc")

    opened_start = source.index("protocol_->OnAudioChannelOpened")
    opened_end = source.index("protocol_->OnAudioChannelClosed", opened_start)
    opened_body = source[opened_start:opened_end]
    assert "StartHeartbeat();" in opened_body

    closed_start = source.index("protocol_->OnAudioChannelClosed")
    closed_end = source.index("protocol_->OnIncomingJson", closed_start)
    closed_body = source[closed_start:closed_end]
    assert "StopHeartbeat();" in closed_body


# ---------------------------------------------------------------------------
# C8 — BLE re-advertise cap
# ---------------------------------------------------------------------------

def test_blufi_caps_re_advertising_on_disconnect():
    header = read("main/boards/common/blufi.h")
    source = read("main/boards/common/blufi.cpp")
    disconnect_body = function_body(source, "case ESP_BLUFI_EVENT_BLE_DISCONNECT:")
    init_body = function_body(source, "esp_err_t Blufi::init()")

    # Bounded attempt counter declared + reset per setup entry.
    assert "ble_readvertise_count_" in header
    assert "kMaxBleReadvertiseAttempts" in header
    assert "ble_readvertise_count_ = 0;" in init_body

    # Disconnect handler honours the cap and only then restarts advertising.
    assert "ble_readvertise_count_ >= kMaxBleReadvertiseAttempts" in disconnect_body
    assert "++ble_readvertise_count_;" in disconnect_body
    assert "esp_blufi_adv_start();" in disconnect_body
    # Cap-reached path must NOT restart (it logs and stops).
    assert "NOT restarting advertising" in disconnect_body


# ---------------------------------------------------------------------------
# Missing copy strings (§3.4) wired into assets + render paths
# ---------------------------------------------------------------------------

REQUIRED_COPY = {
    "READY_TO_CONNECT": ("Ready to connect", "Sẵn sàng kết nối"),
    "OPEN_TBOT_APP": ("Open TBot app", "Mở ứng dụng TBot"),
    "PRESS_BUTTON_TO_CONFIRM": ("Press button to confirm", "Nhấn nút để xác nhận"),
    "SETUP_EXPIRED": ("Setup expired", "Hết hạn thiết lập"),
    "SERVER_UNAVAILABLE_RETRYING": ("Server unavailable. Retrying...",
                                    "Máy chủ không khả dụng. Đang thử lại..."),
    "WIFI_FAILED_CHECK_PASSWORD": ("Wi-Fi failed. Check password.",
                                   "Wi-Fi thất bại. Kiểm tra mật khẩu."),
}


def test_missing_copy_strings_exist_in_language_assets():
    en = read("main/assets/locales/en-US/language.json")
    vi = read("main/assets/locales/vi-VN/language.json")
    generated = read("main/assets/lang_config.h")

    for key, (en_text, vi_text) in REQUIRED_COPY.items():
        assert f'"{key}": "{en_text}"' in en, f"en-US missing {key}"
        assert f'"{key}": "{vi_text}"' in vi, f"vi-VN missing {key}"
        # The build-generated header must carry the constant too (no toolchain
        # here to regenerate it, so it is hand-synced and asserted).
        assert f"constexpr const char* {key} =" in generated, f"lang_config.h missing {key}"


def test_new_copy_is_wired_to_render_paths():
    source = read("main/application.cc")
    # CLAIM_CONFIRM_TIMEOUT -> "Setup expired".
    assert "Lang::Strings::SETUP_EXPIRED" in source
    # OFFLINE_RETRY -> "Server unavailable. Retrying...".
    assert "Lang::Strings::SERVER_UNAVAILABLE_RETRYING" in source
    # CLAIM_AVAILABLE -> "Ready to connect".
    assert "Lang::Strings::READY_TO_CONNECT" in source


# ---------------------------------------------------------------------------
# Runtime FSM mapper (LOCKED decision 3) references the claim states
# ---------------------------------------------------------------------------

def test_fsm_mapper_exists_and_references_claim_states():
    mapper_h = read("main/tbot_connect_mapper.h")
    mapper_cc = read("main/tbot_connect_mapper.cc")
    cmake = read("main/CMakeLists.txt")

    # Compiled into the firmware (unlike the inert state table).
    assert 'list(APPEND SOURCES "tbot_connect_mapper.cc")' in cmake

    # Driven off the real runtime DeviceState + claim/BLE sub-states.
    assert "#include \"device_state.h\"" in mapper_h
    assert "#include \"tbot_connect_state.h\"" in mapper_h
    assert "enum class TbotClaimSubstate" in mapper_h
    assert "DeviceState device_state" in mapper_h

    # The claim-relevant states are wired for real.
    for state in (
        "CLAIM_AVAILABLE",
        "CLAIM_WAITING_CONFIRM",
        "CLAIM_CONFIRM_TIMEOUT",
        "CLAIM_CONFIRMED",
        "CLAIMED",
        "BACKEND_CONNECTING",
        "ONLINE",
        "OFFLINE_RETRY",
        "OTA_UPDATING",
        "ERROR_RECOVERABLE",
        "BLE_SETUP_ADVERTISING",
        "BLE_SETUP_TIMEOUT",
    ):
        assert f"TbotConnectState::{state}" in mapper_cc, f"mapper missing {state}"

    # AP states stay defined-but-dormant (compiled out in build-blufi) — they
    # must be acknowledged as inactive, not faked.
    assert "DORMANT" in mapper_h or "dormant" in mapper_h


def test_mapper_screen_text_comes_from_the_contract_table():
    mapper_cc = read("main/tbot_connect_mapper.cc")
    spec_body = function_body(mapper_cc, "const TbotConnectStateSpec* TbotConnectMapper::SpecFor")
    text_body = function_body(mapper_cc, "const char* TbotConnectMapper::ScreenTextFor")

    # Screen text is read from kTbotConnectStateSpecs, not re-invented.
    assert "kTbotConnectStateSpecs" in spec_body
    assert "screen_text" in text_body
