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
    start = wifi_board.index("void WifiBoard::StartWifiConfigMode()")
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
#      blufi.init() in the explicit Wi-Fi-config setup path.
# ---------------------------------------------------------------------------
def test_fw1_release_wake_word_before_ble_init_in_config_path():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    # The wake-word resource release must run before the BLE stack is brought up,
    # otherwise the AFE detection task contends with BLE during provisioning.
    assert "BeginWifiProvisioning" in body
    assert "blufi.init();" in body
    release_idx = body.index("BeginWifiProvisioning")
    init_idx = body.index("blufi.init();")
    assert release_idx < init_idx

    # It must also precede the GetBleState() read that gates the init decision.
    ble_state_idx = body.index("blufi.GetBleState();")
    assert release_idx < ble_state_idx


# ---------------------------------------------------------------------------
# FW2: wifi_board.cc — BleState::kTimeout is accepted for explicit setup
#      re-entry (re-init BLE + re-arm StartBleSetupTimeout).
# ---------------------------------------------------------------------------
def test_fw2_ble_timeout_accepted_for_explicit_setup_reentry():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    # kTimeout must be treated as a re-init trigger alongside kOff.
    assert "Blufi::BleState::kTimeout" in body
    assert "Blufi::BleState::kOff" in body

    timeout_idx = body.index("Blufi::BleState::kTimeout")
    init_idx = body.index("blufi.init();")
    rearm_idx = body.index("blufi.StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC)")

    # The kTimeout acceptance gates init(), and the hard-timeout is re-armed after.
    assert timeout_idx < init_idx < rearm_idx

    # The acceptance condition must explicitly include kTimeout in the if-guard.
    assert re.search(
        r"if\s*\(\s*ble_state\s*==\s*Blufi::BleState::kOff\s*\|\|\s*"
        r"ble_state\s*==\s*Blufi::BleState::kTimeout\s*\)",
        body,
    )


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

    assert "std::stable_sort" in send_body
    assert ".rssi" in send_body
    assert "seen_ssids" in send_body
    assert "kMaxBlufiWifiListApRecords" in send_body
    assert "resize(kMaxBlufiWifiListApRecords)" in send_body

    sort_idx = send_body.index("std::stable_sort")
    cap_idx = send_body.index("resize(kMaxBlufiWifiListApRecords)")
    send_idx = send_body.index("esp_blufi_send_wifi_list")
    assert sort_idx < cap_idx < send_idx


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
    assert "SsidManager::GetInstance().AddSsid" in helper
    assert "m_wifi_connect_task_started" in helper
    assert "blufi_wifi_conn" in helper
    assert "ESP_BLUFI_STA_CONN_SUCCESS" in helper
    assert "ESP_BLUFI_STA_CONN_FAIL" in helper


