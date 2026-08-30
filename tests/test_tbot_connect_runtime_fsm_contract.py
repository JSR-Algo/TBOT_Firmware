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
    dispatch_body = function_body(source, "void Application::DispatchDeviceHeartbeat")
    send_body = function_body(source, "int Application::SendDeviceHeartbeat")
    start_body = function_body(source, "void Application::StartHeartbeat")

    # Periodic timer plumbing exists.
    assert "esp_timer_handle_t heartbeat_timer_" in header
    assert "kHeartbeatIntervalUs" in source
    assert "esp_timer_start_periodic(heartbeat_timer_, kHeartbeatIntervalUs)" in start_body

    # Cadence is in the 15-30s band (20s).
    assert "20ULL * 1000000ULL" in source

    # POSTs to /device/heartbeat carrying the backend heartbeat DTO. Radio and
    # temperature telemetry still come from the board status JSON.
    assert '"/device/heartbeat"' in dispatch_body
    assert "Board::GetInstance().GetDeviceStatusJson()" in dispatch_body
    assert 'const std::string backend_device_id = backend_settings.GetString("device_id");' in dispatch_body
    assert 'BuildTbotHeartbeatBody(status_json, backend_device_id)' in dispatch_body
    assert 'cJSON_AddStringToObject(root, "device_id", device_id.c_str())' in source
    assert 'Board::GetInstance().GetUuid().c_str()' not in function_body(source, "std::string BuildTbotHeartbeatBody")
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
    assert 'backend_settings.GetString("device_secret")' in dispatch_body
    assert "if (device_secret.empty())" in dispatch_body
    assert 'websocket_settings.GetString("token")' not in dispatch_body
    assert 'http->SetHeader("X-Device-Token", device_secret)' in send_body
    assert 'http->SetHeader("Authorization", "Bearer " + device_secret)' not in send_body
    assert 'ESP_LOGI(TAG, "Heartbeat accepted (HTTP %d)", status_code);' in send_body

    # Started when the device session comes up.
    assert "StartHeartbeat();" in source


def test_heartbeat_uses_one_persistent_worker_allocated_before_heap_fragmentation():
    source = read("main/application.cc")
    start_body = function_body(source, "void Application::StartHeartbeat")
    dispatch_body = function_body(source, "void Application::DispatchDeviceHeartbeat")
    worker_body = function_body(source, "void Application::HeartbeatTask")

    # Physical ESP32-S3 evidence: repeated transient TLS workers progressively
    # fragment internal SRAM until even a fixed internal stack cannot be allocated.
    # Allocate one worker while claim confirmation still has a large contiguous
    # block, then feed it heartbeat contexts through a one-slot queue.
    assert "xQueueCreate(1, sizeof(void*))" in start_body
    assert '"heartbeat_http", 8192' in start_body
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in start_body
    assert "xTaskCreateWithCaps" not in dispatch_body
    assert "xQueueSend(heartbeat_queue_" in dispatch_body
    assert "while (xQueueReceive(self->heartbeat_queue_" in worker_body
    assert "vTaskDelete(nullptr)" not in worker_body


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

def test_websocket_online_paths_dispatch_first_heartbeat_immediately():
    source = read("main/application.cc")

    connected_start = source.index("protocol_->OnConnected(")
    connected_end = source.index("protocol_->OnNetworkError(", connected_start)
    connected_body = source[connected_start:connected_end]
    connected_start_index = connected_body.index("StartHeartbeat();")
    connected_dispatch_index = connected_body.index("DispatchDeviceHeartbeat();")
    assert connected_start_index < connected_dispatch_index

    opened_start = source.index("protocol_->OnAudioChannelOpened")
    opened_end = source.index("protocol_->OnAudioChannelClosed", opened_start)
    opened_body = source[opened_start:opened_end]
    opened_start_index = opened_body.index("StartHeartbeat();")
    opened_dispatch_index = opened_body.index("DispatchDeviceHeartbeat();")
    assert opened_start_index < opened_dispatch_index

