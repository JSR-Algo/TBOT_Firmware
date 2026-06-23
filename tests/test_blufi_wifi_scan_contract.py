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


def test_blufi_wifi_scan_done_handler_is_registered_for_sta_mode_scans():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    body = function_body(source, "bool Blufi::start_wifi_scan")

    assert "EnsureWifiScanEventHandlerRegistered()" in body
    assert body.index("EnsureWifiScanEventHandlerRegistered()") < body.index("esp_wifi_scan_start")
    assert "bool EnsureWifiScanEventHandlerRegistered();" in header


def test_blufi_wifi_scan_handler_registration_is_single_owner_and_unregistered_on_deinit():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    ensure_body = function_body(source, "bool Blufi::EnsureWifiScanEventHandlerRegistered")
    deinit_body = function_body(source, "esp_err_t Blufi::deinit")

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
    assert get_list.index("!m_ap_records.empty() && IsWifiScanCacheFresh()") < get_list.index("_send_wifi_list();")
    assert "m_ap_records.clear();" in get_list

def test_blufi_init_resets_wifi_scan_cache_capture_without_starting_eager_scan():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "esp_err_t Blufi::init")

    assert "m_scan_should_save_ssid = true;" in body
    assert body.index("m_scan_should_save_ssid = true;") < body.index("_controller_init()")
    assert "start_wifi_scan();" not in body

def test_blufi_init_resets_ble_timeout_latch_for_fresh_setup_window():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "esp_err_t Blufi::init")

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