# ---------------------------------------------------------------------------
# FW4: blufi.cpp — esp_blufi_send_wifi_conn_report(... ESP_BLUFI_STA_CONN_SUCCESS
#      ...) appears BEFORE BLE deinit / stop-BLE scheduling.
# ---------------------------------------------------------------------------
def test_fw4_conn_success_report_precedes_ble_teardown():
    blufi = read("main/boards/common/blufi.cpp")

    success_idx = blufi.index("ESP_BLUFI_STA_CONN_SUCCESS")

    # The conn-success report fires first; BLE teardown is scheduled afterwards.
    teardown_idx = blufi.index(
        "WiFi provisioned; stopping BLE before claim refresh"
    )
    deinit_after = blufi.index("self->deinit();", success_idx)
    assert success_idx < teardown_idx
    assert success_idx < deinit_after

    # The success report and the deferred CancelBleSetupTimeout ordering holds.
    cancel_idx = blufi.index("self->CancelBleSetupTimeout();", success_idx)
    assert success_idx < cancel_idx


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
    deinit_idx = body.index("self->deinit();")
    reattempt_idx = body.index("self->TryReportProvisioningAuthenticated(reason);")
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
    clear_def = blufi.index("void Blufi::ClearProvisioningSecrets()")
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
#       "wifi_connect_failed") must keep the temporary provisioning secrets when
#       the backend FAILED-status report did not succeed, so the phone can retry
#       Wi-Fi over the SAME live BLE session. Per ADR-0018 the firmware only
#       clears secrets once the report is acknowledged; an unconditional clear
#       strands the in-session level-triggered retry (provisioning_code_ has no
#       NVS backup).
# ---------------------------------------------------------------------------
def test_fw10_wifi_connect_fail_lane_clears_secrets_only_on_report_success():
    blufi = read("main/boards/common/blufi.cpp")

    # Scope to the wifi-connect-FAIL region: from the STA_CONN_FAIL report down
    # to the end of the task body (vTaskDelete) so markers belong to this lane.
    fail_idx = blufi.index("ESP_BLUFI_STA_CONN_FAIL")
    region_end = blufi.index("vTaskDelete(nullptr);", fail_idx)
    region = blufi[fail_idx:region_end]

    # The fail lane must report the failed status with the wifi_connect_failed
    # reason (sanity: we are anchored on the right region).
    assert '"wifi_connect_failed"' in region

    # The bool return of the FAILED-status Report() must be captured and the
    # clear must be guarded by a success check — never called unconditionally.
    assert "ClearProvisioningSecrets" in region
    clear_idx = region.index("ClearProvisioningSecrets")

    # There must be an `if (ok)` (or `if (reported)`) success gate that precedes
    # the clear. We look for an `if (<bool>)` immediately governing the clear.
    guard = re.search(r"if\s*\(\s*ok\s*\)\s*\{[^}]*ClearProvisioningSecrets", region, re.DOTALL)
    assert guard is not None, (
        "wifi-connect-FAIL lane must clear secrets only inside an `if (ok)` "
        "success gate, not unconditionally"
    )

    # The bool must actually be captured from the Report() call in this region.
    assert re.search(r"bool\s+ok\s*=\s*ProvisioningStatusReporter::Report", region) is not None, (
        "the FAILED-status Report() return must be captured into a bool in the "
        "wifi-connect-FAIL lane"
    )

    # Negative: the clear must NOT sit directly after Report() with no guard
    # between them (the old unconditional-clear shape).
    unconditional = re.search(
        r'Report\([^;]*"wifi_connect_failed"\s*\)\s*;\s*self->ClearProvisioningSecrets\(\)\s*;',
        region,
        re.DOTALL,
    )
    assert unconditional is None, (
        "ClearProvisioningSecrets must not be called unconditionally right after "
        "the FAILED-status Report() in the wifi-connect-FAIL lane"
    )

    # This lane must NOT tear BLE down or open a new POST while BLE is active:
    # the phone retries Wi-Fi over the SAME BLE session here. No deinit(), no
    # new authenticated-report task in this region.
    assert "self->deinit();" not in region
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
    assert 'TryReportProvisioningAuthenticated("custom_data_token")' in custom, (
        "token TLV must re-attempt the authenticated report"
    )
    assert 'TryReportProvisioningAuthenticated("custom_data_code")' in custom, (
        "code TLV must re-attempt the authenticated report"
    )
    req_connect = _station_connect_helper_body()
    assert (
        'TryReportProvisioningAuthenticated("wifi_success_after_ble_teardown")'
        in req_connect
    ), "Wi-Fi success path must drive the authenticated report"

    # The precondition guard returns early when ANY piece is missing — this is
    # exactly what makes an out-of-order arrival harmless (it retries later).
    body = _try_report_body()
    guard_idx = body.index(
        "if (!wifi_connected || token_empty || code_empty || provisioning_report_in_flight_)"
    )
    early_return = body.index("return;", guard_idx)
    # The BLE-active defer block + the report task must come AFTER the guard.
    ble_guard = body.index("if (ble_state != BleState::kOff)")
    assert guard_idx < early_return < ble_guard, (
        "the missing-precondition guard must short-circuit BEFORE the BLE-active "
        "defer and the report task, so an out-of-order input self-heals"
    )