def test_claim_confirm_returns_to_idle_wake_word_instead_of_starting_listening_session():
    source = read("main/application.cc")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")
    result_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    finish_body = function_body(source, "bool Application::FinishClaimActivationAfterLocalAssetsReady")

    # A fresh claim must make the robot ready for the next explicit wake phrase.
    # Opening the realtime channel here can drive the normal connect path into
    # Listening, where CONFIG_WAKE_WORD_DETECTION_IN_LISTENING is disabled for
    # this board. That leaves the user saying "Hi ESP" while the wake-word path
    # is off.
    assert "ApplyPendingTbotClaimConfirmationResult(confirmation_result, provisioning_token)" in confirm_body
    assert "if (!FinishClaimActivationAfterLocalAssetsReady())" in result_body
    assert "SetDeviceState(kDeviceStateIdle);" in finish_body
    assert "audio_service_.EnableWakeWordDetection(true);" in finish_body
    assert "protocol_->Start()" not in result_body
    assert "protocol_->Start()" not in finish_body
    assert "Claim confirmed: warming realtime audio WS" not in result_body

def test_claim_confirm_starts_heartbeat_after_credentials_are_persisted():
    source = read("main/application.cc")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")
    result_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    finish_body = function_body(source, "bool Application::FinishClaimActivationAfterLocalAssetsReady")

    assert "ApplyPendingTbotClaimConfirmationResult(confirmation_result, provisioning_token)" in confirm_body
    assert "if (!FinishClaimActivationAfterLocalAssetsReady())" in result_body
    success_body = finish_body[finish_body.index("ReloadProtocolAfterClaimCredentials();"):]

    idle_index = success_body.index("SetDeviceState(kDeviceStateIdle);")
    wake_index = success_body.index("audio_service_.EnableWakeWordDetection(true);")
    heartbeat_index = success_body.index("StartHeartbeat();")
    dispatch_index = success_body.index("DispatchDeviceHeartbeat();")

    assert idle_index < wake_index < heartbeat_index < dispatch_index
    assert "protocol_->Start()" not in success_body[:heartbeat_index]

def test_websocket_protocol_does_not_auto_start_until_wake_or_explicit_click():
    source = read("main/application.cc")
    initialize_body = function_body(source, "void Application::InitializeProtocol")

    # WebSocket Start() opens an audio channel. Boot initialization may open only
    # the passive lesson socket; the normal realtime path stays behind wake-word
    # or an explicit button/chat action.
    assert "if (is_websocket_protocol)" in initialize_body
    websocket_branch = initialize_body[
        initialize_body.index("if (is_websocket_protocol)") :
        initialize_body.index("} else {\n        protocol_->Start();\n    }")
    ]
    assert "StartPassiveLessonWebsocket();" in websocket_branch
    assert "protocol_->Start();" not in websocket_branch
    assert "else {\n        protocol_->Start();\n    }" in initialize_body
    assert "is_websocket_protocol && !IsDeviceClaimed()" not in initialize_body

def test_late_activation_done_does_not_cut_wake_connect_or_voice_session_to_idle():
    source = read("main/application.cc")
    body = function_body(source, "void Application::HandleActivationDoneEvent")

    # Activation can finish after a wake phrase has already moved the runtime to
    # Connecting. It must not force Idle and drop the wake-open completion path.
    assert "kDeviceStateConnecting" in body
    assert "kDeviceStateListening" in body
    assert "kDeviceStateSpeaking" in body
    guard = body.index("Activation done ignored because runtime audio is active")
    set_idle = body.index("SetDeviceState(kDeviceStateIdle);")
    assert guard < set_idle


# ---------------------------------------------------------------------------
# C8 — BLE re-advertise cap
# ---------------------------------------------------------------------------

def test_blufi_caps_re_advertising_on_disconnect():
    header = read("main/boards/common/blufi.h")
    source = read("main/boards/common/blufi.cpp")
    disconnect_body = function_body(source, "case ESP_BLUFI_EVENT_BLE_DISCONNECT:")
    init_body = function_body(source, "esp_err_t Blufi::_init_impl()")

    # Bounded attempt counter declared + reset per setup entry.
    assert "ble_readvertise_count_" in header
    assert "kMaxBleReadvertiseAttempts" in header
    assert "ble_readvertise_count_ = 0;" in init_body

    # Disconnect handler honours the cap and only then restarts advertising.
    assert "ble_readvertise_count_ >= kMaxBleReadvertiseAttempts" in disconnect_body
    assert "++ble_readvertise_count_;" in disconnect_body
    assert "StartTbotBlufiAdvertising" in disconnect_body
    # Cap-reached path must NOT restart (it logs and stops).
    assert "NOT restarting advertising" in disconnect_body


