import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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


def test_blufi_wifi_scan_done_handler_is_registered_for_sta_mode_scans():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    body = function_body(source, "bool Blufi::start_wifi_scan")

    assert "EnsureWifiScanEventHandlerRegistered()" in body
    assert body.index("EnsureWifiScanEventHandlerRegistered()") < body.index("esp_wifi_scan_start")
    assert "bool EnsureWifiScanEventHandlerRegistered();" in header


def test_blufi_wifi_scan_is_passive_to_preserve_internal_dma_heap():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "bool Blufi::start_wifi_scan")

    assert "wifi_scan_config_t scan_config" in body
    assert "scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE" in body
    assert "scan_config.scan_time.passive = WIFI_PASSIVE_SCAN_DEFAULT_TIME" in body
    scan_start_first_args = re.findall(r"esp_wifi_scan_start\(\s*([^,]+),", body)
    assert scan_start_first_args
    assert all(arg.strip() == "&scan_config" for arg in scan_start_first_args)
    assert "esp_wifi_scan_start(NULL, false)" not in body
    assert "esp_wifi_scan_start(nullptr, false)" not in body


def test_blufi_stops_idle_wifi_radio_before_allocating_ble_list_buffers():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_send_wifi_list")

    stop_guard = body.index("!WifiManager::GetInstance().IsConnected()")
    stop_wifi = body.index("WifiManager::GetInstance().StopRadio()", stop_guard)
    send_list = body.index("esp_err_t err = esp_blufi_send_wifi_list")

    assert stop_guard < stop_wifi < send_list
    assert "Deinitialize()" not in body


def test_wifi_manager_stop_radio_releases_runtime_buffers_without_deinitializing_driver():
    source = read("components/esp-wifi-connect/wifi_manager.cc")
    header = read("components/esp-wifi-connect/include/wifi_manager.h")
    body = function_body(source, "bool WifiManager::StopRadio")

    assert "bool StopRadio();" in header
    assert "station_->Stop()" in body
    assert "config_ap_->Stop()" in body
    assert "esp_wifi_stop()" in body
    assert "esp_wifi_deinit()" not in body
    assert "station_.reset()" not in body
    assert "config_ap_.reset()" not in body
    assert "initialized_ = false" not in body


def test_esp32s3_blufi_build_disables_unused_ble_features_to_preserve_dma_heap():
    defaults = read("sdkconfig.defaults.esp32s3")

    assert "CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n" in defaults
    assert "CONFIG_BT_BLE_SMP_ENABLE=n" in defaults
    assert "CONFIG_BT_BLE_42_DTM_TEST_EN=n" in defaults
    assert "CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y" in defaults
    assert "CONFIG_BT_BLE_42_ADV_EN=y" in defaults


def test_esp32s3_blufi_build_is_sized_for_one_peripheral_connection():
    defaults = read("sdkconfig.defaults.esp32s3")

    assert "CONFIG_BT_GATTC_ENABLE=n" in defaults
    # Bluedroid reserves GAP and GATT profiles before registering BluFi.
    assert "CONFIG_BT_GATT_MAX_SR_PROFILES=3" in defaults
    assert "CONFIG_BT_GATT_MAX_SR_ATTRIBUTES=16" in defaults
    assert "CONFIG_BT_ACL_CONNECTIONS=1" in defaults
    assert "CONFIG_BT_MULTI_CONNECTION_ENBALE=n" in defaults
    assert "CONFIG_BT_CTRL_BLE_MAX_ACT=2" in defaults
    assert "CONFIG_BT_CTRL_DTM_ENABLE=n" in defaults
    assert "CONFIG_BT_CTRL_BLE_SCAN=n" in defaults
    assert "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE=n" in defaults


def test_blufi_scan_reinitializes_wifi_after_list_dispatch_teardown():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "bool Blufi::start_wifi_scan")

    initialize = body.index("wifi_manager.Initialize()")
    get_mode = body.index("esp_wifi_get_mode")
    null_mode = body.index("current_mode == WIFI_MODE_NULL")

    assert initialize < get_mode < null_mode