# ---------------------------------------------------------------------------
# FW16: blufi.cpp — the precondition guard must require ALL FOUR of:
#       wifi_connected, token present, code present, and no report already in
#       flight. Dropping any one of these either leaks a credential report with
#       a missing piece, or double-fires the network POST.
# ---------------------------------------------------------------------------
def test_fw16_authenticated_report_requires_wifi_token_code_and_not_in_flight():
    body = _try_report_body()

    # The four precondition locals are derived from real state.
    assert "const bool wifi_connected = WifiManager::GetInstance().IsConnected();" in body
    assert "const bool token_empty = bootstrap_token_.empty();" in body
    assert "const bool code_empty = provisioning_code_.empty();" in body

    # The single guard combines all four with OR of the negatives.
    assert (
        "if (!wifi_connected || token_empty || code_empty || provisioning_report_in_flight_)"
        in body
    ), "the report must be gated on wifi+token+code+not-in-flight together"

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
def test_fw17_in_flight_flag_set_before_task_and_cleared_on_all_exits():
    body = _try_report_body()

    set_idx = body.index("provisioning_report_in_flight_ = true;")
    task_idx = body.index("xTaskCreate(", set_idx)
    assert set_idx < task_idx, "the in-flight flag must be set before the report task is created"

    # Completion handler resets the flag (runs for both ok and !ok).
    completion_reset = body.index("self->provisioning_report_in_flight_ = false;")
    ok_branch = body.index("if (ok) {", completion_reset)
    assert completion_reset < ok_branch, (
        "the in-flight flag must be cleared unconditionally before the ok/else split"
    )

    # The task-create FAILURE path must also clear the flag (created != pdPASS).
    fail_idx = body.index("if (created != pdPASS)")
    fail_reset = body.index("provisioning_report_in_flight_ = false;", fail_idx)
    fail_return = body.index("return;", fail_idx)
    assert fail_idx < fail_reset < fail_return, (
        "a failed xTaskCreate must reset provisioning_report_in_flight_ so future "
        "retries are not permanently blocked"
    )


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
        "ReportTaskContext{this, bootstrap_token_, provisioning_code_}" in body
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
    deinit_idx = body.index("self->deinit();", ble_guard)
    reattempt_idx = body.index("self->TryReportProvisioningAuthenticated(reason);", ble_guard)
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
    not_connecting = body.index("if (!m_sta_is_connecting)", ble_guard)
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
# FW21: blufi.cpp — the Wi-Fi connect-SUCCESS lane must send the
#       ESP_BLUFI_STA_CONN_SUCCESS conn-report (with BSSID + SSID extra info)
#       BEFORE it disconnects the BLE link, and the BLE teardown +
#       authenticated-report must be deferred onto the Application task (never
#       executed inline in the wifi-connect FreeRTOS task).
# ---------------------------------------------------------------------------
def test_fw21_conn_success_report_carries_extra_info_and_precedes_ble_disconnect():
    req = _station_connect_helper_body()

    # Anchor on the success branch (wifi.IsConnected()).
    conn_idx = req.index("if (wifi.IsConnected())")
    fail_idx = req.index("} else {", conn_idx)
    success = req[conn_idx:fail_idx]

    # The success conn-report carries the resolved BSSID + SSID extra info.
    assert "info.sta_bssid_set = true;" in success
    assert "info.sta_ssid = self->m_sta_ssid;" in success
    assert "info.sta_ssid_len = self->m_sta_ssid_len;" in success

    report_idx = success.index(
        "esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS"
    )
    disconnect_idx = success.index("esp_blufi_disconnect();")
    assert report_idx < disconnect_idx, (
        "the SUCCESS conn-report must be sent to the phone BEFORE the BLE link is "
        "dropped, otherwise the phone never sees the success frame"
    )

    # The BLE teardown + authenticated report happen on the Application task.
    sched_idx = success.index("Application::GetInstance().Schedule(")
    assert report_idx < sched_idx
    sched_body = success[sched_idx:]
    assert "self->deinit();" in sched_body
    assert (
        'self->TryReportProvisioningAuthenticated("wifi_success_after_ble_teardown");'
        in sched_body
    )
    assert "SchedulePendingTbotClaimRefresh();" in sched_body
    # Teardown precedes the authenticated report inside the deferred lambda.
    assert sched_body.index("self->deinit();") < sched_body.index(
        "TryReportProvisioningAuthenticated"
    )


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
    conn_idx = req.index("if (wifi.IsConnected())")
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
    deinit_pos = sched_body.index("self->deinit();")
    assert deinit_pos < sched_body.index("SchedulePendingTbotClaimRefresh")


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

    # Token cap = 64, code cap = 16; the assign uses the clamped safe_len, never
    # the raw frame length.
    assert "uint8_t safe_len = (len > 64) ? 64 : len;" in custom
    assert "bootstrap_token_.assign(value, safe_len);" in custom
    assert "uint8_t safe_len = (len > 16) ? 16 : len;" in custom
    assert "provisioning_code_.assign(value, safe_len);" in custom

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
    assert 'ESP_LOGI(BLUFI_TAG, "Received bootstrap token (%u bytes)", safe_len);' in custom
    assert 'ESP_LOGI(BLUFI_TAG, "Received provisioning code (%u bytes)", safe_len);' in custom

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
    assert 'websocket_settings.SetString("bootstrap_token", bootstrap_token_);' in custom

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

    # Idempotency: only a fully completed transaction may return stale success.
    guard_idx = body.index("if (m_deinited && !host_active_ && !controller_active_) {")
    return_idx = body.index("return ESP_OK;", guard_idx)
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
    conn_idx = req.index("if (wifi.IsConnected())")
    else_idx = req.index("} else {", conn_idx)
    fail = req[else_idx:]

    # State-machine flags are cleared so a later GET_WIFI_STATUS does not report
    # a stale "connecting" / "connected".
    assert "self->m_sta_is_connecting = false;" in fail
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
    assert "else if (m_sta_is_connecting)" in status_body
    connecting_idx = status_body.index("else if (m_sta_is_connecting)")
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

    clear_def = blufi.index("void Blufi::ClearProvisioningSecrets()")
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
    assert 'websocket_settings.SetString("bootstrap_token", bootstrap_token_);' in blufi, (
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
    clear_def = blufi.index("void Blufi::ClearProvisioningSecrets()")
    clear_end = blufi.index("\n}", clear_def)
    assert clear_def < erase_idx < clear_end, (
        "the NVS erase must be scoped inside ClearProvisioningSecrets(), which is "
        "only called on report SUCCESS (FW6/FW10)"
    )

    # The two retain (failure) paths must NOT erase the NVS token: they log
    # retention and leave ClearProvisioningSecrets uncalled (FW7/FW10). Re-anchor
    # on the retain logs to prove the erase is not on these lanes.
    auth_fail_log = "Provisioning report failed after BLE teardown; secrets retained for retry"
    wifi_fail_log = "failed-status report failed; secrets retained for retry"
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