# ---------------------------------------------------------------------------
# Missing copy strings (§3.4) wired into assets + render paths
# ---------------------------------------------------------------------------

REQUIRED_COPY = {
    "READY_TO_CONNECT": ("Ready to connect", "Sẵn sàng kết nối"),
    "OPEN_TBOT_APP": ("Open TBot app", "Mở ứng dụng TBot"),
    "SEARCHING_FOR_DEVICE": ("Searching for device...", "Đang tìm kiếm thiết bị..."),
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
    generator = read("scripts/gen_lang.py")

    assert "key.upper()" in generator
    for key, (en_text, vi_text) in REQUIRED_COPY.items():
        assert f'"{key}": "{en_text}"' in en, f"en-US missing {key}"
        assert f'"{key}": "{vi_text}"' in vi, f"vi-VN missing {key}"


def test_only_ble_advertising_uses_searching_for_device_copy():
    states = read("main/tbot_connect_state.h")

    wifi_not_configured = states[
        states.index(".state = TbotConnectState::WIFI_NOT_CONFIGURED,"):
        states.index(".state = TbotConnectState::WIFI_CONNECTING,")
    ]
    ble_advertising = states[
        states.index(".state = TbotConnectState::BLE_SETUP_ADVERTISING,"):
        states.index(".state = TbotConnectState::BLE_SETUP_TIMEOUT,")
    ]
    ap_setup = states[
        states.index(".state = TbotConnectState::AP_SETUP_ACTIVE,"):
        states.index(".state = TbotConnectState::AP_SETUP_TIMEOUT,")
    ]

    assert '.screen_text = "Searching for device..."' in ble_advertising
    assert '.screen_text = "Open TBot app"' in wifi_not_configured
    assert '.screen_text = "Open TBot app"' in ap_setup


def test_new_copy_is_wired_to_render_paths():
    source = read("main/application.cc")
    # CLAIM_CONFIRM_TIMEOUT -> "Setup expired".
    assert "Lang::Strings::SETUP_EXPIRED" in source
    # OFFLINE_RETRY -> "Server unavailable. Retrying...".
    assert "Lang::Strings::SERVER_UNAVAILABLE_RETRYING" in source
    # CLAIM_AVAILABLE -> "Ready to connect".
    assert "Lang::Strings::READY_TO_CONNECT" in source
    connect_copy = function_body(source, "static const char* ConnectStateScreenCopy")
    ble_case = connect_copy[
        connect_copy.index("case TbotConnectState::BLE_SETUP_ADVERTISING:"):
        connect_copy.index("default:")
    ]
    assert "return Lang::Strings::SEARCHING_FOR_DEVICE;" in ble_case


def test_lcdwiki_font_fallback_preserves_ready_to_connect_vietnamese_glyph():
    cmake = read("main/CMakeLists.txt")
    font_source = read("main/display/lvgl_display/tbot_vietnamese_20_4.c")
    font_runtime = read("main/display/lvgl_display/lvgl_font.cc")

    lcdwiki_block = cmake.split("elseif(CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P)", 1)[1]
    lcdwiki_block = lcdwiki_block.split("elseif(", 1)[0]
    assert 'set(BUILTIN_TEXT_FONT tbot_vietnamese_20_4)' in lcdwiki_block
    assert '"display/lvgl_display/tbot_vietnamese_20_4.c"' in cmake
    assert ".range_start = 7861" in font_source  # U+1EB5: ẵ
    assert ".fallback = &font_puhui_basic_20_4" in font_source
    assert ".line_height = 25" in font_source
    assert ".base_line = 6" in font_source
    assert "font_->fallback = &tbot_vietnamese_20_4" in font_runtime


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


def test_wifi_config_screen_takes_priority_over_claim_overlay():
    mapper_cc = read("main/tbot_connect_mapper.cc")
    resolve = function_body(mapper_cc, "TbotConnectState TbotConnectMapper::ResolveState")

    wifi_setup = resolve.index("if (device_state == kDeviceStateWifiConfiguring)")
    claim_overlay = resolve.index("switch (claim_substate)")

    assert wifi_setup < claim_overlay