def test_blufi_wifi_scan_caps_application_owned_candidates():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_wifi_scan_event_handler")

    cap = "ap_num = std::min<uint16_t>(ap_num, kMaxBlufiWifiScanCandidates);"
    assert "kMaxBlufiWifiScanCandidates" in source
    assert cap in body
    assert body.index(cap) < body.index("m_ap_records.resize(ap_num)")
    assert body.index(cap) < body.index("esp_wifi_scan_get_ap_records(&ap_num")


def test_blufi_wifi_list_dispatch_is_deferred_and_guarded_until_scan_callback_returns():
    source = read("main/boards/common/blufi.cpp")
    handler = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert "ScheduleWifiListSend" in handler
    assert "_send_wifi_list();" not in handler

    helper = function_body(source, "void Blufi::ScheduleWifiListSend")
    assert "Application::GetInstance().Schedule" in helper
    assert "RunIfSetupGenerationCurrent" in helper
    assert "m_ble_is_connected" in helper


def test_blufi_deferred_wifi_list_is_bound_to_exact_ble_connection():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    handler = function_body(source, "void Blufi::_wifi_scan_event_handler")
    helper = function_body(source, "void Blufi::ScheduleWifiListSend")
    connect = source[
        source.index("case ESP_BLUFI_EVENT_BLE_CONNECT") :
        source.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT")
    ]
    disconnect = source[
        source.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT") :
        source.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST")
    ]

    assert "uint64_t expected_ble_session_state" in header
    assert "uint64_t expected_ble_connection_epoch" in header
    assert "ble_connection_epoch_" in header
    assert "ble_session_state_.load(std::memory_order_acquire)" in handler
    assert "ble_connection_epoch_.load(std::memory_order_acquire)" in handler
    assert "expected_ble_session_state" in helper
    assert "expected_ble_connection_epoch" in helper
    assert "current_ble_session_state != expected_ble_session_state" in helper
    assert "DecodeBleSessionPhase(expected_ble_session_state)" in helper
    assert "DecodeBleSessionPhase(current_ble_session_state)" in helper
    assert helper.count("BleSessionPhase::kConnected") >= 2
    assert "current_ble_connection_epoch != expected_ble_connection_epoch" in helper
    assert "ble_connection_epoch_.fetch_add(1" in connect
    assert "provisioning_finalization_mutex_" in handler
    assert "provisioning_finalization_mutex_" in connect
    assert "provisioning_finalization_mutex_" in disconnect
    assert "m_send_list_after_scan = false;" in disconnect


def test_blufi_wifi_list_retry_waits_for_owned_deferred_dispatch():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    handler = function_body(source, "void Blufi::_wifi_scan_event_handler")
    event_body = function_body(source, "void Blufi::_handle_event")
    get_list = event_body[
        event_body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") :
        event_body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")
    ]

    assert "m_wifi_list_dispatch_pending_epoch_" in header
    pending_check = get_list.index("m_wifi_list_dispatch_pending_epoch_")
    assert pending_check < get_list.index("if (m_scan_in_progress)")
    assert pending_check < get_list.index("IsWifiScanCacheFresh()")
    assert pending_check < get_list.index("m_ap_records.clear();")

    assert "std::vector<wifi_ap_record_t> owned_ap_records" in handler
    transfer = "owned_ap_records.swap(self->m_ap_records);"
    assert transfer in handler
    assert handler.index(transfer) < handler.index("self->m_scan_in_progress = false;")
    assert "std::move(owned_ap_records)" in handler


def test_blufi_stale_deferred_dispatch_only_destroys_its_owned_records():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    helper = function_body(source, "void Blufi::ScheduleWifiListSend")
    restart = function_body(source, "esp_err_t Blufi::RestartForSetup")
    event_body = function_body(source, "void Blufi::_handle_event")
    disconnect = event_body[
        event_body.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT") :
        event_body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST")
    ]

    assert "uint64_t expected_wifi_list_dispatch_epoch" in header
    assert "std::vector<wifi_ap_record_t> ap_records" in header
    assert "m_wifi_list_dispatch_epoch_" in header
    assert "m_wifi_list_dispatch_pending_epoch_" in header
    assert "expected_wifi_list_dispatch_epoch" in helper
    assert "current_wifi_list_dispatch_epoch != expected_wifi_list_dispatch_epoch" in helper
    assert "compare_exchange_strong" in helper
    assert "_send_wifi_list(std::move(ap_records))" in helper
    assert "swap(m_ap_records)" not in helper
    assert "m_ap_records.clear()" not in helper
    assert "m_wifi_list_dispatch_epoch_.fetch_add(1" in restart
    assert "m_wifi_list_dispatch_epoch_.fetch_add(1" in disconnect


