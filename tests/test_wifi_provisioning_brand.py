import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_BRAND_TEXT = ("Xiaozhi", "XiaoZhi", "小智")
CJK_SCRIPT_PATTERN = re.compile(r"[\u3040-\u30ff\u3400-\u9fff\uf900-\ufaff\uac00-\ud7af]")


def read(path: str) -> str:
    candidate = ROOT / path
    if not candidate.exists() and path.startswith("managed_components/78__esp-wifi-connect/"):
        candidate = ROOT / path.replace(
            "managed_components/78__esp-wifi-connect",
            "components/esp-wifi-connect",
            1,
        )
    return candidate.read_text(encoding="utf-8")

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


def test_wifi_provisioning_uses_tbot_brand_names():
    wifi_board = read("main/boards/common/wifi_board.cc")
    blufi = read("main/boards/common/blufi.cpp")

    assert 'config.ssid_prefix = "TBot";' in wifi_board
    assert 'config.ssid_prefix = "Xiaozhi";' not in wifi_board
    assert 'static std::string GetBlufiDeviceName()' in blufi
    assert 'StartTbotBlufiAdvertising' in blufi
    assert 'esp_ble_gap_set_device_name(device_name)' in blufi
    assert 'TBOT-%02X%02X%02X%02X%02X%02X' in blufi
    assert '#define BLUFI_DEVICE_NAME "TBot-Blufi"' not in blufi
    assert '#define BLUFI_DEVICE_NAME "Xiaozhi-Blufi"' not in blufi


def test_blufi_compact_advertising_waits_for_adv_and_scan_response_payloads():
    blufi = read("main/boards/common/blufi.cpp")

    assert "TbotBlufiGapEventHandler" in blufi
    assert "esp_ble_gap_register_callback(TbotBlufiGapEventHandler)" in blufi

    handler = function_body(blufi, "static void TbotBlufiGapEventHandler")
    assert "esp_blufi_gap_event_handler(event, param);" in handler
    assert "ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT" in handler
    assert "ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT" in handler
    assert "ESP_GAP_BLE_ADV_START_COMPLETE_EVT" in handler
    assert "CompleteCompactConfigAndSubmit" in handler
    assert "CompleteStartAndMaybeFallback" in handler

    configure = function_body(blufi, "static void StartTbotBlufiAdvertising")
    assert "esp_ble_gap_config_adv_data_raw" in configure
    assert "esp_ble_gap_config_scan_rsp_data_raw" in configure
    assert "esp_ble_gap_start_advertising" not in configure
    assert "uint8_t adv_raw[31]" in configure
    assert "0x08" in configure
    assert "std::memcpy(adv_raw + adv_len" in configure
    assert "name_len == std::strlen(device_name) ? 0x09 : 0x08" in configure

    init_body = function_body(blufi, "esp_err_t Blufi::_init_impl")
    deinit_body = function_body(blufi, "esp_err_t Blufi::_deinit_impl")
    assert "InvalidateTbotBlufiAdvertising();" in init_body
    assert "InvalidateTbotBlufiAdvertising();" in deinit_body
    assert init_body.index("InvalidateTbotBlufiAdvertising();") < init_body.index("_controller_init()")
    assert deinit_body.index("InvalidateTbotBlufiAdvertising();") < deinit_body.index("_host_deinit()")


def test_blufi_compact_advertising_ignores_callbacks_from_prior_lifecycle():
    blufi = read("main/boards/common/blufi.cpp")

    assert "TbotBlufiAdvertisingLedger" in blufi
    assert "BeginCompactAndSubmit" in blufi
    assert "CompleteCompactConfigAndSubmit" in blufi
    assert "CompleteStartAndMaybeFallback" in blufi

    handler = function_body(blufi, "static void TbotBlufiGapEventHandler")
    assert "CompleteDefaultConfigAndSubmit" in handler
    assert "CompleteCompactConfigAndSubmit" in handler
    assert "CompleteStartAndMaybeFallback" in handler


def test_blufi_default_fallback_owns_config_and_start_callback_epochs():
    blufi = read("main/boards/common/blufi.cpp")
    ledger = read("main/boards/common/blufi_advertising_ledger.h")

    assert '#include "blufi_advertising_ledger.h"' in blufi
    assert "TbotBlufiAdvertisingLedger tbot_adv_ledger" in blufi

    claim_and_submit = function_body(
        ledger, "std::optional<Owner> ClaimDefaultFallbackAndSubmit"
    )
    claim = claim_and_submit.index("ClaimDefaultFallback()")
    start_default = claim_and_submit.index("submit_default();")
    assert claim < start_default
    assert "std::lock_guard" not in claim_and_submit[claim:start_default]

    handler = function_body(blufi, "static void TbotBlufiGapEventHandler")
    assert "if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT)" in handler
    assert "if (param == nullptr)" in handler
    structured_complete = handler.index("ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT")
    transfer_epoch = handler.index("CompleteDefaultConfigAndSubmit", structured_complete)
    start_default = handler.index(
        "esp_ble_gap_start_advertising(&tbot_adv_params)", structured_complete
    )
    assert transfer_epoch < start_default
    assert "kCompactStart" in ledger
    assert "kDefaultStart" in ledger


