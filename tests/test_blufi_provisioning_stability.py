"""Source-level regression tests locking US-005 BLUFI provisioning-stability invariants.

These are static assertions over the firmware .cc/.h text (no device needed),
following the convention in tests/test_wifi_provisioning_brand.py:
module-level ROOT, a read(path) helper, and test_* functions asserting on
substrings / regex / relative ordering of real markers in the source.

Each test is tagged with its invariant id (FW1..FW8) so a failure is traceable
back to the audited invariant.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _start_wifi_config_body(wifi_board: str) -> str:
    """The body of WifiBoard::StartWifiConfigMode() up to the next function."""
    start = wifi_board.index("void WifiBoard::StartWifiConfigMode(")
    end = wifi_board.index("void WifiBoard::EnterWifiConfigMode()", start)
    return wifi_board[start:end]

def _function_body(text: str, signature: str) -> str:
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
# FW1: wifi_board.cc — BeginWifiProvisioning appears BEFORE
#      RestartForSetup() in the explicit Wi-Fi-config setup path.
# ---------------------------------------------------------------------------
def test_fw1_release_wake_word_before_ble_init_in_config_path():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    # The wake-word resource release must run before BLE setup restart, otherwise
    # the AFE detection task contends with BLE during provisioning.
    assert "BeginWifiProvisioning" in body
    assert "blufi.RestartForSetup();" in body
    release_idx = body.index("BeginWifiProvisioning")
    restart_idx = body.index("blufi.RestartForSetup();")
    assert release_idx < restart_idx


# ---------------------------------------------------------------------------
# FW2: wifi_board.cc — explicit setup re-entry restarts the BLE generation
#      before re-arming StartBleSetupTimeout.
# ---------------------------------------------------------------------------
def test_fw2_ble_timeout_accepted_for_explicit_setup_reentry():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    # The restart helper handles off, timeout, and already-advertising states so
    # every explicit setup entry gets a fresh generation and scan window.
    restart_idx = body.index("blufi.RestartForSetup();")
    rearm_idx = body.index("blufi.StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC)")
    assert restart_idx < rearm_idx


# ---------------------------------------------------------------------------
# FW3: blufi.cpp — station Wi-Fi connect timeout is 60000ms or greater
#      (kConnectTimeoutMs).
# ---------------------------------------------------------------------------
def test_fw3_station_connect_timeout_is_at_least_60s():
    blufi = read("main/boards/common/blufi.cpp")

    assert "kConnectTimeoutMs" in blufi
    match = re.search(r"constexpr\s+int\s+kConnectTimeoutMs\s*=\s*(\d+)\s*;", blufi)
    assert match is not None, "kConnectTimeoutMs declaration not found"
    assert int(match.group(1)) >= 60000

    # The poll loop must actually bound the wait on this constant.
    assert "waited_ms < kConnectTimeoutMs && !wifi.IsConnected()" in blufi

# ---------------------------------------------------------------------------
# FW3b: blufi.cpp — the BLE hard-timeout must not tear down an active phone
#       GATT/BluFi session. Hardware proof on 2026-06-19 showed Android was
#       connected and discovering services when the one-shot timer fired,
#       causing `Device ... was disconnected` before Wi-Fi scan could complete.
# ---------------------------------------------------------------------------
def test_fw3b_ble_timeout_defers_teardown_while_phone_session_is_active():
    blufi = read("main/boards/common/blufi.cpp")
    timeout_body = _function_body(blufi, "void Blufi::_ble_setup_timeout_cb")

    assert "m_ble_is_connected" in timeout_body
    assert "m_sta_is_connecting" in timeout_body
    assert "StartBleSetupTimeout" in timeout_body
    active_idx = min(timeout_body.index("m_ble_is_connected"), timeout_body.index("m_sta_is_connecting"))
    rearm_idx = timeout_body.index("StartBleSetupTimeout")
    timed_out_idx = timeout_body.index("ble_timed_out_ = true")
    deinit_idx = timeout_body.index("self->deinit();")

    assert active_idx < rearm_idx < timed_out_idx < deinit_idx
    assert "return;" in timeout_body[active_idx:timed_out_idx]


# ---------------------------------------------------------------------------
# FW3c: blufi.cpp — the AP list sent over BluFi must be small and strongest
#       first. Hardware proof on 2026-06-19 showed sending 24 AP records made
#       `esp_blufi_send_encap` back up and the BLE stack report malloc failures,
#       so provisioning fell back to manual SSID entry even though scan worked.
# ---------------------------------------------------------------------------
def test_fw3c_wifi_list_sent_over_blufi_is_capped_and_strongest_first():
    blufi = read("main/boards/common/blufi.cpp")
    send_body = _function_body(blufi, "void Blufi::_send_wifi_list")

    cap_match = re.search(
        r"constexpr\s+size_t\s+kMaxBlufiWifiListApRecords\s*=\s*(\d+)\s*;",
        blufi,
    )
    assert cap_match is not None, "BluFi Wi-Fi list cap is not declared"
    assert 1 <= int(cap_match.group(1)) <= 4

    assert "strongest" in send_body
    assert ".rssi" in send_body
    assert "already_selected" in send_body
    assert "kMaxBlufiWifiListApRecords" in send_body
    assert "blufi_ap_count < blufi_ap_list.size()" in send_body

    select_idx = send_body.index("const wifi_ap_record_t* strongest")
    cap_idx = send_body.index("blufi_ap_count < blufi_ap_list.size()")
    send_idx = send_body.index("esp_blufi_send_wifi_list")
    assert cap_idx < select_idx < send_idx


# ---------------------------------------------------------------------------
# FW3c-memory: a dense scan must not keep/copy heap-proportional AP containers
#       while BluFi allocates its BTC and notification buffers. Physical Android
#       testing with 22 APs left the largest internal block near 1.4 KiB and the
#       controller logged `BLE_INIT: Malloc failed` exactly at list dispatch.
# ---------------------------------------------------------------------------
def test_fw3c_wifi_list_releases_scan_heap_before_blufi_dispatch():
    blufi = read("main/boards/common/blufi.cpp")
    send_body = _function_body(blufi, "void Blufi::_send_wifi_list")

    assert "std::array<esp_blufi_ap_record_t, kMaxBlufiWifiListApRecords>" in send_body
    assert "std::vector<wifi_ap_record_t> sorted_ap_records" not in send_body
    assert "std::vector<std::string>" not in send_body
    assert "std::vector<esp_blufi_ap_record_t>" not in send_body
    assert "std::stable_sort" not in send_body

    release_idx = send_body.index("std::vector<wifi_ap_record_t>().swap(m_ap_records)")
    send_idx = send_body.index("esp_err_t err = esp_blufi_send_wifi_list")
    assert release_idx < send_idx


def test_fw3c_scan_driver_records_are_released_on_all_exit_paths():
    blufi = read("main/boards/common/blufi.cpp")
    scan_body = _function_body(blufi, "void Blufi::_wifi_scan_event_handler")

    assert scan_body.count("esp_wifi_clear_ap_list()") >= 2


# ---------------------------------------------------------------------------
# FW3d: blufi.cpp — after dispatching the Wi-Fi list, do not immediately start
#       another Wi-Fi scan from the same path. The phone already has a response
#       pending over BLE; a concurrent refresh scan raises heap pressure during
#       the fragile BluFi notification burst.
# ---------------------------------------------------------------------------
def test_fw3d_wifi_list_send_path_does_not_start_overlapping_refresh_scan():
    blufi = read("main/boards/common/blufi.cpp")
    send_body = _function_body(blufi, "void Blufi::_send_wifi_list")

    assert "esp_blufi_send_wifi_list" in send_body
    after_send = send_body[send_body.index("esp_blufi_send_wifi_list") :]
    assert "start_wifi_scan()" not in after_send


# ---------------------------------------------------------------------------
# FW3e: blufi.cpp — scan completion must not log every AP while a phone is
#       waiting for BluFi notifications. In dense RF environments this floods
#       serial work and heap use immediately before esp_blufi_send_wifi_list().
# ---------------------------------------------------------------------------
def test_fw3e_wifi_scan_done_does_not_log_every_ap_before_blufi_send():
    blufi = read("main/boards/common/blufi.cpp")
    scan_body = _function_body(blufi, "void Blufi::_wifi_scan_event_handler")

    assert "Found %d APs" in scan_body
    assert "SSID: %s" not in scan_body
    assert "for (const auto& ap : self->m_ap_records)" not in scan_body


# ---------------------------------------------------------------------------
# FW3f: blufi.cpp — hardware proof on 2026-06-19 showed Android delivered
#       SSID/password, then the final CONNECT_TO_AP control frame was not
#       observed by firmware before BLE disconnected. Receiving a secure
#       password frame must therefore arm a delayed, duplicate-safe station
#       connect fallback instead of leaving m_sta_is_connecting true forever.
# ---------------------------------------------------------------------------
def test_fw3f_password_frame_schedules_duplicate_safe_connect_fallback():
    blufi = read("main/boards/common/blufi.cpp")
    passwd_idx = blufi.index("case ESP_BLUFI_EVENT_RECV_STA_PASSWD:")
    list_idx = blufi.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST", passwd_idx)
    passwd_body = blufi[passwd_idx:list_idx]
    fallback_body = _function_body(blufi, "void Blufi::ScheduleStationConnectFallback")

    assert "ScheduleStationConnectFallback();" in passwd_body
    assert 'StartStationConnectFromCredentials("password_fallback")' in fallback_body
    assert "m_wifi_connect_task_started" in fallback_body
    assert "vTaskDelay" in fallback_body


def test_fw3f_connect_fallback_task_deletes_itself_on_all_exits():
    blufi = read("main/boards/common/blufi.cpp")
    fallback_body = _function_body(blufi, "void Blufi::ScheduleStationConnectFallback")
    early_exit = fallback_body[
        fallback_body.index("if (!self->m_sta_is_connecting") : fallback_body.index("ESP_LOGW")
    ]

    # FreeRTOS task entry functions must not return directly on ESP-IDF; doing
    # so can abort/panic during the delayed password fallback path.
    assert "vTaskDelete(nullptr);" in early_exit
    assert early_exit.index("vTaskDelete(nullptr);") < early_exit.index("return;")
    assert fallback_body.rfind("vTaskDelete(nullptr);") > fallback_body.index(
        'StartStationConnectFromCredentials("password_fallback")'
    )


# ---------------------------------------------------------------------------
# FW3g: blufi.cpp — explicit CONNECT_TO_AP and the delayed password fallback
#       must share one guarded implementation so they cannot start duplicate
#       Wi-Fi station tasks or drift in claim/report behavior.
# ---------------------------------------------------------------------------
def test_fw3g_connect_request_and_password_fallback_share_guarded_helper():
    blufi = read("main/boards/common/blufi.cpp")
    req = _req_connect_body()
    helper = _function_body(blufi, "void Blufi::StartStationConnectFromCredentials")

    assert 'StartStationConnectFromCredentials("blufi_connect_request")' in req
    assert "BeginSsidTransaction" in helper
    assert "m_wifi_connect_task_started" in helper
    assert "blufi_wifi_conn" in helper
    assert "ReleaseBleForStationAssociation" in helper
    assert "RestoreBleAfterStationFailure" in helper


# ---------------------------------------------------------------------------
# FW4: blufi.cpp — esp_blufi_send_wifi_conn_report(... ESP_BLUFI_STA_CONN_SUCCESS
#      ...) appears BEFORE BLE deinit / stop-BLE scheduling.
# ---------------------------------------------------------------------------
def test_fw4_ble_teardown_precedes_station_association():
    helper = _station_connect_helper_body()

    release_idx = helper.index("ReleaseBleForStationAssociation")
    station_idx = helper.index("wifi.StartStation()")
    assert release_idx < station_idx
    assert "ESP_BLUFI_STA_CONN_SUCCESS" not in helper


# ---------------------------------------------------------------------------
# FW5: blufi.cpp — TryReportProvisioningAuthenticated defers (stops BLE first /
#      does not POST) while BLE is active.
# ---------------------------------------------------------------------------
def test_fw5_authenticated_report_defers_while_ble_active():
    blufi = read("main/boards/common/blufi.cpp")

    fn_idx = blufi.index("void Blufi::TryReportProvisioningAuthenticated")
    # Scope to the function body so ordering markers belong to this path.
    fn_end = blufi.index("\nvoid Blufi::", fn_idx + 1)
    body = blufi[fn_idx:fn_end]

    # While BLE is active it must NOT post: it schedules a BLE teardown and
    # re-attempt, then returns early before reaching the xTaskCreate report path.
    guard_idx = body.index("if (ble_state != BleState::kOff)")
    deinit_idx = body.index("self->CompleteSuccessfulProvisioningTeardown")
    reattempt_idx = body.index("self->TryReportProvisioningAuthenticated(", deinit_idx)
    early_return_idx = body.index("return;", guard_idx)
    report_task_idx = body.index('xTaskCreate(')

    # BLE-active guard comes first; teardown + re-attempt are inside it; the early
    # return happens before the actual report task is ever created.
    assert guard_idx < deinit_idx < reattempt_idx
    assert guard_idx < early_return_idx < report_task_idx


# ---------------------------------------------------------------------------
# FW6: blufi.cpp — a successful backend report calls ClearProvisioningSecrets.
# ---------------------------------------------------------------------------
def test_fw6_successful_report_clears_secrets():
    blufi = read("main/boards/common/blufi.cpp")

    fn_idx = blufi.index("void Blufi::TryReportProvisioningAuthenticated")
    fn_end = blufi.index("\nvoid Blufi::", fn_idx + 1)
    body = blufi[fn_idx:fn_end]

    # The success branch (if (ok)) must clear secrets.
    ok_idx = body.index("if (ok) {")
    clear_idx = body.index("self->ClearProvisioningSecrets();", ok_idx)
    assert ok_idx < clear_idx

    # ClearProvisioningSecrets actually zeroizes + clears both secrets.
    clear_def = blufi.index("void Blufi::ClearProvisioningSecrets(bool preserve_claim_token)")
    clear_body = blufi[clear_def:blufi.index("\n}", clear_def)]
    assert "bootstrap_token_.clear();" in clear_body
    assert "provisioning_code_.clear();" in clear_body


# ---------------------------------------------------------------------------
# FW7: blufi.cpp — a FAILED backend report retains secrets for retry
#      (does NOT clear them).
# ---------------------------------------------------------------------------
def test_fw7_failed_authenticated_report_retains_secrets():
    blufi = read("main/boards/common/blufi.cpp")

    fn_idx = blufi.index("void Blufi::TryReportProvisioningAuthenticated")
    fn_end = blufi.index("\nvoid Blufi::", fn_idx + 1)
    body = blufi[fn_idx:fn_end]

    # The failure branch logs retention and must NOT clear secrets.
    retain_log = "Provisioning report failed after BLE teardown; secrets retained for retry"
    assert retain_log in body

    ok_idx = body.index("if (ok) {")
    else_idx = body.index("} else {", ok_idx)
    retain_idx = body.index(retain_log)

    # The retention log lives in the else (failure) branch, after the if (ok).
    assert ok_idx < else_idx < retain_idx

    # The else/failure branch must NOT contain a ClearProvisioningSecrets call.
    else_branch = body[else_idx:body.index("}", retain_idx)]
    assert "ClearProvisioningSecrets" not in else_branch


# ---------------------------------------------------------------------------
# FW8: blufi.cpp — password log lines do not include the raw password value
#      (no %s on password).
# ---------------------------------------------------------------------------
def test_fw8_password_log_lines_never_print_raw_password():
    blufi = read("main/boards/common/blufi.cpp")

    # The RECV_STA_PASSWD log must be a bare presence string with no value.
    assert 'ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD");' in blufi
    assert 'ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD : %s"' not in blufi
    assert 'ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD: %s"' not in blufi

    # No logging line anywhere may pass the station password value to a formatter.
    for line in blufi.splitlines():
        if "ESP_LOG" in line and "password" in line.lower():
            assert "m_sta_config.sta.password" not in line
            assert "sta_passwd" not in line

    # Defense-in-depth: no "%s"-formatted log line may reference the password
    # buffer on the same line.
    for line in blufi.splitlines():
        if "ESP_LOG" in line and "%s" in line:
            assert "m_sta_config.sta.password" not in line


# ---------------------------------------------------------------------------
# FW9: provisioning_status_reporter.cc — the device_authenticated/failed report
#      path must not log the bootstrap token (even a prefix) or the raw request
#      body. The request body carries the provisioning code on a
#      device_authenticated report, so logging it leaks a credential.
# ---------------------------------------------------------------------------
def test_fw9_provisioning_report_does_not_log_token_or_request_body():
    reporter = read("main/provisioning/provisioning_status_reporter.cc")

    # No partial-token logging.
    assert "token_prefix" not in reporter
    assert "%.8s" not in reporter

    # The request body (which carries the provisioning code on a
    # device_authenticated report) and the response body (which a misbehaving
    # backend could reflect) are BOTH credential-bearing surfaces. Neither may
    # ever be formatted into a log line as a raw string. We assert on the whole
    # file because ESP_LOG calls can span multiple physical lines: the earlier
    # line-by-line scan only inspected the line carrying the ESP_LOG token, so a
    # resp_body.c_str() sitting on the *next* physical line slipped through.
    assert "body.c_str()" not in reporter
    assert "resp_body.c_str()" not in reporter

    # Defense-in-depth: also catch multi-line ESP_LOG(...) calls. Collapse each
    # ESP_LOG statement (which may wrap across lines up to its closing ");") into
    # one logical string and assert no credential buffer is referenced inside it.
    for stmt in re.findall(r"ESP_LOG\w*\((?:[^;]*?)\);", reporter, re.DOTALL):
        # The token is a credential and must never be formatted into a log line.
        assert "token.c_str()" not in stmt
        # The request body is built from `std::string body(raw)` (the POST
        # payload). Logging `body.c_str()` would leak the provisioning code.
        assert "body.c_str()" not in stmt
        # The non-2xx response body could echo the submitted request body and
        # therefore the provisioning code; redact it to a length only.
        assert "resp_body.c_str()" not in stmt


# ---------------------------------------------------------------------------
# FW10: blufi.cpp — the wifi-connect-FAIL lane (ESP_BLUFI_STA_CONN_FAIL /
#       "wifi_connect_failed") must keep the temporary provisioning secrets even
#       when the legacy FAILED-status report succeeds. The bootstrap token is
#       reserved for /claim/confirm; retryable Wi-Fi failure must not consume the
#       phone's claim attempt before an in-session retry.
# ---------------------------------------------------------------------------
def test_fw10_wifi_connect_fail_lane_never_clears_claim_secrets():
    blufi = read("main/boards/common/blufi.cpp")
    helper = _station_connect_helper_body()
    region = helper[helper.index("if (!credentials_committed)") :]

    # The fail lane must report the failed status with the wifi_connect_failed
    # reason (sanity: we are anchored on the right region).
    assert '"wifi_connect_failed"' in region

    # Reporting the retryable failure is best-effort telemetry only. Neither a
    # successful nor failed report may clear the claim token/code.
    assert "ProvisioningStatusReporter::Report" in region
    assert 'websocket_settings.GetString("claim_device_id")' in region
    assert "ClearProvisioningSecrets" not in region
    assert "secrets retained for WiFi retry" in region

    # BLE is already off for station association. Failure returns to a fresh BLE
    # setup generation automatically, without consuming the claim secrets.
    assert "RestoreBleAfterStationFailure(generation)" in region
    assert "xTaskCreate(" not in region


# ---------------------------------------------------------------------------
# FW11: blufi.cpp — GetBlufiDeviceName() must advertise a name the mobile
#       allowlist accepts. The mobile filter only admits names beginning with
#       TBot/TBOT/TBT/TJBot, and the product doc requires "TBOT-<MAC-or-serial>".
#       The eFuse-serial branch must therefore return the serial WITH a "TBOT-"
#       prefix — never a bare (unprefixed) serial — so a serial-provisioned unit
#       is still discoverable for pairing. The MAC fallback already prefixes.
# ---------------------------------------------------------------------------
def test_fw11_blufi_device_name_serial_branch_has_tbot_prefix():
    blufi = read("main/boards/common/blufi.cpp")

    fn_idx = blufi.index("static std::string GetBlufiDeviceName()")
    fn_end = blufi.index("\nstatic wifi_mode_t GetWifiModeWithFallback", fn_idx)
    body = blufi[fn_idx:fn_end]

    # Sanity: this is the serial branch (SanitizedSerial result is named `serial`).
    assert "SanitizedSerial(serial_number" in body

    # The serial branch must NOT hand back a bare, unprefixed serial.
    assert "return serial;" not in body, (
        "GetBlufiDeviceName() returns a bare serial with no TBOT- prefix; the "
        "mobile allowlist rejects it and the unit never appears for pairing"
    )

    # The serial branch must apply the documented "TBOT-" prefix.
    assert 'std::string("TBOT-") + serial' in body

    # The MAC fallback must remain prefixed (unchanged).
    assert "TBOT-%02X%02X%02X%02X%02X%02X" in body


# ---------------------------------------------------------------------------
# FW12: blufi.cpp — init() must NOT mark itself initialized before the BLE
#       controller/host actually come up. inited_=true must appear only AFTER
#       _host_and_cb_init() succeeds (on the success path), and every early
#       failure-return must set inited_=false so GetBleState() reports kOff and
#       wifi_board.cc re-runs init() on the next config-mode entry.
# ---------------------------------------------------------------------------
def test_fw12_init_marks_inited_only_on_success_path():
    blufi = read("main/boards/common/blufi.cpp")

    fn_idx = blufi.index("esp_err_t Blufi::init()")
    fn_end = blufi.index("esp_err_t Blufi::deinit()", fn_idx)
    body = blufi[fn_idx:fn_end]

    host_init_idx = body.index("_host_and_cb_init()")
    success_inited_idx = body.index("inited_ = true;")
    success_return_idx = body.index("return ESP_OK;")

    # inited_=true must come AFTER host init is attempted and BEFORE the final
    # ESP_OK return — i.e. on the success path only.
    assert host_init_idx < success_inited_idx < success_return_idx, (
        "inited_=true must be set only on the success path, after "
        "_host_and_cb_init(), not at the top of init()"
    )

    # There must be exactly one inited_=true (no top-of-function premature set).
    assert body.count("inited_ = true;") == 1

    # The controller-init failure-return must reset inited_ first.
    controller_fail = body.index("BLUFI controller init failed")
    controller_return = body.index("return ret;", controller_fail)
    assert "inited_ = false;" in body[:controller_return], (
        "controller-init failure path must set inited_=false"
    )
    last_inited_false_before_ctrl = body.rindex("inited_ = false;", 0, controller_return)
    assert last_inited_false_before_ctrl < controller_return

    # The host-init failure-return must reset inited_ first.
    host_fail = body.index("BLUFI host and cb init failed")
    host_return = body.index("return ret;", host_fail)
    last_inited_false_before_host = body.rindex("inited_ = false;", 0, host_return)
    assert last_inited_false_before_host < host_return, (
        "host-init failure path must set inited_=false"
    )


def test_fw12b_init_cleans_controller_after_partial_init_failures():
    blufi = read("main/boards/common/blufi.cpp")

    fn_idx = blufi.index("esp_err_t Blufi::init()")
    fn_end = blufi.index("esp_err_t Blufi::deinit()", fn_idx)
    body = blufi[fn_idx:fn_end]

    # If controller init partially succeeds and later fails, or if host/Bluedroid
    # init fails after controller enable, leaving the controller allocated makes
    # the next standby retry fail with ESP_ERR_INVALID_STATE. Every init failure
    # path that can follow _controller_init() must deinit the controller before
    # reporting BLE off to callers.
    controller_fail = body.index("BLUFI controller init failed")
    controller_return = body.index("return ret;", controller_fail)
    controller_branch = body[controller_fail:controller_return]
    assert "_controller_deinit();" in controller_branch

    host_fail = body.index("BLUFI host and cb init failed")
    host_return = body.index("return ret;", host_fail)
    host_branch = body[host_fail:host_return]
    assert "_controller_deinit();" in host_branch

# ---------------------------------------------------------------------------
# FW13: blufi.cpp — the RECV_STA_SSID / RECV_STA_PASSWD copy must be
#       length-bounded. A raw `buf[param->..._len] = '\0'` overflows when len
#       equals the buffer size; the copy length must be clamped to
#       sizeof(buffer)-1 before the NUL terminator is written.
# ---------------------------------------------------------------------------
def test_fw13_recv_sta_ssid_passwd_copy_is_length_bounded():
    blufi = read("main/boards/common/blufi.cpp")

    ssid_idx = blufi.index("case ESP_BLUFI_EVENT_RECV_STA_SSID:")
    passwd_idx = blufi.index("case ESP_BLUFI_EVENT_RECV_STA_PASSWD:")
    end_idx = blufi.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST", passwd_idx)

    ssid_body = blufi[ssid_idx:passwd_idx]
    passwd_body = blufi[passwd_idx:end_idx]

    # Neither handler may write a NUL at an unbounded length index.
    assert "m_sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\\0';" not in ssid_body, (
        "SSID copy writes NUL at an unbounded index — overflows when len == sizeof"
    )
    assert "m_sta_config.sta.password[param->sta_passwd.passwd_len] = '\\0';" not in passwd_body, (
        "password copy writes NUL at an unbounded index — overflows when len == sizeof"
    )

    # Both copies must clamp the length against the buffer size.
    for label, segment, buf in (
        ("ssid", ssid_body, "m_sta_config.sta.ssid"),
        ("passwd", passwd_body, "m_sta_config.sta.password"),
    ):
        assert "std::min" in segment, f"{label} copy is not bounded with std::min"
        assert f"sizeof({buf})" in segment, (
            f"{label} copy does not clamp against sizeof({buf})"
        )

    # Defense-in-depth: the password handler still must not log the value.
    assert 'ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD");' in passwd_body
    assert "%s" not in passwd_body


# ---------------------------------------------------------------------------
# FW14: blufi.cpp — the BLE re-advertise cap must count CONSECUTIVE failed
#       auto-readvertises, NOT successful sessions. A user whose phone
#       disconnects + reconnects a few times in one setup window must NOT
#       exhaust the cap and be stranded with BLE down until the next explicit
#       BOOT setup entry. The fix: a SUCCESSFUL client connect proves
#       re-advertising works, so the ESP_BLUFI_EVENT_BLE_CONNECT handler must
#       reset ble_readvertise_count_ to 0. The cap itself must still exist so a
#       genuinely flapping peer (N consecutive readvertises with no successful
#       connect) is still bounded.
# ---------------------------------------------------------------------------
def test_fw14_ble_connect_resets_readvertise_cap_for_legitimate_reconnects():
    blufi = read("main/boards/common/blufi.cpp")

    # The cap must still exist (the bound is not removed).
    assert "kMaxBleReadvertiseAttempts" in blufi
    assert "ble_readvertise_count_ >= kMaxBleReadvertiseAttempts" in blufi, (
        "the consecutive-readvertise cap comparison must remain in place"
    )

    # Scope to the BLE_CONNECT handler body so the reset belongs to the
    # successful-connect path, not the disconnect/init paths.
    connect_idx = blufi.index("case ESP_BLUFI_EVENT_BLE_CONNECT:")
    connect_end = blufi.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT:", connect_idx)
    connect_body = blufi[connect_idx:connect_end]

    # A successful client connect clears the re-advertise counter: this is what
    # makes the cap count CONSECUTIVE failed readvertises rather than legitimate
    # reconnect sessions. Without this, N reconnects in one window trip the cap.
    assert "ble_readvertise_count_ = 0;" in connect_body, (
        "ESP_BLUFI_EVENT_BLE_CONNECT must reset ble_readvertise_count_ to 0 so a "
        "successful connect clears the re-advertise cap; otherwise legitimate "
        "reconnects in one setup window exhaust the cap and leave BLE down"
    )

    # Sanity: the increment must still live in the DISCONNECT path (we did not
    # move the counting logic out of the auto-readvertise lane).
    disconnect_idx = blufi.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT:")
    disconnect_end = blufi.index("case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:", disconnect_idx)
    disconnect_body = blufi[disconnect_idx:disconnect_end]
    assert "++ble_readvertise_count_;" in disconnect_body, (
        "the readvertise increment must remain in the BLE_DISCONNECT handler"
    )


# ===========================================================================
# Extended coverage (FW15..FW28). Same source-level static-assertion
# convention: each test is anchored on a real marker in blufi.cpp and asserts
# REAL behavior (ordering / presence / scoping), never a tautology.
# ===========================================================================


def _handle_event_body() -> str:
    """The body of Blufi::_handle_event(...) (the big event switch)."""
    blufi = read("main/boards/common/blufi.cpp")
    start = blufi.index("void Blufi::_handle_event(")
    end = blufi.index("\n// ---", start)
    return blufi[start:end]


def _try_report_body() -> str:
    """The body of Blufi::TryReportProvisioningAuthenticated(...)."""
    blufi = read("main/boards/common/blufi.cpp")
    fn_idx = blufi.index("void Blufi::TryReportProvisioningAuthenticated")
    fn_end = blufi.index("\nvoid Blufi::", fn_idx + 1)
    # Guard: ensure we actually captured a non-trivial body containing the guard.
    body = blufi[fn_idx:fn_end]
    assert "ble_state" in body
    return body


def _custom_data_body() -> str:
    """The body of the ESP_BLUFI_EVENT_RECV_CUSTOM_DATA case (TLV parser)."""
    blufi = read("main/boards/common/blufi.cpp")
    start = blufi.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")
    end = blufi.index("default:", start)
    return blufi[start:end]


def _req_connect_body() -> str:
    """The body of the ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP case (Wi-Fi connect task)."""
    blufi = read("main/boards/common/blufi.cpp")
    start = blufi.index("case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:")
    end = blufi.index("case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:", start)
    return blufi[start:end]

def _station_connect_helper_body() -> str:
    """The shared BluFi station-connect helper used by CONNECT_TO_AP and fallback."""
    blufi = read("main/boards/common/blufi.cpp")
    return _function_body(blufi, "void Blufi::StartStationConnectFromCredentials")


def test_wifi_credentials_release_ble_before_station_association():
    helper = _station_connect_helper_body()

    release = helper.index("ReleaseBleForStationAssociation")
    station = helper.index("wifi.StartStation()")
    assert release < station


def test_failed_station_association_rolls_back_then_restores_ble_without_boot():
    helper = _station_connect_helper_body()
    failure = helper[helper.index("if (!credentials_committed)") :]

    rollback = failure.index("RollbackSsidTransaction(ssid_transaction)")
    restore = failure.index("RestoreBleAfterStationFailure")
    assert rollback < restore


def test_ble_restore_is_scoped_to_the_originating_setup_generation():
    blufi = read("main/boards/common/blufi.cpp")
    restore = _function_body(blufi, "void Blufi::RestoreBleAfterStationFailure")

    assert "expected_generation != setup_generation_.load()" in restore
    assert "init()" in restore
    assert "StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC)" in restore


# ---------------------------------------------------------------------------
# FW15: blufi.cpp — TryReportProvisioningAuthenticated is LEVEL-TRIGGERED. It is
#       invoked on EVERY input edge that can complete the precondition set
#       (bootstrap token arrival, provisioning code arrival, and Wi-Fi success
#       after BLE teardown) so that whichever input arrives LAST drives the
#       report. Out-of-order token/code/wifi therefore self-heals: an early call
#       with a missing piece returns at the precondition guard, and the next
#       arriving piece re-attempts.
# ---------------------------------------------------------------------------
def test_fw15_authenticated_report_is_level_triggered_on_all_inputs():
    blufi = read("main/boards/common/blufi.cpp")

    # Every triggering edge calls TryReportProvisioningAuthenticated:
    #   - token TLV (tag 0x01)
    #   - code TLV  (tag 0x02)
    #   - Wi-Fi connect success (after BLE teardown)
    custom = _custom_data_body()
    assert 'TryReportProvisioningAuthenticated(' in custom
    assert '"custom_data_token", generation' in custom, (
        "token TLV must re-attempt the authenticated report"
    )
    assert '"custom_data_code", generation' in custom, (
        "code TLV must re-attempt the authenticated report"
    )
    req_connect = _station_connect_helper_body()
    assert (
        '"wifi_success_after_ble_teardown", generation' in req_connect
    ), "Wi-Fi success path must drive the authenticated report"

    # The precondition guard returns early when ANY piece is missing — this is
    # exactly what makes an out-of-order arrival harmless (it retries later).
    body = _try_report_body()
    guard_idx = body.index(
        "if (!m_provisioned || !wifi_connected || token_empty || code_empty || report_in_flight)"
    )
    early_return = body.index("return;", guard_idx)
    # The BLE-active defer block + the report task must come AFTER the guard.
    ble_guard = body.index("if (ble_state != BleState::kOff)")
    assert guard_idx < early_return < ble_guard, (
        "the missing-precondition guard must short-circuit BEFORE the BLE-active "
        "defer and the report task, so an out-of-order input self-heals"
    )


# ---------------------------------------------------------------------------
# FW16: blufi.cpp — the precondition guard requires a completed credential
#       transaction, wifi_connected, token present, code present, and no report
#       already in flight. A previously connected network is not provisioning.
# ---------------------------------------------------------------------------
def test_fw16_authenticated_report_requires_wifi_token_code_and_not_in_flight():
    body = _try_report_body()

    # The four precondition locals are derived from real state.
    assert "const bool wifi_connected = WifiManager::GetInstance().IsConnected();" in body
    assert "const bool token_empty = bootstrap_token_.empty();" in body
    assert "const bool code_empty = provisioning_code_.empty();" in body

    # The single guard combines all five with OR of the negatives.
    assert (
        "if (!m_provisioned || !wifi_connected || token_empty || code_empty || report_in_flight)"
        in body
    ), "the report must be gated on new provisioning+wifi+token+code+not-in-flight"

    # The skip log must NOT format any secret value — only booleans/reason.
    skip_log_start = body.index("Reporting provisioning authenticated skipped")
    skip_log_end = body.index(");", skip_log_start)
    skip_log = body[skip_log_start:skip_log_end]
    assert "bootstrap_token_" not in skip_log
    assert "provisioning_code_" not in skip_log
    assert ".c_str()" not in skip_log


# ---------------------------------------------------------------------------
# FW17: blufi.cpp — the in-flight flag is the concurrency guard for the report
#       POST. It must be SET to true exactly before the report task is created
#       and reset to false on the completion Schedule() (both branches) AND on
#       the task-create-failure path, so a failed xTaskCreate cannot strand the
#       flag set and permanently block all future retries.
# ---------------------------------------------------------------------------
def test_fw17_generation_owner_set_before_task_and_cleared_on_all_exits():
    body = _try_report_body()

    set_idx = body.index("provisioning_report_owner_generation_ = expected_generation;")
    task_idx = body.index("xTaskCreate(", set_idx)
    assert set_idx < task_idx, "the generation owner must be set before the report task is created"

    # Completion handler verifies and resets the matching owner for both outcomes.
    owner_match = body.index("provisioning_report_owner_generation_.value() != expected_generation")
    completion_reset = body.index("provisioning_report_owner_generation_.reset();", owner_match)
    ok_branch = body.index("if (ok) {", completion_reset)
    assert owner_match < completion_reset < ok_branch

    # The task-create FAILURE path clears only the still-matching owner.
    fail_idx = body.index("if (created != pdPASS)")
    fail_reset = body.index("provisioning_report_owner_generation_.reset();", fail_idx)
    fail_return = body.index("return;", fail_idx)
    assert fail_idx < fail_reset < fail_return


# ---------------------------------------------------------------------------
# FW18: blufi.cpp — the report task copies the token+code into a heap context
#       and ZEROIZES that copy after the POST, on BOTH the normal completion
#       path and the task-create-failure cleanup path. The temporary copy must
#       never outlive the POST in cleartext.
# ---------------------------------------------------------------------------
def test_fw18_report_task_zeroizes_its_secret_copy_on_every_path():
    body = _try_report_body()

    # The heap context carries copies of the secrets (not references).
    assert (
        "this, bootstrap_token_, provisioning_code_, expected_generation" in body
    ), "the report task must take copies of the secrets, not live references"

    # The task body zeroizes both copies after the POST completes.
    task_idx = body.index("xTaskCreate(")
    post_idx = body.index("ProvisioningStatusReporter::Report(", task_idx)
    fill_token = body.index("std::fill(ctx->token.begin(), ctx->token.end(), '\\0');", post_idx)
    fill_code = body.index("std::fill(ctx->code.begin(), ctx->code.end(), '\\0');", post_idx)
    delete_ctx = body.index("delete ctx;", fill_code)
    assert post_idx < fill_token < delete_ctx, "token copy must be zeroized before delete"
    assert post_idx < fill_code < delete_ctx, "code copy must be zeroized before delete"

    # The task-create-failure cleanup also zeroizes + deletes the ctx (no leak).
    fail_idx = body.index("if (created != pdPASS)")
    assert "std::fill(ctx->token.begin(), ctx->token.end(), '\\0');" in body[fail_idx:]
    assert "std::fill(ctx->code.begin(), ctx->code.end(), '\\0');" in body[fail_idx:]
    assert "delete ctx;" in body[fail_idx:]


# ---------------------------------------------------------------------------
# FW19: blufi.cpp — device_authenticated must be reported only AFTER BLE is off.
#       The BLE-active branch schedules a teardown then RE-CALLS the report; only
#       a kOff state reaches the actual ProvisioningStatusReporter::Report call.
#       Cross-check FW5 with the concrete Status::DeviceAuthenticated marker.
# ---------------------------------------------------------------------------
def test_fw19_device_authenticated_reported_only_after_ble_off():
    body = _try_report_body()

    ble_guard = body.index("if (ble_state != BleState::kOff)")
    # Inside the BLE-active branch: cancel timeout, deinit, re-attempt, early return.
    deinit_idx = body.index("self->CompleteSuccessfulProvisioningTeardown", ble_guard)
    reattempt_idx = body.index("self->TryReportProvisioningAuthenticated(", deinit_idx)
    defer_return = body.index("return;", body.index("ble_state=%s", ble_guard))

    # The actual authenticated POST must come AFTER the BLE-active early return.
    report_call = body.index(
        "ProvisioningStatusReporter::Status::DeviceAuthenticated", defer_return
    )
    assert ble_guard < deinit_idx < reattempt_idx
    assert defer_return < report_call, (
        "the DeviceAuthenticated POST must only be reachable after the BLE-active "
        "branch has returned (i.e. BLE is off)"
    )


# ---------------------------------------------------------------------------
# FW20: blufi.cpp — the BLE-active defer must guard against a duplicate teardown
#       schedule while a Wi-Fi connect is in progress (m_sta_is_connecting). The
#       re-attempt is only scheduled when NOT mid-connect; otherwise the wifi
#       connect task itself will drive the report after it tears BLE down.
# ---------------------------------------------------------------------------
def test_fw20_ble_active_defer_skips_reschedule_during_wifi_connect():
    body = _try_report_body()

    ble_guard = body.index("if (ble_state != BleState::kOff)")
    # The reschedule is gated on !m_sta_is_connecting.
    not_connecting = body.index("if (!m_sta_is_connecting.load())", ble_guard)
    schedule_idx = body.index("Application::GetInstance().Schedule(", not_connecting)
    assert ble_guard < not_connecting < schedule_idx, (
        "the BLE teardown reschedule must be gated on !m_sta_is_connecting so a "
        "mid-connect report does not double-schedule a teardown"
    )

    # Even when mid-connect, the function still logs+returns (deferred), never
    # reaching the POST.
    defer_log = body.index("Reporting provisioning authenticated deferred until BLE is off")
    assert defer_log > ble_guard


# ---------------------------------------------------------------------------
# FW21: BLE must be fully released before station association; success then
#       completes the provisioning owner and starts claim work from Application.
# ---------------------------------------------------------------------------
def test_fw21_ble_release_precedes_station_and_claim_continuation():
    req = _station_connect_helper_body()
    release_idx = req.index("ReleaseBleForStationAssociation")
    station_idx = req.index("wifi.StartStation()")
    success_idx = req.index("if (credentials_committed)")
    sched_idx = req.index("Application::GetInstance().Schedule(", success_idx)
    sched_body = req[sched_idx:]

    assert release_idx < station_idx < success_idx < sched_idx
    assert "ESP_BLUFI_STA_CONN_SUCCESS" not in req
    assert "self->CompleteSuccessfulProvisioningTeardown" in sched_body
    assert (
        '"wifi_success_after_ble_teardown", generation' in sched_body
    )
    assert "SchedulePendingTbotClaimRefresh(generation);" in sched_body
    # Teardown precedes the authenticated report inside the deferred lambda.
    assert sched_body.index("self->CompleteSuccessfulProvisioningTeardown") < sched_body.index(
        "TryReportProvisioningAuthenticated"
    )


# ---------------------------------------------------------------------------
# FW21b: esp_blufi_send_wifi_conn_report() only queues a BTC notification; it
#        does not provide a delivery-complete callback or indication ACK. Keep
#        the live GATT link up for one short, explicit grace period after a
#        successful enqueue before disconnect/deinit. An enqueue error must be
#        handled separately and must never be described as delivered.
# ---------------------------------------------------------------------------
def test_fw21b_station_association_has_no_ble_delivery_grace_or_report():
    req = _station_connect_helper_body()
    release_idx = req.index("ReleaseBleForStationAssociation")
    station_idx = req.index("wifi.StartStation()")
    assert release_idx < station_idx
    assert "kBlufiSuccessReportDeliveryGraceMs" not in req
    assert "esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS" not in req


# ---------------------------------------------------------------------------
# FW21c: m_sta_is_connecting is also the concurrency barrier used by
#        TryReportProvisioningAuthenticated() while BLE is live. Keep it set
#        through the delivery grace, then fence on setup generation before the
#        old worker may clear the barrier or disconnect a potentially newer
#        GATT session.
# ---------------------------------------------------------------------------
def test_fw21c_ble_release_and_station_worker_are_generation_fenced():
    blufi = read("main/boards/common/blufi.cpp")
    req = _station_connect_helper_body()
    release_idx = req.index("ReleaseBleForStationAssociation")
    generation_check_idx = req.index(
        "if (generation != self->setup_generation_.load())", release_idx
    )
    station_idx = req.index("wifi.StartStation()", generation_check_idx)
    assert release_idx < generation_check_idx < station_idx

    header = read("main/boards/common/blufi.h")
    assert "std::atomic<bool> m_sta_is_connecting{false};" in header
    assert not re.search(r"m_sta_is_connecting\s*=\s*(?:true|false)", blufi)
    assert "m_sta_is_connecting.store(true);" in blufi
    assert "m_sta_is_connecting.store(false);" in blufi
    assert "m_sta_is_connecting.load()" in blufi
    for line in blufi.splitlines():
        if "m_sta_is_connecting" not in line:
            continue
        if "m_sta_is_connecting(false)" in line:
            continue
        assert ".load()" in line or ".store(" in line, line


# ---------------------------------------------------------------------------
# FW21d: completion must re-check setup ownership immediately before every
#        transaction resolve operation, then fence again before mutating shared
#        station state or reporting. Transaction cleanup uses the opaque id and
#        CAS so an old worker cannot clear a newer candidate's ownership.
# ---------------------------------------------------------------------------
def test_fw21d_transaction_resolution_and_shared_mutation_are_generation_fenced():
    req = _station_connect_helper_body()
    worker_start = req.index("while (waited_ms < kConnectTimeoutMs")
    worker = req[worker_start:]

    commit_marker = worker.index(
        "Revalidate setup ownership immediately before transaction commit"
    )
    pre_commit_fence = worker.index(
        "if (generation != self->setup_generation_.load())", commit_marker
    )
    commit_idx = worker.index("CommitSsidTransaction(ssid_transaction)", pre_commit_fence)
    assert commit_marker < pre_commit_fence < commit_idx
    assert "vTaskDelete(nullptr);" in worker[pre_commit_fence:commit_idx]
    assert "return;" in worker[pre_commit_fence:commit_idx]

    rollback_marker = worker.index(
        "Revalidate setup ownership immediately before transaction rollback",
        commit_idx,
    )
    pre_rollback_fence = worker.index(
        "if (generation != self->setup_generation_.load())", rollback_marker
    )
    rollback_after_commit = worker.index(
        "RollbackSsidTransaction(ssid_transaction)", pre_rollback_fence
    )
    assert commit_idx < rollback_marker < pre_rollback_fence < rollback_after_commit
    first_shared_mutation = min(
        worker.index("self->m_sta_connected = true;"),
        worker.index("self->m_wifi_connect_task_started.store(false);"),
    )
    shared_marker = worker.index(
        "Fence resolved transaction before shared station or report mutation",
        rollback_after_commit,
    )
    post_resolve_fence = worker.index(
        "if (generation != self->setup_generation_.load())", shared_marker
    )
    rollback_region = worker[pre_rollback_fence:shared_marker]
    assert rollback_region.count("RollbackSsidTransaction(ssid_transaction)") >= 2
    assert "ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0)" in rollback_region
    assert rollback_after_commit < shared_marker < post_resolve_fence < first_shared_mutation
    assert "vTaskDelete(nullptr);" in worker[post_resolve_fence:first_shared_mutation]
    assert "return;" in worker[post_resolve_fence:first_shared_mutation]

    ownership = worker[pre_commit_fence:first_shared_mutation]
    assert ownership.count("uint32_t expected_transaction = ssid_transaction;") >= 2
    assert ownership.count(
        "ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0)"
    ) >= 2


# ---------------------------------------------------------------------------
# FW21e: generation checks alone are TOCTOU. RestartForSetup and the completion
#        worker must serialize generation/transaction/state/report ownership on
#        one Blufi mutex. The worker keeps it through SUCCESS grace/disconnect
#        or FAIL enqueue, then releases it before scheduling claim/TLS work.
# ---------------------------------------------------------------------------
def test_fw21e_restart_and_completion_share_finalization_mutex_without_lock_leaks():
    header = read("main/boards/common/blufi.h")
    blufi = read("main/boards/common/blufi.cpp")
    restart = _function_body(blufi, "esp_err_t Blufi::RestartForSetup")
    req = _station_connect_helper_body()
    worker_start = req.index("while (waited_ms < kConnectTimeoutMs")
    worker = req[worker_start:]

    assert "#include <mutex>" in header
    assert "std::mutex provisioning_finalization_mutex_;" in header

    restart_lock = restart.index(
        "std::lock_guard<std::mutex> finalization_lock(provisioning_finalization_mutex_);"
    )
    generation_advance = restart.index("setup_generation_.fetch_add(1)")
    transaction_take = restart.index("ssid_transaction_id_.exchange(0)")
    rollback = restart.index("RollbackSsidTransaction(stale_ssid_transaction)")
    state_reset = restart.index("m_sta_is_connecting.store(false)")
    assert restart_lock < generation_advance < transaction_take < rollback < state_reset

    worker_lock = worker.index("std::unique_lock<std::mutex> finalization_lock(")
    assert "self->provisioning_finalization_mutex_);" in worker[
        worker_lock:worker_lock + 160
    ]
    first_generation_check = worker.index(
        "if (generation != self->setup_generation_.load())", worker_lock
    )
    first_transaction_resolve = min(
        worker.index("CommitSsidTransaction(ssid_transaction)", worker_lock),
        worker.index("RollbackSsidTransaction(ssid_transaction)", worker_lock),
    )
    assert worker_lock < first_generation_check < first_transaction_resolve

    success_start = worker.index("if (credentials_committed)", worker_lock)
    failure_start = worker.index("} else {", success_start)
    success = worker[success_start:failure_start]
    failure = worker[failure_start:]

    success_unlock = success.index("finalization_lock.unlock();")
    claim_schedule = success.index("Application::GetInstance().Schedule(", success_unlock)
    assert success_unlock < claim_schedule

    failure_unlock = failure.index("finalization_lock.unlock();")
    failure_restore = failure.index("RestoreBleAfterStationFailure", failure_unlock)
    assert failure_unlock < failure_restore

    # Every self-delete reachable while the mutex is owned explicitly unlocks
    # first; FreeRTOS self-delete does not guarantee C++ stack unwinding.
    locked_region = worker[worker_lock:success_unlock]
    for delete_match in re.finditer(r"vTaskDelete\(nullptr\);", locked_region):
        prefix = locked_region[max(0, delete_match.start() - 120):delete_match.start()]
        assert "finalization_lock.unlock();" in prefix


# ---------------------------------------------------------------------------
# FW21f: the deferred success continuation re-enters after the worker releases
#        the mutex, so it must reacquire the same lock before generation check
#        and BLE teardown. Failure reporting must snapshot secrets by value
#        under the worker lock and clear them after unlocked HTTP use.
# ---------------------------------------------------------------------------
def test_fw21f_deferred_teardown_and_failure_secret_snapshot_are_synchronized():
    req = _station_connect_helper_body()
    success_start = req.index("if (credentials_committed)")
    failure_start = req.index("} else {", success_start)
    success = req[success_start:failure_start]
    failure = req[failure_start:]

    schedule_start = success.index("Application::GetInstance().Schedule(")
    continuation = success[schedule_start:]
    lock_idx = continuation.index(
        "std::unique_lock<std::mutex> continuation_lock("
    )
    assert "self->provisioning_finalization_mutex_);" in continuation[
        lock_idx:lock_idx + 170
    ]
    generation_idx = continuation.index(
        "if (generation != self->setup_generation_.load())", lock_idx
    )
    stale_unlock_idx = continuation.index("continuation_lock.unlock();", generation_idx)
    stale_return_idx = continuation.index("return;", stale_unlock_idx)
    teardown_idx = continuation.index(
        "self->CompleteSuccessfulProvisioningTeardown(", stale_return_idx
    )
    assert '"wifi_credentials_connected", provisioning_token' in continuation[
        teardown_idx:teardown_idx + 180
    ]
    success_unlock_idx = continuation.index("continuation_lock.unlock();", teardown_idx)
    report_idx = continuation.index("self->TryReportProvisioningAuthenticated", success_unlock_idx)
    claim_idx = continuation.index("SchedulePendingTbotClaimRefresh", report_idx)
    assert lock_idx < generation_idx < stale_unlock_idx < stale_return_idx
    assert stale_return_idx < teardown_idx < success_unlock_idx < report_idx < claim_idx

    token_snapshot = failure.index(
        "std::string failure_token = self->bootstrap_token_;"
    )
    code_snapshot = failure.index(
        "std::string failure_code = self->provisioning_code_;", token_snapshot
    )
    unlock_idx = failure.index("finalization_lock.unlock();", code_snapshot)
    legacy_report = failure.index("ProvisioningStatusReporter::Report(", unlock_idx)
    token_clear = failure.index("SecureClearLocalString(failure_token);", legacy_report)
    code_clear = failure.index("SecureClearLocalString(failure_code);", token_clear)
    assert token_snapshot < code_snapshot < unlock_idx < legacy_report < token_clear < code_clear

    unlocked_failure = failure[unlock_idx:code_clear]
    assert "self->bootstrap_token_" not in unlocked_failure
    assert "self->provisioning_code_" not in unlocked_failure
    assert "const std::string& token = failure_token;" in unlocked_failure
    assert "const std::string& code = failure_code;" in unlocked_failure


# ---------------------------------------------------------------------------
# FW21g: claim promotion/refresh dispatch carries the setup generation into the
#        Application task and runs under the same finalization mutex. The lock
#        covers generation validation + state promotion + refresh dispatch only;
#        the dispatched RefreshPendingTbotClaim executes after the helper returns
#        so network/TLS never runs while the Blufi mutex is held.
# ---------------------------------------------------------------------------
def test_fw21g_claim_refresh_dispatch_is_generation_bound_without_tls_under_lock():
    blufi_header = read("main/boards/common/blufi.h")
    blufi = read("main/boards/common/blufi.cpp")
    app_header = read("main/application.h")
    app = read("main/application.cc")
    helper = _function_body(blufi, "bool Blufi::RunIfSetupGenerationCurrent")
    schedule = _function_body(app, "void Application::SchedulePendingTbotClaimRefresh")

    assert "#include <functional>" in blufi_header
    assert "bool RunIfSetupGenerationCurrent(uint32_t expected_generation," in blufi_header
    assert "const std::function<void()>& action);" in blufi_header
    assert "std::lock_guard<std::mutex> finalization_lock(" in helper
    assert "provisioning_finalization_mutex_" in helper
    generation_idx = helper.index("expected_generation != setup_generation_.load()")
    action_idx = helper.index("action();", generation_idx)
    assert generation_idx < action_idx

    assert "void SchedulePendingTbotClaimRefresh(uint32_t expected_setup_generation);" in app_header
    assert "expected_setup_generation" in schedule
    run_idx = schedule.index("RunIfSetupGenerationCurrent(")
    assert "expected_setup_generation" in schedule[run_idx:run_idx + 180]
    promote_idx = schedule.index("PromoteFromWifiConfigAfterProvisioning();", run_idx)
    dispatch_idx = schedule.index(
        "DispatchPendingTbotClaimRefreshForSetupGeneration(", promote_idx
    )
    assert "expected_setup_generation" in schedule[dispatch_idx:dispatch_idx + 150]
    assert run_idx < promote_idx < dispatch_idx
    guarded_dispatch = schedule[run_idx:dispatch_idx]
    assert "FetchBackendApiUrlFromBootstrap" not in guarded_dispatch
    assert "ConfirmPendingTbotClaim" not in guarded_dispatch
    assert "RefreshPendingTbotClaim" not in schedule
    assert "Schedule([this]()" not in schedule[promote_idx:dispatch_idx]

    success_start = blufi.index("if (credentials_committed)")
    success_end = blufi.index("Failed to connect to WiFi via esp-wifi-connect", success_start)
    success = blufi[success_start:success_end]
    assert "SchedulePendingTbotClaimRefresh(generation);" in success


# ---------------------------------------------------------------------------
# FW21h: generation-bound refresh is prepare/dispatch-only. API fallback,
#        device-config fetch, and cached confirmation all carry the setup
#        generation through worker context; their Application-task result is
#        discarded unless RunIfSetupGenerationCurrent accepts it.
# ---------------------------------------------------------------------------
def test_fw21h_generation_flows_through_claim_network_contexts_and_result_apply():
    app = read("main/application.cc")
    header = read("main/application.h")
    prepare = _function_body(
        app, "void Application::DispatchPendingTbotClaimRefreshForSetupGeneration"
    )
    fetch_dispatch = _function_body(app, "bool Application::DispatchPendingTbotClaimFetch")
    fetch_task = _function_body(app, "void Application::ClaimFetchTask")
    confirm_dispatch = _function_body(
        app, "bool Application::DispatchPendingTbotClaimConfirmation"
    )
    confirm_task = _function_body(app, "void Application::ClaimConfirmationTask")

    assert "DispatchPendingTbotClaimRefreshForSetupGeneration" in header
    assert "DispatchPendingTbotClaimFetch" in prepare
    assert "DispatchPendingTbotClaimConfirmation" in prepare
    for blocking in (
        "FetchBackendApiUrlFromBootstrap",
        "FetchPendingTbotClaimFromDeviceConfig",
        "ClaimConfirmationReporter::Confirm",
        "ConfirmPendingTbotClaim",
    ):
        assert blocking not in prepare

    assert "uint32_t expected_setup_generation;" in app
    assert "bool enforce_setup_generation;" in app
    assert "expected_setup_generation" in fetch_dispatch
    assert "enforce_setup_generation" in fetch_dispatch
    assert "ctx->expected_setup_generation" in fetch_task
    assert "FetchBackendApiUrlFromBootstrap(token, false)" in fetch_task
    fetch_apply = fetch_task.index("ApplyPendingTbotClaimFetchResult")
    fetch_gate = fetch_task.index("RunIfSetupGenerationCurrent(", fetch_apply)
    assert "expected_setup_generation" in fetch_task[fetch_gate:fetch_gate + 180]
    assert "apply_result" in fetch_task[fetch_gate:fetch_gate + 220]
    assert fetch_apply < fetch_gate

    assert "expected_setup_generation" in confirm_dispatch
    assert "enforce_setup_generation" in confirm_dispatch
    assert "ctx->expected_setup_generation" in confirm_task
    assert "ClaimConfirmationReporter::Confirm" in confirm_task
    assert "success_response" in confirm_task
    confirm_apply = confirm_task.index("ApplyPendingTbotClaimConfirmationResult")
    confirm_gate = confirm_task.index("RunIfSetupGenerationCurrent(", confirm_apply)
    assert "expected_setup_generation" in confirm_task[confirm_gate:confirm_gate + 180]
    assert "apply_result" in confirm_task[confirm_gate:confirm_gate + 220]
    assert confirm_apply < confirm_gate
    persist_idx = confirm_task.index("PersistTbotClaimConfirmationResponse")
    assert persist_idx < confirm_gate
    assert "SecureClearString(success_response);" in confirm_task

    assert "claim_confirm_inflight_" in header


# ---------------------------------------------------------------------------
# FW21i: the legacy authenticated-status worker is generation-owned too. Start,
#        secret snapshot, and result application serialize on the finalization
#        mutex; only a current generation whose optional owner still matches may
#        clear in-flight ownership or provisioning secrets.
# ---------------------------------------------------------------------------
def test_fw21i_legacy_report_is_generation_owned_and_cannot_clear_new_setup():
    header = read("main/boards/common/blufi.h")
    blufi = read("main/boards/common/blufi.cpp")
    restart = _function_body(blufi, "esp_err_t Blufi::RestartForSetup")
    report = _function_body(blufi, "void Blufi::TryReportProvisioningAuthenticated")
    custom = _function_body(blufi, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")
    connect = _station_connect_helper_body()

    assert "#include <optional>" in header
    assert "std::optional<uint32_t> provisioning_report_owner_generation_;" in header
    assert "bool provisioning_report_in_flight_" not in header
    assert (
        "void TryReportProvisioningAuthenticated(const char* reason, "
        "uint32_t expected_generation);"
        in header
    )

    restart_lock = restart.index("finalization_lock")
    owner_reset = restart.index("provisioning_report_owner_generation_.reset()")
    assert restart_lock < owner_reset

    report_lock = report.index("std::unique_lock<std::mutex> report_lock(")
    generation_check = report.index(
        "expected_generation != setup_generation_.load()", report_lock
    )
    owner_check = report.index("provisioning_report_owner_generation_.has_value()")
    owner_assign = report.index(
        "provisioning_report_owner_generation_ = expected_generation", owner_check
    )
    context_generation = report.index("uint32_t expected_generation;")
    assert report_lock < generation_check < owner_check < context_generation < owner_assign

    result_gate = report.index("RunIfSetupGenerationCurrent(")
    assert "expected_generation" in report[result_gate:result_gate + 180]
    matching_owner = report.index(
        "provisioning_report_owner_generation_.value() != expected_generation",
        result_gate,
    )
    result_reset = report.index("provisioning_report_owner_generation_.reset()", matching_owner)
    clear_secrets = report.index("self->ClearProvisioningSecrets();", result_reset)
    assert result_gate < matching_owner < result_reset < clear_secrets

    success_start = connect.index("Application::GetInstance().Schedule(")
    continuation = connect[success_start:]
    unlock_idx = continuation.index("continuation_lock.unlock();")
    report_idx = continuation.index("TryReportProvisioningAuthenticated(", unlock_idx)
    assert '"wifi_success_after_ble_teardown", generation' in continuation[report_idx:report_idx + 180]
    assert unlock_idx < report_idx

    assert '"custom_data_token", generation' in custom
    assert '"custom_data_code", generation' in custom


# ---------------------------------------------------------------------------
# FW21j: the BTC custom-data callback must never wait on the finalization mutex.
#        It only parses bounded TLVs into a generation-tagged snapshot and posts
#        to the Application task. The Application action applies device_id first,
#        then token/code under RunIfSetupGenerationCurrent, clears copies on all
#        exits, and triggers generation-aware reporting only after unlock.
# ---------------------------------------------------------------------------
def test_fw21j_custom_data_callback_is_nonblocking_and_application_owned():
    blufi = read("main/boards/common/blufi.cpp")
    custom = _custom_data_body()
    schedule_in_case = custom.index("Application::GetInstance().Schedule(")
    callback = custom[:schedule_in_case]

    for forbidden in (
        "Settings ",
        "bootstrap_token_.assign",
        "provisioning_code_.assign",
        "TryReportProvisioningAuthenticated",
        "provisioning_finalization_mutex_",
        "RunIfSetupGenerationCurrent",
    ):
        assert forbidden not in callback

    assert "BlufiCustomDataSnapshot snapshot" in custom
    assert "std::array<char, 64> device_id" in blufi
    assert "std::array<char, 64> token" in blufi
    assert "std::array<char, 16> code" in blufi
    assert "std::min<size_t>(len, snapshot.token.size())" in custom
    assert "std::min<size_t>(len, snapshot.code.size())" in custom
    assert "std::min<size_t>(len, snapshot.device_id.size())" in custom
    assert "snapshot.expected_generation = session_generation;" in custom
    assert "Application::GetInstance().Schedule(" in custom
    assert "SecureClearCustomDataSnapshot(snapshot);" in custom

    scheduled = blufi[blufi.index("Application::GetInstance().Schedule(", blufi.index("BlufiCustomDataSnapshot snapshot")) :]
    gate_idx = scheduled.index("RunIfSetupGenerationCurrent(")
    device_store = scheduled.index('"claim_device_id"', gate_idx)
    code_assign = scheduled.index("provisioning_code_.assign", device_store)
    token_assign = scheduled.index("bootstrap_token_.assign", code_assign)
    token_store = scheduled.index('"bootstrap_token"', token_assign)
    report_idx = scheduled.index("TryReportProvisioningAuthenticated(", token_store)
    clear_idx = scheduled.index("secure_context->Clear();", report_idx)
    assert gate_idx < device_store < code_assign < token_assign < token_store < report_idx < clear_idx


# ---------------------------------------------------------------------------
# FW21k: Application::Schedule stores std::function, whose construction/moves
#        may copy a lambda. Secret arrays therefore must not be captured by
#        value. A single heap-owned secure context is referenced through a
#        copyable pointer handle; every handle copy contains only a pointer,
#        and the context zeroizes both explicitly after apply and in its
#        destructor if queued work is discarded.
# ---------------------------------------------------------------------------
def test_fw21k_custom_data_schedule_copies_only_secure_context_pointer():
    blufi = read("main/boards/common/blufi.cpp")
    custom = _custom_data_body()

    assert "[self, snapshot]" not in custom
    assert "struct BlufiCustomDataContext" in blufi
    assert "class BlufiCustomDataContextPtr" in blufi
    assert "BlufiCustomDataSnapshot snapshot;" in custom
    assert "new (std::nothrow) BlufiCustomDataContext" in custom
    assert "BlufiCustomDataContextPtr secure_context" in custom
    assert "[self, secure_context]" in custom

    allocate_idx = custom.index("new (std::nothrow) BlufiCustomDataContext")
    allocation_failure_idx = custom.index("if (raw_context == nullptr)", allocate_idx)
    allocation_failure_end = custom.index("}", allocation_failure_idx)
    allocation_failure = custom[allocation_failure_idx:allocation_failure_end]
    assert "SecureClearCustomDataSnapshot(snapshot);" in allocation_failure
    assert "break;" in allocation_failure
    for forbidden in (
        "Settings ",
        "bootstrap_token_.assign",
        "provisioning_code_.assign",
        "Application::GetInstance().Schedule",
    ):
        assert forbidden not in allocation_failure

    transfer_idx = custom.index("raw_context->snapshot = snapshot;", allocation_failure_idx)
    stack_clear_idx = custom.index("SecureClearCustomDataSnapshot(snapshot);", transfer_idx)
    schedule_idx = custom.index("Application::GetInstance().Schedule(", stack_clear_idx)
    assert allocate_idx < allocation_failure_idx < transfer_idx < stack_clear_idx < schedule_idx

    context_start = blufi.index("struct BlufiCustomDataContext")
    context_end = blufi.index("class BlufiCustomDataContextPtr", context_start)
    context = blufi[context_start:context_end]
    assert "~BlufiCustomDataContext()" in context
    assert "SecureClearCustomDataSnapshot(snapshot);" in context

    handle_start = blufi.index("class BlufiCustomDataContextPtr")
    handle_end = blufi.index("Blufi& Blufi::GetInstance()", handle_start)
    handle = blufi[handle_start:handle_end]
    assert "BlufiCustomDataContext* context_" in handle
    assert "BlufiCustomDataSnapshot" not in handle
    assert "Retain()" in handle
    assert "Release()" in handle

    scheduled = custom[schedule_idx:]
    assert "secure_context->Clear();" in scheduled


# ---------------------------------------------------------------------------
# FW21l: RestartForSetup advances setup_generation_ before the old Bluetooth
#        host is drained. Custom data from that old connection must therefore
#        use the generation captured when BLE_CONNECT established the session,
#        never re-sample the current setup generation while handling data.
# ---------------------------------------------------------------------------
def test_fw21l_custom_data_is_bound_to_ble_connection_generation():
    blufi = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    custom = _custom_data_body()

    assert "std::atomic<uint64_t> ble_session_state_" in header
    for phase in ("kStopping", "kAccepting", "kConnected", "kDisconnected"):
        assert phase in blufi

    connect_start = blufi.index("case ESP_BLUFI_EVENT_BLE_CONNECT:")
    disconnect_start = blufi.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT:", connect_start)
    connect = blufi[connect_start:disconnect_start]
    state_load = connect.index("ble_session_state_.load(")
    accepting_check = connect.index("BleSessionPhase::kAccepting", state_load)
    generation_decode = connect.index("DecodeBleSessionGeneration(expected_state)")
    connected_encode = connect.index("BleSessionPhase::kConnected", generation_decode)
    connect_cas = connect.index("ble_session_state_.compare_exchange_strong(", connected_encode)
    assert state_load < accepting_check < generation_decode < connected_encode < connect_cas
    assert "ble_session_state_.store(" not in connect

    disconnect_end = blufi.index("case ESP_BLUFI_EVENT_GET_WIFI_STATUS:", disconnect_start)
    disconnect = blufi[disconnect_start:disconnect_end]
    connected_check = disconnect.index("BleSessionPhase::kConnected")
    disconnected_encode = disconnect.index("BleSessionPhase::kDisconnected", connected_check)
    disconnect_cas = disconnect.index(
        "ble_session_state_.compare_exchange_strong(", disconnected_encode
    )
    accepting_encode = disconnect.index("BleSessionPhase::kAccepting", disconnect_cas)
    rearm_cas = disconnect.index(
        "ble_session_state_.compare_exchange_strong(", accepting_encode
    )
    assert connected_check < disconnected_encode < disconnect_cas < accepting_encode < rearm_cas
    assert "ble_session_state_.store(" not in disconnect

    restart = _function_body(blufi, "esp_err_t Blufi::RestartForSetup")
    restart_invalidate = restart.index("ble_session_state_.exchange(")
    restart_stopping = restart.index("BleSessionPhase::kStopping", restart_invalidate)
    restart_generation = restart.index("setup_generation_.fetch_add(1)")
    assert restart_invalidate < restart_stopping < restart_generation

    deinit = _function_body(blufi, "esp_err_t Blufi::_deinit_impl")
    deinit_invalidate = deinit.index("ble_session_state_.exchange(")
    deinit_host = deinit.index("_host_deinit()")
    assert "BleSessionPhase::kStopping" in deinit[deinit_invalidate:deinit_host]
    assert deinit_invalidate < deinit_host

    init = _function_body(blufi, "esp_err_t Blufi::_init_impl")
    host_ready = init.index("_host_and_cb_init()")
    accepting_publish = init.index("BleSessionPhase::kAccepting", host_ready)
    assert host_ready < accepting_publish

    assert "snapshot.expected_generation = setup_generation_.load();" not in custom
    state_idx = custom.index("ble_session_state_.load(")
    connected_idx = custom.index("BleSessionPhase::kConnected", state_idx)
    session_generation_idx = custom.index("DecodeBleSessionGeneration(session_state)", connected_idx)
    snapshot_generation_idx = custom.index(
        "snapshot.expected_generation = session_generation;", session_generation_idx
    )
    assert state_idx < connected_idx < session_generation_idx < snapshot_generation_idx


# ---------------------------------------------------------------------------
# FW22: blufi.cpp — never HTTPS/TLS while BLE active. Both report lanes prove
#       this structurally: the authenticated lane tears BLE down BEFORE the POST
#       (FW19); and the connect-SUCCESS lane sends the conn-report, then defers
#       deinit()+report to a Schedule() where deinit runs before the report. The
#       only Report() call inside the live wifi-connect task (no BLE teardown
#       there) is the FAILED-status lane, which by design keeps BLE up for retry
#       — that lane is plain HTTP failure-status, not a claim/TLS poll.
# ---------------------------------------------------------------------------
def test_fw22_no_authenticated_post_or_claim_refresh_inline_while_ble_up():
    req = _station_connect_helper_body()
    conn_idx = req.index("if (credentials_committed)")
    fail_idx = req.index("} else {", conn_idx)
    success = req[conn_idx:fail_idx]

    # In the success branch the authenticated report + claim refresh must NOT be
    # called inline before the BLE teardown — they live inside the deferred
    # Application Schedule() lambda, after deinit().
    sched_idx = success.index("Application::GetInstance().Schedule(")
    inline_region = success[:sched_idx]
    assert "TryReportProvisioningAuthenticated" not in inline_region, (
        "the authenticated report must not run inline (BLE still up) — it belongs "
        "in the deferred Schedule() after deinit()"
    )
    assert "SchedulePendingTbotClaimRefresh" not in inline_region, (
        "the claim refresh (TLS poll) must not run inline while BLE is up"
    )

    # Inside the deferred lambda, deinit() precedes both the authenticated report
    # and the claim refresh (so TLS only ever runs after BLE is down).
    sched_body = success[sched_idx:]
    deinit_pos = sched_body.index("self->CompleteSuccessfulProvisioningTeardown")
    report_pos = sched_body.index("self->TryReportProvisioningAuthenticated", deinit_pos)
    refresh_pos = sched_body.index("SchedulePendingTbotClaimRefresh", report_pos)
    assert deinit_pos < report_pos < refresh_pos


# ---------------------------------------------------------------------------
# FW23: blufi.cpp — the TLV custom-data parser bounds every field and caps the
#       two secrets (token <= 64, code <= 16). A truncated frame must break out
#       of the loop without copying past the payload (no over-read), and a TLV
#       length larger than the cap must be clamped, not trusted.
# ---------------------------------------------------------------------------
def test_fw23_custom_data_tlv_is_bounded_and_secrets_are_capped():
    custom = _custom_data_body()

    # The header read requires two bytes (tag + len) before advancing.
    assert "while (offset + 2 <= data_len)" in custom

    # Value bytes must be proven in-range before use (no over-read).
    assert "if (offset + static_cast<int>(len) > data_len)" in custom
    assert "TLV truncated at tag=0x%02x" in custom
    assert "break;" in custom

    # Fixed-size snapshot caps token/device id at 64 and code at 16.
    assert "std::min<size_t>(len, snapshot.token.size())" in custom
    assert "std::min<size_t>(len, snapshot.code.size())" in custom
    assert "bootstrap_token_.assign(" in custom
    assert "snapshot.token.data(), snapshot.token_len" in custom
    assert "provisioning_code_.assign(" in custom
    assert "snapshot.code.data(), snapshot.code_len" in custom

    # Defense-in-depth: the assign must use safe_len, not the raw len.
    assert "bootstrap_token_.assign(value, len)" not in custom
    assert "provisioning_code_.assign(value, len)" not in custom


# ---------------------------------------------------------------------------
# FW24: blufi.cpp — the custom-data handler logs ONLY byte counts for the two
#       secrets, never the secret value (no %s on token/code, no .c_str() of
#       either). The token-byte-count and code-byte-count logs must use %u of
#       safe_len, which is a length, not the credential.
# ---------------------------------------------------------------------------
def test_fw24_custom_data_logs_byte_counts_not_secret_values():
    custom = _custom_data_body()

    # Byte-count logs only.
    assert '"Received bootstrap token (%u bytes)"' in custom
    assert '"Received provisioning code (%u bytes)"' in custom

    # No ESP_LOG line in this handler may format the raw token/code value.
    for line in custom.splitlines():
        if "ESP_LOG" in line:
            assert "bootstrap_token_.c_str()" not in line
            assert "provisioning_code_.c_str()" not in line
            # `value` is the raw TLV bytes (token/code cleartext) — never log it.
            if "%s" in line:
                assert "value" not in line.split(",", 1)[-1], (
                    "a %s log line in the TLV handler must not pass the raw `value`"
                )


# ---------------------------------------------------------------------------
# FW25: blufi.cpp — the bootstrap token is persisted to NVS (it has a durable
#       home) but the provisioning code is RAM-only (never written to NVS). This
#       is the structural reason FW10 keeps RAM secrets on a failed report: the
#       code cannot be reloaded, so a clear would strand the in-session retry.
# ---------------------------------------------------------------------------
def test_fw25_token_persisted_to_nvs_but_code_is_ram_only():
    custom = _custom_data_body()

    # Token is written to the websocket NVS namespace.
    assert 'Settings websocket_settings("websocket", true);' in custom
    assert '"bootstrap_token", self->bootstrap_token_' in custom

    # The provisioning code is NEVER persisted to NVS anywhere in the file.
    blufi = read("main/boards/common/blufi.cpp")
    for marker in (
        '.SetString("provisioning_code"',
        'SetString("code", provisioning_code_)',
    ):
        assert marker not in blufi, (
            "the provisioning code must remain RAM-only; persisting it to NVS "
            "would defeat the secret-lifetime model"
        )
    # The code never appears as an argument to any Settings::Set* call.
    for line in blufi.splitlines():
        if "SetString(" in line or "SetInt(" in line:
            assert "provisioning_code_" not in line


# ---------------------------------------------------------------------------
# FW26: blufi.cpp — deinit() is ordered and transactionally idempotent. A
#       completed transaction returns early, while partial failures leave the
#       failed layer active for a retry without repeating successful layers.
# ---------------------------------------------------------------------------
def test_fw26_deinit_is_idempotent_and_host_precedes_controller():
    blufi = read("main/boards/common/blufi.cpp")
    fn_idx = blufi.index("esp_err_t Blufi::deinit()")
    fn_end = blufi.index("#ifdef CONFIG_BT_BLUEDROID_ENABLED", fn_idx)
    body = blufi[fn_idx:fn_end]

    # Idempotency: only a fully completed transaction may return stale success,
    # and a previously failed teardown surfaces ESP_ERR_INVALID_STATE.
    guard_idx = body.index("if (m_deinited && !host_active_ && !controller_active_) {")
    return_idx = body.index(
        "return teardown_failed_.load() ? ESP_ERR_INVALID_STATE : ESP_OK;", guard_idx
    )
    host_guard_idx = body.index("if (host_active_)", return_idx)
    controller_guard_idx = body.index("if (controller_active_)", host_guard_idx)
    set_deinited = body.index("m_deinited = true;", controller_guard_idx)
    assert guard_idx < return_idx < host_guard_idx < controller_guard_idx < set_deinited

    # Ordering: host deinit precedes controller deinit.
    host_idx = body.index("_host_deinit();")
    controller_idx = body.index("_controller_deinit();")
    assert host_idx < controller_idx, (
        "host deinit must run before controller deinit so the profile/host is "
        "torn down before the controller is removed"
    )

    # The scan event handler is unregistered during deinit (no dangling handler).
    assert "esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE," in body
    assert "scan_event_instance_ = nullptr;" in body


# ---------------------------------------------------------------------------
# FW27: blufi.cpp — GetBleState() is a strict priority machine: a fired hard
#       timeout (ble_timed_out_) DOMINATES every other state (so wifi_board.cc
#       can re-init on kTimeout per FW2), then off (not inited / deinited), then
#       connected, then advertising. The timeout check must come first; the
#       off check must come before the connected check.
# ---------------------------------------------------------------------------
def test_fw27_get_ble_state_priority_timeout_dominates_then_off():
    blufi = read("main/boards/common/blufi.cpp")
    fn_idx = blufi.index("Blufi::BleState Blufi::GetBleState() const")
    fn_end = blufi.index("const char* Blufi::GetBleStateString()", fn_idx)
    body = blufi[fn_idx:fn_end]

    timeout_idx = body.index("if (ble_timed_out_)")
    off_idx = body.index("if (!inited_ || m_deinited)")
    connected_idx = body.index("if (m_ble_is_connected)")
    advertising_default = body.index("return BleState::kAdvertising;")

    assert timeout_idx < off_idx < connected_idx < advertising_default, (
        "GetBleState() priority must be timeout > off > connected > advertising"
    )
    # Each branch returns the matching state.
    assert "return BleState::kTimeout;" in body[timeout_idx:off_idx]
    assert "return BleState::kOff;" in body[off_idx:connected_idx]
    assert "return BleState::kConnected;" in body[connected_idx:advertising_default]


# ---------------------------------------------------------------------------
# FW28: blufi.cpp — the wifi-connect FAIL lane never tears BLE down (the phone
#       retries Wi-Fi on the SAME session) and never clears state-machine flags
#       in a way that leaves a stale "connecting". On a fail it must reset
#       m_sta_is_connecting/m_sta_connected/m_sta_got_ip to false and send the
#       ESP_BLUFI_STA_CONN_FAIL conn-report so the phone surfaces
#       WIFI_CONNECT_FAILED. (Complements FW10 which guards the secret-clear.)
# ---------------------------------------------------------------------------
def test_fw28_wifi_connect_fail_lane_resets_flags_and_reports_fail():
    req = _station_connect_helper_body()
    conn_idx = req.index("if (credentials_committed)")
    else_idx = req.index("} else {", conn_idx)
    fail = req[else_idx:]

    # State-machine flags are cleared so a later GET_WIFI_STATUS does not report
    # a stale "connecting" / "connected".
    assert "self->m_sta_is_connecting.store(false);" in fail
    assert "self->m_sta_connected = false;" in fail
    assert "self->m_sta_got_ip = false;" in fail

    # The FAIL conn-report is sent (phone -> WIFI_CONNECT_FAILED).
    assert "esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL" in fail

    # The fail lane must NOT tear BLE down (same-session retry) — re-asserted in
    # this scope so the invariant is anchored to the FAIL branch specifically.
    assert "self->deinit();" not in fail, (
        "the wifi-connect FAIL lane must keep BLE up for same-session Wi-Fi retry"
    )

    # GET_WIFI_STATUS mid-connect must report STA_CONNECTING (not fail/success),
    # so the phone keeps its waiting screen rather than declaring failure early.
    handle = _handle_event_body()
    status_start = handle.index("case ESP_BLUFI_EVENT_GET_WIFI_STATUS:")
    status_end = handle.index("case ESP_BLUFI_EVENT_RECV_STA_BSSID:", status_start)
    status_body = handle[status_start:status_end]
    assert "else if (m_sta_is_connecting.load())" in status_body
    connecting_idx = status_body.index("else if (m_sta_is_connecting.load())")
    connecting_report = status_body.index("ESP_BLUFI_STA_CONNECTING", connecting_idx)
    assert connecting_idx < connecting_report, (
        "an in-progress connect must surface STA_CONNECTING on status poll"
    )


# ---------------------------------------------------------------------------
# FW29: blufi.cpp — ClearProvisioningSecrets() must ALSO erase the NVS-persisted
#       bootstrap token, not just the in-RAM copy. The bootstrap token has a
#       durable NVS home (FW25: it is written to the "websocket"/"bootstrap_token"
#       key at custom-data arrival). The product contract
#       (docs/product/device-provisioning.md: "Successful backend report zeroizes
#       and clears the bootstrap token and provisioning code") therefore requires
#       the at-rest copy to be erased too — otherwise a stale, single-attempt
#       credential survives at rest after a successful report and after a factory
#       reset.
#
#       SAFE-end-state rationale (why erasing on success does not strand claim):
#         * ClearProvisioningSecrets() is called ONLY on report SUCCESS (FW6); the
#           failure path (FW7) retains everything for retry, so erasing here never
#           touches the retry credential.
#         * The wifi-success lane schedules SchedulePendingTbotClaimRefresh()
#           (which reads NVS "bootstrap_token" in application.cc) synchronously in
#           the same Application lambda, BEFORE the async provisioning POST returns
#           and defers the clear — so the claim refresh reads the token first.
#         * application.cc already erases this exact NVS key on every terminal
#           claim outcome (SetString("bootstrap_token", "")), i.e. the firmware
#           already treats it as single-use that must not persist at rest.
# ---------------------------------------------------------------------------
def test_fw29_clear_provisioning_secrets_erases_nvs_token_and_ram():
    blufi = read("main/boards/common/blufi.cpp")

    clear_def = blufi.index("void Blufi::ClearProvisioningSecrets(bool preserve_claim_token)")
    clear_body = blufi[clear_def:blufi.index("\n}", clear_def)]

    # RAM zeroize + clear must remain (the existing FW6 behavior is preserved).
    assert "bootstrap_token_.clear();" in clear_body, (
        "the RAM bootstrap-token clear must be preserved"
    )
    assert "provisioning_code_.clear();" in clear_body, (
        "the RAM provisioning-code clear must be preserved"
    )
    assert "std::fill(bootstrap_token_.begin(), bootstrap_token_.end(), '\\0');" in clear_body, (
        "the RAM bootstrap-token zeroize must be preserved"
    )

    # NEW invariant: the at-rest (NVS) bootstrap token must be erased too. The
    # function must open the "websocket" namespace read-write and EraseKey the
    # "bootstrap_token" key, fulfilling the product contract that a successful
    # report clears the bootstrap token (which has a durable NVS home per FW25).
    assert 'Settings websocket_settings("websocket", true);' in clear_body, (
        "ClearProvisioningSecrets() must open the websocket NVS namespace "
        "read-write to erase the at-rest bootstrap token"
    )
    assert 'EraseKey("bootstrap_token")' in clear_body, (
        "ClearProvisioningSecrets() must EraseKey(\"bootstrap_token\") so the "
        "at-rest single-attempt credential does not survive a successful report "
        "or a factory reset (docs/product/device-provisioning.md contract)"
    )

    # The NVS erase must operate on the same key the custom-data handler writes
    # (FW25), so the at-rest copy that was persisted is the one removed.
    assert '"bootstrap_token", self->bootstrap_token_' in blufi, (
        "sanity: the bootstrap token is persisted to the websocket NVS key that "
        "ClearProvisioningSecrets() must now erase"
    )


# ---------------------------------------------------------------------------
# FW30: blufi.cpp — the NVS bootstrap-token erase must live ONLY inside
#       ClearProvisioningSecrets() (the success-only clear, FW6) and must NOT be
#       reachable from the FAILED/retain path. ClearProvisioningSecrets is only
#       ever called from the two `if (ok)` success gates (FW6/FW10); the failure
#       branches log retention and never call it (FW7/FW10). Asserting the erase
#       is exclusive to this function proves the at-rest token is retained on a
#       failed report (so an in-session / post-reboot retry still has it).
# ---------------------------------------------------------------------------
def test_fw30_nvs_token_erase_is_success_only_not_on_retain_path():
    blufi = read("main/boards/common/blufi.cpp")

    # The EraseKey("bootstrap_token") must appear exactly once in the file, and
    # that single occurrence must be inside ClearProvisioningSecrets().
    assert blufi.count('EraseKey("bootstrap_token")') == 1, (
        "the at-rest bootstrap-token erase must exist exactly once (only the "
        "success-only ClearProvisioningSecrets path), never duplicated onto a "
        "failure/retain lane"
    )
    erase_idx = blufi.index('EraseKey("bootstrap_token")')
    clear_def = blufi.index("void Blufi::ClearProvisioningSecrets(bool preserve_claim_token)")
    clear_end = blufi.index("\n}", clear_def)
    assert clear_def < erase_idx < clear_end, (
        "the NVS erase must be scoped inside ClearProvisioningSecrets(), which is "
        "only called on report SUCCESS (FW6/FW10)"
    )

    # The two retain (failure) paths must NOT erase the NVS token: they log
    # retention and leave ClearProvisioningSecrets uncalled (FW7/FW10). Re-anchor
    # on the retain logs to prove the erase is not on these lanes.
    auth_fail_log = "Provisioning report failed after BLE teardown; secrets retained for retry"
    wifi_fail_log = "Provisioning secrets retained for WiFi retry"
    assert auth_fail_log in blufi
    assert wifi_fail_log in blufi
    for log in (auth_fail_log, wifi_fail_log):
        log_idx = blufi.index(log)
        # Inspect a window around the retain log line; the NVS erase token must
        # not appear inline on a retain branch.
        window = blufi[log_idx - 200:log_idx + 200]
        assert 'EraseKey("bootstrap_token")' not in window, (
            "a failed report must RETAIN the at-rest bootstrap token for retry; "
            "the NVS erase must not be reachable on the retain path"
        )


# ---------------------------------------------------------------------------
# FW31: unclaimed provisioning must not reserve the three audio worker stacks.
#       Real hardware reproduced a silent claim hang after Wi-Fi association:
#       the 4 KiB claim/connect workers could not be created while idle audio
#       tasks retained roughly 39 KiB of internal SRAM.
# ---------------------------------------------------------------------------
def test_fw31_unclaimed_boot_defers_audio_workers_until_claim_confirmation():
    application = read("main/application.cc")
    initialize = _function_body(application, "void Application::Initialize")
    confirm = _function_body(
        application, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )

    assert "if (IsDeviceClaimed())" in initialize
    claimed_gate = initialize.index("if (IsDeviceClaimed())")
    audio_start = initialize.index("audio_service_.Start()")
    assert claimed_gate < audio_start

    assert "FinishClaimActivationAfterLocalAssetsReady()" in confirm
    finish = _function_body(
        application, "bool Application::FinishClaimActivationAfterLocalAssetsReady"
    )
    assert "audio_service_.Start();" in finish
    assert finish.index("audio_service_.Start();") < finish.index(
        "audio_service_.EnableWakeWordDetection(true)"
    )


def test_fw31b_stopped_configuration_audio_cannot_enter_a_fake_audio_test_state():
    application = read("main/application.cc")
    audio_header = read("main/audio/audio_service.h")
    toggle = _function_body(application, "void Application::HandleToggleChatEvent")
    listen = _function_body(application, "void Application::HandleStartListeningEvent")

    assert "bool IsRunning() const" in audio_header
    for body in (toggle, listen):
        wifi_branch = body[body.index("state == kDeviceStateWifiConfiguring") :]
        running_guard = wifi_branch.index("audio_service_.IsRunning()")
        enable = wifi_branch.index("audio_service_.EnableAudioTesting(true)")
        transition = wifi_branch.index("SetDeviceState(kDeviceStateAudioTesting)")
        assert running_guard < enable < transition
        guard_body = wifi_branch[running_guard:enable]
        assert "return;" in guard_body


def test_fw31c_stopped_configuration_sounds_drop_before_queueing_stale_audio():
    audio = read("main/audio/audio_service.cc")
    play = _function_body(audio, "void AudioService::PlaySound")

    stopped_guard = play.index("if (service_stopped_)")
    codec_enable = play.index("codec_->EnableOutput(true)")
    enqueue = play.index("PushPacketToDecodeQueue")
    assert stopped_guard < codec_enable < enqueue
    assert "return;" in play[stopped_guard:codec_enable]


# ---------------------------------------------------------------------------
# FW32: allocation failure in the Wi-Fi completion worker must be observable and
#       must reset the in-progress flags. Never leave Android waiting forever
#       after the robot has accepted credentials.
# ---------------------------------------------------------------------------
def test_fw32_wifi_completion_worker_creation_failure_cannot_silently_hang():
    blufi = read("main/boards/common/blufi.cpp")
    helper = _function_body(blufi, "void Blufi::StartStationConnectFromCredentials")

    assert "BaseType_t created = xTaskCreate(" in helper
    assert "if (created != pdPASS)" in helper
    failure = helper[helper.index("if (created != pdPASS)") :]
    assert "m_wifi_connect_task_started.store(false);" in failure
    assert "m_sta_is_connecting.store(false);" in failure
    assert "Failed to create BluFi WiFi completion task" in failure


def test_fw33_boot_reentry_starts_a_new_generation_and_clean_ble_session():
    wifi_board = read("main/boards/common/wifi_board.cc")
    start = _start_wifi_config_body(wifi_board)
    blufi = read("main/boards/common/blufi.cpp")
    restart = _function_body(blufi, "esp_err_t Blufi::RestartForSetup")

    assert "blufi.TryReserveProvisioningSession()" in start
    assert "blufi.RestartForSetup();" in start
    assert "CancelBleSetupTimeout();" in restart
    assert "setup_generation_.fetch_add" in restart
    assert "deinit();" in restart
    assert "init();" in restart
    assert restart.index("deinit();") < restart.index("init();")


def test_fw34_stale_timeout_and_wifi_workers_cannot_mutate_new_boot_generation():
    blufi = read("main/boards/common/blufi.cpp")
    arm = _function_body(blufi, "void Blufi::StartBleSetupTimeout")
    timeout = _function_body(blufi, "void Blufi::_ble_setup_timeout_cb")
    connect = _function_body(blufi, "void Blufi::StartStationConnectFromCredentials")

    assert "reinterpret_cast<void*>(static_cast<uintptr_t>(generation))" in arm
    assert "reinterpret_cast<uintptr_t>(arg)" in timeout
    assert "self->ble_timeout_generation_.load()" not in timeout.split(
        "Application::GetInstance().Schedule", 1
    )[0]
    assert "generation != self->ble_timeout_generation_.load()" in timeout
    assert "Ignoring stale BLE setup timeout" in timeout

    assert "const uint32_t generation = setup_generation_.load()" in connect
    assert "generation != self->setup_generation_.load()" in connect
    assert "Ignoring stale BluFi WiFi completion worker" in connect


def test_fw35_receiving_ssid_does_not_reopen_wifi_worker_single_flight():
    blufi = read("main/boards/common/blufi.cpp")
    event = _function_body(blufi, "case ESP_BLUFI_EVENT_RECV_STA_SSID:")
    assert "m_wifi_connect_task_started = false" not in event


def test_fw36_rapid_boot_wifi_config_entries_are_epoch_scoped_and_single_flight():
    header = read("main/boards/common/wifi_board.h")
    wifi_board = read("main/boards/common/wifi_board.cc")
    enter = _function_body(wifi_board, "void WifiBoard::EnterWifiConfigMode")
    request = _function_body(wifi_board, "void WifiBoard::RequestWifiConfigMode")

    assert "wifi_config_entry_pending_" in header
    assert "RequestWifiConfigMode(true);" in enter
    assert "wifi_config_entry_pending_.compare_exchange_strong" in request
    assert "WiFi config request coalesced while entry is pending" in request
    assert "wifi_config_entry_pending_.store(false)" in request


def test_fw37_wifi_completion_generation_is_captured_before_spawn_and_rechecked_on_app_task():
    blufi = read("main/boards/common/blufi.cpp")
    helper = _function_body(blufi, "void Blufi::StartStationConnectFromCredentials")

    capture = helper.index("const uint32_t generation = setup_generation_.load();")
    settle_delay = helper.index("vTaskDelay(pdMS_TO_TICKS(500));")
    spawn = helper.index("xTaskCreate(", capture)
    release = helper.index("ReleaseBleForStationAssociation", spawn)
    start_station = helper.index("wifi.StartStation();", release)
    assert capture < settle_delay < spawn < release < start_station
    post_delay = helper[settle_delay:spawn]
    assert "generation != setup_generation_.load()" in post_delay
    assert "generation != self->setup_generation_.load()" in helper
    continuation = helper[helper.index("Application::GetInstance().Schedule") :]
    assert "generation != self->setup_generation_.load()" in continuation
    assert continuation.index("generation != self->setup_generation_.load()") < continuation.index(
        "self->CompleteSuccessfulProvisioningTeardown"
    )
    assert "delete ctx;" in helper


def test_fw38_password_fallback_is_generation_scoped_and_spawn_failure_is_recoverable():
    blufi = read("main/boards/common/blufi.cpp")
    fallback = _function_body(blufi, "void Blufi::ScheduleStationConnectFallback")

    assert "const uint32_t generation = setup_generation_.load();" in fallback
    assert "generation != self->setup_generation_.load()" in fallback
    assert "BaseType_t created = xTaskCreate(" in fallback
    assert "if (created != pdPASS)" in fallback
    failure = fallback[fallback.index("if (created != pdPASS)") :]
    assert "Application::GetInstance().Schedule" in failure
    assert "generation != setup_generation_.load()" in failure
    assert 'StartStationConnectFromCredentials("password_fallback_task_create_failed")' in failure


def test_fw39_failed_wifi_candidate_is_transactional_and_retryable_without_factory_reset():
    blufi = read("main/boards/common/blufi.cpp")
    ssid_header = read("components/esp-wifi-connect/include/ssid_manager.h")
    ssid_source = read("components/esp-wifi-connect/ssid_manager.cc")
    helper = _function_body(blufi, "void Blufi::StartStationConnectFromCredentials")
    restart = _function_body(blufi, "esp_err_t Blufi::RestartForSetup")

    assert "BeginSsidTransaction(ssid, password)" in helper
    assert "CommitSsidTransaction(ssid_transaction)" in helper
    assert "RollbackSsidTransaction(ssid_transaction)" in helper
    assert "ssid_transaction_id_.exchange(0)" in restart
    assert "RollbackSsidTransaction(stale_ssid_transaction)" in restart

    stage_idx = helper.index("BeginSsidTransaction(ssid, password)")
    station_idx = helper.index("wifi.StartStation()")
    commit_idx = helper.index("CommitSsidTransaction(ssid_transaction)")
    success_idx = helper.index("if (credentials_committed)")
    failure_branch_idx = helper.index("} else {", success_idx)
    failure_idx = helper.index("RestoreBleAfterStationFailure", failure_branch_idx)
    rollback_idx = helper.index("RollbackSsidTransaction(ssid_transaction)", commit_idx)
    assert stage_idx < station_idx < commit_idx < rollback_idx < success_idx < failure_idx

    # The candidate must not survive reboot after a rejected password, while
    # BLE setup is restored automatically for an immediate corrected retry.
    assert "SsidManager::GetInstance().AddSsid(ssid, password);" not in helper[:success_idx]
    failure = helper[failure_branch_idx:]
    assert "m_provisioned = false;" in failure
    assert "RestoreBleAfterStationFailure(generation)" in failure
    assert "ClearProvisioningSecrets" not in failure

    for method in (
        "BeginSsidTransaction",
        "CommitSsidTransaction",
        "RollbackSsidTransaction",
    ):
        assert method in ssid_header

    # A completion worker from an invalidated setup generation carries its own
    # opaque transaction id; it cannot commit or rollback a newer candidate.
    assert "uint32_t ssid_transaction;" in helper
    assert "task_ctx->ssid_transaction" in helper
    assert "std::atomic<uint32_t> ssid_transaction_id_" in read(
        "main/boards/common/blufi.h"
    )

    # The hot provisioning path keeps only compact inverse metadata, not a copy
    # of every saved SSID/password. This covers overwrite, insert, and full-list
    # eviction rollback without multiplying credential heap use.
    assert "std::vector<SsidItem> transaction_backup_" not in ssid_header
    assert "transaction_old_password_" in ssid_header
    assert "transaction_evicted_item_" in ssid_header
    begin = _function_body(ssid_source, "uint32_t SsidManager::BeginSsidTransaction")
    restore = _function_body(ssid_source, "void SsidManager::RestoreActiveTransaction")
    assert "transaction_old_password_ = ssid_list_[index].password" in begin
    assert "transaction_evicted_item_ = std::move(ssid_list_.back())" in begin
    assert "ssid_list_.erase(ssid_list_.begin())" in restore
    assert "ssid_list_.push_back(transaction_evicted_item_)" in restore

    # Opaque ids make stale completion workers harmless: both terminal methods
    # reject a transaction that is no longer the active candidate.
    for signature in (
        "bool SsidManager::CommitSsidTransaction",
        "bool SsidManager::RollbackSsidTransaction",
    ):
        body = _function_body(ssid_source, signature)
        assert "active_transaction_id_ != transaction_id" in body
        assert "return false;" in body

    # NVS failure is a provisioning failure, never a false SUCCESS. The manager
    # restores the prior credential and BluFi stops STA so it reaches FAIL.
    commit = _function_body(ssid_source, "bool SsidManager::CommitSsidTransaction")
    assert "if (!SaveToNvs())" in commit
    assert "RestoreActiveTransaction();" in commit
    persistence_guard = helper[
        helper.index("if (wifi.IsConnected())") : helper.index(
            "if (credentials_committed)"
        )
    ]
    assert "CommitSsidTransaction(ssid_transaction)" in persistence_guard
    assert "wifi.StopStation();" in failure
    assert failure.index("wifi.StopStation();") < failure.index("RestoreBleAfterStationFailure")
    assert "if (credentials_committed)" in helper


def test_fw40_only_exact_candidate_wifi_can_commit_and_report_success():
    helper = _station_connect_helper_body()

    assert "connected_to_candidate" in helper
    assert "wifi.GetSsid()" in helper
    assert "std::array<uint8_t" in helper
    assert "candidate_ssid" in helper
    assert "candidate_ssid_len" in helper
    assert "task_ctx->candidate_ssid" in helper
    assert "memcmp" in helper

    context_idx = helper.index("WifiConnectTaskContext")
    capture_idx = helper.index("memcpy(ctx->candidate_ssid.data()")
    delay_idx = helper.index("vTaskDelay(pdMS_TO_TICKS(500));")
    station_idx = helper.index("wifi.StartStation()")
    assert context_idx < capture_idx < delay_idx < station_idx

    commit_guard = helper[
        helper.index("connected_to_candidate") : helper.index("if (credentials_committed)")
    ]
    assert "candidate_ssid_len" in commit_guard
    assert "candidate_ssid.data()" in commit_guard
    assert "self->m_sta_ssid" not in commit_guard
    assert "CommitSsidTransaction(ssid_transaction)" in commit_guard
    assert "wifi.StopStation();" not in commit_guard

    failure_start = helper.index("} else {", helper.index("if (credentials_committed)"))
    failure = helper[failure_start:]
    report_idx = failure.index("RestoreBleAfterStationFailure")
    assert "wifi.StopStation();" in failure
    assert failure.index("wifi.StopStation();") < report_idx
    assert "self->deinit();" not in failure
    assert "ClearProvisioningSecrets" not in failure


def test_fw41_early_connect_setup_failures_report_deterministic_sta_fail():
    helper = _station_connect_helper_body()

    init_failure = helper[
        helper.index("if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize())") :
        helper.index("vTaskDelay(pdMS_TO_TICKS(500));")
    ]
    allocation_failure = helper[
        helper.index("if (ctx == nullptr)") : helper.index("BaseType_t created = xTaskCreate(")
    ]

    for failure in (init_failure, allocation_failure):
        assert "RollbackSsidTransaction(ssid_transaction)" in failure
        assert "SendStationConnectFailureReport()" in failure
        assert failure.index("RollbackSsidTransaction(ssid_transaction)") < failure.index(
            "SendStationConnectFailureReport()"
        ) < failure.index("return;")

    blufi = read("main/boards/common/blufi.cpp")
    report = _function_body(blufi, "void Blufi::SendStationConnectFailureReport")
    assert "ESP_BLUFI_STA_CONN_FAIL" in report


def test_fw42_wifi_connect_single_flight_is_atomic_and_teardown_errors_are_preserved():
    header = read("main/boards/common/blufi.h")
    blufi = read("main/boards/common/blufi.cpp")
    helper = _station_connect_helper_body()
    deinit = _function_body(blufi, "esp_err_t Blufi::_deinit_impl")
    restart = _function_body(blufi, "esp_err_t Blufi::RestartForSetup")

    assert "std::atomic<bool> m_wifi_connect_task_started" in header
    assert "compare_exchange_strong" in helper
    assert "m_wifi_connect_task_started.store(false)" in blufi
    assert "m_wifi_connect_task_started = false" not in blufi
    assert "m_wifi_connect_task_started = true" not in blufi

    assert "first_error" in deinit
    assert "host_error" in deinit
    assert "controller_error" in deinit
    assert "return first_error;" in deinit
    assert "esp_err_t teardown_error = deinit();" in restart
    assert "if (teardown_error != ESP_OK)" in restart
    teardown_guard = restart[
        restart.index("esp_err_t teardown_error = deinit();") : restart.index("_security_deinit();")
    ]
    assert "return teardown_error;" in teardown_guard


def test_fw43_ssid_transactions_compensate_persist_failure_and_lock_all_list_access():
    header = read("components/esp-wifi-connect/include/ssid_manager.h")
    source = read("components/esp-wifi-connect/ssid_manager.cc")
    commit = _function_body(source, "bool SsidManager::CommitSsidTransaction")
    begin = _function_body(source, "uint32_t SsidManager::BeginSsidTransaction")
    upsert = _function_body(source, "void SsidManager::UpsertSsid")

    failure = commit[commit.index("if (!SaveToNvs())") :]
    assert failure.count("SaveToNvs()") >= 2
    assert failure.index("RestoreActiveTransaction();") < failure.rindex("SaveToNvs()")
    assert "Compensating WiFi credential restore failed" in failure

    assert "std::vector<SsidItem> GetSsidList() const;" in header
    assert "mutable std::mutex transaction_mutex_" in header
    get_list = _function_body(source, "std::vector<SsidItem> SsidManager::GetSsidList() const")
    assert "std::lock_guard<std::mutex>" in get_list
    for signature in (
        "void SsidManager::Clear",
        "void SsidManager::AddSsid",
        "uint32_t SsidManager::BeginSsidTransaction",
        "bool SsidManager::CommitSsidTransaction",
        "bool SsidManager::RollbackSsidTransaction",
        "void SsidManager::RemoveSsid",
        "void SsidManager::SetDefaultSsid",
    ):
        assert "std::lock_guard<std::mutex>" in _function_body(source, signature)

    for body in (begin, upsert):
        replace = body[body.index("if (ssid_list_[" if body is begin else "if (item.ssid == ssid)") :]
        assert "SecureClearString" in replace


def test_fw44_full_32_byte_ssid_uses_explicit_length_and_clears_local_credentials():
    header = read("main/boards/common/blufi.h")
    blufi = read("main/boards/common/blufi.cpp")
    helper = _station_connect_helper_body()
    ssid_event = _function_body(blufi, "case ESP_BLUFI_EVENT_RECV_STA_SSID:")

    assert "size_t m_sta_config_ssid_len_" in header
    assert "sizeof(m_sta_config.sta.ssid) - 1" not in ssid_event
    assert "sizeof(m_sta_config.sta.ssid)" in ssid_event
    assert "m_sta_config_ssid_len_ = ssid_n" in ssid_event
    assert "m_sta_config.sta.ssid[ssid_n] = '\\0'" not in ssid_event
    assert "memset(m_sta_config.sta.ssid, 0" in ssid_event

    assert "m_sta_config_ssid_len_" in helper
    assert "reinterpret_cast<const char*>(m_sta_config.sta.ssid)," in helper
    assert "SecureClearLocalString(ssid);" in helper
    assert "SecureClearLocalString(password);" in helper
    begin_idx = helper.index("BeginSsidTransaction(ssid, password)")
    clear_idx = helper.index("SecureClearLocalString(ssid);")
    delay_idx = helper.index("vTaskDelay(pdMS_TO_TICKS(500));")
    assert begin_idx < clear_idx < delay_idx

    station = read("components/esp-wifi-connect/wifi_station.cc")
    start_connect = _function_body(station, "void WifiStation::StartConnect")
    assert "strcpy((char *)wifi_config.sta.ssid" not in start_connect
    assert "memcpy(wifi_config.sta.ssid" in start_connect
    assert "sizeof(wifi_config.sta.ssid)" in start_connect


def test_fw45_teardown_failure_poison_blocks_all_blind_reinit_attempts():
    header = read("main/boards/common/blufi.h")
    blufi = read("main/boards/common/blufi.cpp")
    init = _function_body(blufi, "esp_err_t Blufi::init")
    deinit = _function_body(blufi, "esp_err_t Blufi::_deinit_impl")
    restart = _function_body(blufi, "esp_err_t Blufi::RestartForSetup")

    assert "std::atomic<bool> teardown_failed_" in header
    assert "teardown_failed_.load()" in init
    assert "return ESP_ERR_INVALID_STATE;" in init

    assert "teardown_failed_.store(true)" in deinit
    assert "teardown_failed_.store(false)" in deinit
    already_deinited = deinit[
        deinit.index("if (m_deinited &&") : deinit.index("m_deinited = true")
    ]
    assert "teardown_failed_.load()" in already_deinited
    assert "ESP_ERR_INVALID_STATE" in already_deinited

    poison_guard = restart[: restart.index("setup_generation_.fetch_add")]
    assert "teardown_failed_.load()" in poison_guard
    assert "return ESP_ERR_INVALID_STATE;" in poison_guard
    assert "init();" not in poison_guard


def test_fw46_station_stop_discards_stale_credential_snapshots():
    station = read("components/esp-wifi-connect/wifi_station.cc")
    stop = _function_body(station, "void WifiStation::Stop")

    # Scan results copy passwords into connect_queue_. A new provisioning
    # transaction for the same SSID must never consume a queued password from
    # the previous station session and then commit the newly staged password.
    assert "for (auto& record : connect_queue_)" in stop
    assert "std::fill(record.password.begin(), record.password.end(), '\\0');" in stop
    assert "connect_queue_.clear();" in stop

    clear_idx = stop.index("connect_queue_.clear();")
    stopped_idx = stop.index("xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED)")
    assert clear_idx < stopped_idx