def test_blufi_wifi_scan_handler_registration_is_single_owner_and_unregistered_on_deinit():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    ensure_body = function_body(source, "bool Blufi::EnsureWifiScanEventHandlerRegistered")
    deinit_body = function_body(source, "esp_err_t Blufi::_deinit_impl")

    assert "scan_event_instance_" in header
    assert "esp_event_handler_instance_register" in ensure_body
    assert "WIFI_EVENT_SCAN_DONE" in ensure_body
    assert "scan_event_instance_ != nullptr" in ensure_body
    assert "esp_event_handler_instance_unregister" in deinit_body
    assert "scan_event_instance_ = nullptr" in deinit_body

def test_blufi_scan_done_handler_ignores_scan_events_it_did_not_start():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert "!self->m_scan_in_progress" in body
    assert body.index("!self->m_scan_in_progress") < body.index("esp_wifi_scan_get_ap_num")
    assert "Ignoring WiFi scan done event not owned by BluFi" in body

def test_blufi_wifi_list_requests_refresh_stale_cached_scan_results():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    body = function_body(source, "void Blufi::_handle_event")
    get_list = body[body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") : body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")]

    assert "m_ap_records_updated_us" in header
    assert "kWifiScanCacheMaxAgeUs" in header
    assert "IsWifiScanCacheFresh()" in header
    assert "IsWifiScanCacheFresh()" in source
    assert "!m_ap_records.empty() && IsWifiScanCacheFresh()" in get_list
    fresh_idx = get_list.index("!m_ap_records.empty() && IsWifiScanCacheFresh()")
    assert "_send_wifi_list(" in get_list[fresh_idx:]
    assert "m_ap_records.clear();" in get_list

def test_blufi_init_resets_wifi_scan_cache_capture_without_starting_eager_scan():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "esp_err_t Blufi::_init_impl")

    assert "m_scan_should_save_ssid = true;" in body
    assert body.index("m_scan_should_save_ssid = true;") < body.index("_controller_init()")
    assert "start_wifi_scan();" not in body

def test_blufi_init_resets_ble_timeout_latch_for_fresh_setup_window():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "esp_err_t Blufi::_init_impl")

    assert "ble_timed_out_ = false;" in body
    assert body.index("ble_timed_out_ = false;") < body.index("_controller_init()")

def test_blufi_wifi_list_marks_inflight_scan_results_as_app_visible():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_handle_event")
    get_list = body[body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") : body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")]
    inflight_branch = get_list[get_list.index("if (m_scan_in_progress)") : get_list.index("break;", get_list.index("if (m_scan_in_progress)"))]

    assert "m_scan_should_save_ssid = true;" in inflight_branch
    assert inflight_branch.index("m_scan_should_save_ssid = true;") < inflight_branch.index("m_send_list_after_scan = true;")

def test_wifi_station_does_not_consume_blufi_owned_scan_results():
    source = read("managed_components/78__esp-wifi-connect/wifi_station.cc")
    header = read("managed_components/78__esp-wifi-connect/include/wifi_station.h")
    handler = function_body(source, "void WifiStation::WifiEventHandler")

    assert "scan_in_progress_" in header
    assert "bool StartOwnedScan();" in header
    assert "bool WifiStation::StartOwnedScan()" in source
    assert "!this_->scan_in_progress_" in handler
    assert handler.index("!this_->scan_in_progress_") < handler.index("HandleScanResult();")
    assert "Ignoring WiFi scan done event not owned by WifiStation" in handler