def test_blufi_advertising_ledger_resets_only_after_successful_host_deinit():
    blufi = read("main/boards/common/blufi.cpp")
    invalidate = function_body(blufi, "void InvalidateTbotBlufiAdvertising")
    host_deinit = function_body(blufi, "esp_err_t Blufi::_host_deinit")

    assert "ResetTbotBlufiAdvertisingAfterSuccessfulHostDeinit" not in invalidate
    reset = host_deinit.index("ResetTbotBlufiAdvertisingAfterSuccessfulHostDeinit")
    deinit = host_deinit.index("esp_bluedroid_deinit()")
    success = host_deinit.index("host_initialized_ = false;")
    assert deinit < success < reset
    init = function_body(blufi, "esp_err_t Blufi::_init_impl")
    gap_register = function_body(blufi, "esp_err_t Blufi::_gap_register_callback")
    assert "ActivateTbotBlufiAdvertisingAfterSuccessfulHostInit" not in init
    register = gap_register.index("esp_ble_gap_register_callback")
    activate = gap_register.index("ActivateTbotBlufiAdvertisingAfterSuccessfulHostInit")
    profile_init = gap_register.index("esp_blufi_profile_init()")
    assert register < activate < profile_init
    activation_failure = gap_register[activate:profile_init]
    assert "InvalidateTbotBlufiAdvertising()" in activation_failure


def test_blufi_gap_handler_gates_advertising_but_delegates_unrelated_events():
    blufi = read("main/boards/common/blufi.cpp")
    handler = function_body(blufi, "static void TbotBlufiGapEventHandler")

    structured = handler.index("ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT")
    owned_start = handler.index("CompleteDefaultConfigAndSubmit", structured)
    raw = handler.index("ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT")
    start = handler.index("ESP_GAP_BLE_ADV_START_COMPLETE_EVT")
    unrelated = handler.index("esp_blufi_gap_event_handler(event, param)", start)
    assert structured < owned_start < raw < start < unrelated
    assert "default:" in handler[:unrelated]
    assert "esp_blufi_gap_event_handler(event, param)" not in handler[structured:raw]


def test_blufi_default_start_uses_idf_equivalent_params_with_exact_cancellation():
    blufi = read("main/boards/common/blufi.cpp")
    ledger = read("main/boards/common/blufi_advertising_ledger.h")
    handler = function_body(blufi, "static void TbotBlufiGapEventHandler")

    assert "CompleteDefaultConfigAndSubmit" in handler
    assert "param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS" in handler
    assert "esp_ble_gap_start_advertising(&tbot_adv_params)" in handler
    assert "esp_blufi_gap_event_handler(event, param)" not in handler[
        handler.index("ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT"):
        handler.index("ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT")
    ]
    assert "Cancel(result.start_owner)" in ledger

    params = blufi[blufi.index("esp_ble_adv_params_t tbot_adv_params"):]
    assert ".adv_int_min = 0x100" in params
    assert ".adv_int_max = 0x100" in params
    assert ".adv_type = ADV_TYPE_IND" in params
    assert ".own_addr_type = BLE_ADDR_TYPE_PUBLIC" in params
    assert ".channel_map = ADV_CHNL_ALL" in params
    assert ".adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY" in params


def test_blufi_never_logs_wifi_password_values():
    blufi = read("main/boards/common/blufi.cpp")

    assert 'ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD : %s"' not in blufi
    assert 'm_sta_config.sta.password' not in [
        line.strip()
        for line in blufi.splitlines()
        if 'ESP_LOG' in line and 'password' in line.lower()
    ]


def test_wifi_config_page_defaults_to_vietnamese_without_chinese_locale():
    html = read("managed_components/78__esp-wifi-connect/assets/wifi_configuration.html")
    wifi_manager_h = read("managed_components/78__esp-wifi-connect/include/wifi_manager.h")
    wifi_config_ap = read("managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc")

    assert "return 'vi-VN';" in html
    assert "defaulting to Vietnamese" in html
    assert 'std::string language = "vi-VN";' in wifi_manager_h
    assert 'language_ = "vi-VN";' in wifi_config_ap
    assert 'std::string language = "zh-CN";' not in wifi_manager_h
    assert 'language_ = "zh-CN";' not in wifi_config_ap
    assert '<option value="zh-CN">' not in html
    assert '<option value="zh-TW">' not in html
    assert "'zh-CN':" not in html
    assert "'zh-TW':" not in html
    assert "简体中文" not in html
    assert "繁體中文" not in html
    assert "网络配置" not in html
    assert "连接 Wi-Fi 时记住 BSSID" not in html

def test_wifi_config_pages_do_not_render_cjk_locale_text():
    config_html = read("managed_components/78__esp-wifi-connect/assets/wifi_configuration.html")
    done_html = read("managed_components/78__esp-wifi-connect/assets/wifi_configuration_done.html")

    assert '<option value="ja-JP">' not in config_html
    assert '<option value="ko-KR">' not in config_html
    assert "'ja-JP':" not in config_html
    assert "'ko-KR':" not in config_html
    assert "'ja': 'ja-JP'" not in config_html
    assert "'ko': 'ko-KR'" not in config_html
    assert CJK_SCRIPT_PATTERN.search(config_html) is None
    assert CJK_SCRIPT_PATTERN.search(done_html) is None
    assert "Cấu hình Wi-Fi thành công" in done_html


def test_blufi_config_mode_is_wired_into_firmware():
    wifi_board = read("main/boards/common/wifi_board.cc")
    cmake = read("main/CMakeLists.txt")
    kconfig = read("main/Kconfig.projbuild")
    defaults = read("sdkconfig.defaults")

    assert '#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING' in wifi_board
    assert 'auto &blufi = Blufi::GetInstance();' in wifi_board
    assert 'blufi.TryReserveProvisioningSession()' in wifi_board
    assert 'if (CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)' in cmake
    assert 'list(APPEND SOURCES "boards/common/blufi.cpp")' in cmake
    assert 'config USE_ESP_BLUFI_WIFI_PROVISIONING' in kconfig
    assert 'select BT_BLE_BLUFI_ENABLE' in kconfig
    assert '# CONFIG_USE_HOTSPOT_WIFI_PROVISIONING is not set' in defaults
    assert 'CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING=y' in defaults
    assert 'CONFIG_BT_BLUEDROID_ENABLED=y' in defaults

def test_blufi_config_mode_reopens_robot_scan_after_ble_timeout():
    wifi_board = read("main/boards/common/wifi_board.cc")
    start = wifi_board.index("void WifiBoard::StartWifiConfigMode(")
    body = wifi_board[start : wifi_board.index("void WifiBoard::EnterWifiConfigMode()", start)]

    restart_idx = body.index("blufi.RestartForSetup();")
    timer_idx = body.index("blufi.StartBleSetupTimeout")
    assert restart_idx < timer_idx


def test_blufi_only_build_does_not_reference_softap_timeout_without_hotspot_guard():
    wifi_board = read("main/boards/common/wifi_board.cc")
    start = wifi_board.index("void WifiBoard::OnApSetupTimeout")
    end = wifi_board.index("void WifiBoard::StartApSetupTimeout", start)
    body = wifi_board[start:end]

    timeout_idx = body.index("CONFIG_AP_SETUP_TIMEOUT_SEC")
    guard_idx = body.index("#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING")
    assert guard_idx < timeout_idx

def test_wifi_config_releases_wake_word_resources_before_ble_init():
    wifi_board = read("main/boards/common/wifi_board.cc")
    audio_h = read("main/audio/audio_service.h")
    audio_cc = read("main/audio/audio_service.cc")
    afe_h = read("main/audio/wake_words/afe_wake_word.h")
    afe_cc = read("main/audio/wake_words/afe_wake_word.cc")

    start = wifi_board.index("void WifiBoard::StartWifiConfigMode(")
    body = wifi_board[start : wifi_board.index("void WifiBoard::EnterWifiConfigMode()", start)]
    release_idx = body.index("BeginWifiProvisioning")
    restart_idx = body.index("blufi.RestartForSetup();")
    assert release_idx < restart_idx

    assert "WifiProvisioningBeginResult BeginWifiProvisioning();" in audio_h
    release_start = audio_cc.index("AudioService::WifiProvisioningBeginResult AudioService::BeginWifiProvisioning()")
    release_body = audio_cc[release_start:audio_cc.index("void AudioService::EnableVoiceProcessing", release_start)]
    prewarm_body = function_body(audio_cc, "void AudioService::PrewarmWakeWord")
    assert "WakeWordLifecycleController wake_word_lifecycle_;" in audio_h
    assert "wake_word_lifecycle_.TryAcquirePrewarm" in prewarm_body
    assert "wake_word_lifecycle_.BeginProvisioningAndQuiesce" in release_body
    assert "Shutdown(0)" in release_body
    assert "Shutdown(5000)" in release_body
    assert "wake_word_.reset();" in release_body
    assert "wake_word_initialized_ = false;" in release_body

    enable_start = audio_cc.index("void AudioService::EnableWakeWordDetection")
    enable_body = audio_cc[enable_start:audio_cc.index("void AudioService::EnableVoiceProcessing", enable_start)]
    assert "CreateWakeWordIfAvailable();" in enable_body

    assert "std::atomic<TaskHandle_t> audio_detection_task_handle_{nullptr};" in afe_h
    assert "bool Shutdown(uint32_t timeout_ms) override;" in afe_h
    shutdown_body = function_body(afe_cc, "bool AfeWakeWord::Shutdown")
    assert "const EventBits_t required = DETECTION_EXITED_EVENT;" in shutdown_body
    assert "ENCODE_EXITED_EVENT" not in shutdown_body
    assert "xEventGroupWaitBits" in shutdown_body

def test_assets_model_load_does_not_create_afe_before_claimed_audio_gate():
    audio_cc = read("main/audio/audio_service.cc")

    set_models_body = function_body(audio_cc, "void AudioService::SetModelsList")
    assert "models_list_ = models_list;" in set_models_body
    assert "CreateWakeWordIfAvailable" not in set_models_body

    enable_body = function_body(audio_cc, "void AudioService::EnableWakeWordDetection")
    prewarm_body = function_body(audio_cc, "void AudioService::PrewarmWakeWord")
    assert "CreateWakeWordIfAvailable();" in enable_body
    assert "CreateWakeWordIfAvailable();" in prewarm_body

def test_wifi_config_mode_accepts_startup_activation_window():
    policy = read("main/wifi_config_entry_policy.h")

    assert "state == kDeviceStateActivating" in policy
    assert "state == kDeviceStateConnecting" in policy

def test_wifi_config_mode_can_be_rearmed_while_already_configuring():
    policy = read("main/wifi_config_entry_policy.h")

    assert "state == kDeviceStateWifiConfiguring" in policy

def test_wifi_config_entry_ignores_active_lesson_before_setup_side_effects():
    wifi_board = read("main/boards/common/wifi_board.cc")
    enter_body = function_body(wifi_board, "void WifiBoard::EnterWifiConfigMode")
    start_body = function_body(wifi_board, "void WifiBoard::StartWifiConfigMode")

    assert "app.IsLessonRuntimeActive()" in enter_body
    assert enter_body.index("app.IsLessonRuntimeActive()") < enter_body.index("RequestWifiConfigMode")
    guard = enter_body[
        enter_body.index("app.IsLessonRuntimeActive()") :
        enter_body.index("RequestWifiConfigMode(true)")
    ]
    assert "return;" in guard
    assert "ShowNotification" not in guard
    assert "ResetProtocol" not in guard
    assert "StopStation" not in guard
    assert "RequestWifiConfigMode" not in guard
    assert "ShowNotification" in start_body
    assert "PrepareWifiConfigEntry" in start_body
    assert "StopStation" in start_body

def test_runtime_state_machine_can_interrupt_backend_connect_for_wifi_config():
    state_machine = read("main/device_state_machine.cc")
    connecting_start = state_machine.index("case kDeviceStateConnecting:")
    connecting_body = state_machine[connecting_start : state_machine.index("case kDeviceStateListening:", connecting_start)]

    assert "to == kDeviceStateWifiConfiguring" in connecting_body


def test_start_network_preserves_boot_reprovisioning_during_startup():
    wifi_board = read("main/boards/common/wifi_board.cc")
    start = wifi_board.index("void WifiBoard::StartNetwork()")
    start_body = wifi_board[start : wifi_board.index("void WifiBoard::TryWifiConnect()", start)]

    assert "StartNetwork skipped auto-connect because config mode is already active" in start_body
    assert "if (in_config_mode_)" in start_body
    assert start_body.index("if (in_config_mode_)") < start_body.index("TryWifiConnect();")


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
        for path in ROOT.rglob("*")
        if path.is_file()
        and path.suffix in text_suffixes
        and ignored_dirs.isdisjoint(path.relative_to(ROOT).parts)
    ]
    files.append(ROOT / "main" / "Kconfig.projbuild")

    offenders = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        for forbidden in FORBIDDEN_BRAND_TEXT:
            if forbidden in text:
                offenders.append(f"{path.relative_to(ROOT)} contains {forbidden}")

    assert offenders == []
