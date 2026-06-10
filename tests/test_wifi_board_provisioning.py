"""Source-level regression tests locking US-005 wifi_board.cc provisioning invariants.

These are static assertions over the firmware .cc/.h text (no device needed),
following the convention in tests/test_wifi_provisioning_brand.py and
tests/test_blufi_provisioning_stability.py: module-level ROOT, a read(path)
helper, and test_* functions asserting on substrings / regex / relative
ordering of real markers in the source.

Scope: main/boards/common/wifi_board.cc — the Wi-Fi board provisioning seam.
The companion suite test_blufi_provisioning_stability.py (FW1..FW14) audits
blufi.cpp internals; THIS suite audits the wifi_board.cc side of the same
US-005 contract: the explicit-setup config-mode ordering, the kOff||kTimeout
re-entry guard SHAPE (init() is gated, never unconditional), the
GetDeviceStatusJson ble_state/ap_state observability surface, the 60s station
connect timeout, the TBot/TBOT brand, the report-ownership / BLE-off ordering on
NetworkEvent::Connected, the AP/BLE hard-timeout cancel-before-teardown gates,
and the wifi_board.cc credential-redaction surface (token/code presence is
logged only as a boolean, never the value).

Each test is tagged with its invariant id (WB1..WBn) so a failure is traceable.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _func_body(src: str, signature: str, next_signature: str) -> str:
    """Return the text of `signature`'s definition up to `next_signature`."""
    start = src.index(signature)
    end = src.index(next_signature, start + len(signature))
    return src[start:end]


def _start_wifi_config_body(wifi_board: str) -> str:
    """Body of WifiBoard::StartWifiConfigMode() up to EnterWifiConfigMode()."""
    return _func_body(
        wifi_board,
        "void WifiBoard::StartWifiConfigMode()",
        "void WifiBoard::EnterWifiConfigMode()",
    )


def _device_status_body(wifi_board: str) -> str:
    """Body of WifiBoard::GetDeviceStatusJson() to end of file."""
    start = wifi_board.index("std::string WifiBoard::GetDeviceStatusJson()")
    return wifi_board[start:]


def _connected_event_body(wifi_board: str) -> str:
    """The NetworkEvent::Connected case body inside OnNetworkEvent()."""
    fn = _func_body(
        wifi_board,
        "void WifiBoard::OnNetworkEvent(",
        "void WifiBoard::SetNetworkEventCallback(",
    )
    case_idx = fn.index("case NetworkEvent::Connected:")
    next_case = fn.index("case NetworkEvent::Scanning:", case_idx)
    return fn[case_idx:next_case]


# ---------------------------------------------------------------------------
# WB1: StartWifiConfigMode() full setup ordering for the BLE provisioning path:
#      ReleaseWakeWordResourcesForWifiConfig -> GetBleState() ->
#      blufi.init() -> StartBleSetupTimeout(). The wake-word release must free
#      the AFE detection task before BLE comes up; the state read gates the
#      conditional init; the hard-timeout is armed last so advertising cannot
#      run forever.
# ---------------------------------------------------------------------------
def test_wb1_start_config_mode_setup_step_ordering():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    release_idx = body.index("ReleaseWakeWordResourcesForWifiConfig")
    get_state_idx = body.index("blufi.GetBleState();")
    init_idx = body.index("blufi.init();")
    timer_idx = body.index("blufi.StartBleSetupTimeout(")

    assert release_idx < get_state_idx < init_idx < timer_idx, (
        "StartWifiConfigMode() BLE setup steps must run in order: "
        "wake-word release -> GetBleState() -> init() -> StartBleSetupTimeout()"
    )


# ---------------------------------------------------------------------------
# WB2: the kOff||kTimeout re-entry GUARD shape. init() must be CONDITIONAL on
#      the BLE state being off-or-timed-out — never unconditional. If init()
#      were called every time, re-entering config mode while BLE is already
#      advertising (claimable-standby) would double-init and leak the
#      controller/host. The guard must wrap init(); the timeout re-arm must
#      stay OUTSIDE the guard (re-armed unconditionally on every entry).
# ---------------------------------------------------------------------------
def test_wb2_init_is_gated_by_off_or_timeout_guard_not_unconditional():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    # The guard reads the state into a local and tests kOff || kTimeout.
    assert re.search(
        r"if\s*\(\s*ble_state\s*==\s*Blufi::BleState::kOff\s*\|\|\s*"
        r"ble_state\s*==\s*Blufi::BleState::kTimeout\s*\)",
        body,
    ), "init() must be gated by an `if (ble_state == kOff || ble_state == kTimeout)` guard"

    # init() must sit INSIDE that guard block: the `{` opening the guard body
    # must precede blufi.init(), and the guard's closing `}` must follow it.
    guard_match = re.search(
        r"if\s*\(\s*ble_state\s*==\s*Blufi::BleState::kOff\s*\|\|\s*"
        r"ble_state\s*==\s*Blufi::BleState::kTimeout\s*\)\s*\{",
        body,
    )
    guard_open = guard_match.end()
    guard_close = body.index("}", guard_open)
    init_idx = body.index("blufi.init();")
    assert guard_open < init_idx < guard_close, (
        "blufi.init() must live inside the kOff||kTimeout guard block, not "
        "be called unconditionally on every config-mode entry"
    )

    # Negative: init() must NOT be reachable as a bare unconditional statement.
    # The only init() call must be the guarded one. (Exactly one init call.)
    assert body.count("blufi.init();") == 1

    # The hard-timeout re-arm must NOT be trapped inside the guard: it is armed
    # on EVERY explicit setup entry (even when BLE was already up) so the
    # timeout window is reset. It must therefore come AFTER the guard closes.
    timer_idx = body.index("blufi.StartBleSetupTimeout(")
    assert timer_idx > guard_close, (
        "StartBleSetupTimeout() must be armed outside the init guard so the "
        "BLE hard-timeout is reset on every explicit setup entry"
    )


# ---------------------------------------------------------------------------
# WB3: GetDeviceStatusJson() exposes both ble_state and ap_state. These are the
#      backend observability surface for US-005: the status poll reads ble_state
#      to know whether BLE is still advertising and ap_state for the SoftAP.
#      ble_state must be the live string from Blufi (under the BLUFI build) and
#      fall back to "off" when BLUFI is compiled out. ap_state comes from
#      GetApStateString().
# ---------------------------------------------------------------------------
def test_wb3_device_status_json_exposes_ble_and_ap_state():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _device_status_body(wifi_board)

    # ble_state key is added from the live Blufi state string under BLUFI.
    assert 'cJSON_AddStringToObject(root, "ble_state", Blufi::GetInstance().GetBleStateString())' in body, (
        "GetDeviceStatusJson() must expose ble_state from the live Blufi state"
    )

    # A non-BLUFI build still reports the key as "off" (key must always exist so
    # the backend status shape is stable regardless of provisioning transport).
    assert 'cJSON_AddStringToObject(root, "ble_state", "off")' in body, (
        "non-BLUFI build must still emit ble_state:\"off\" so the key is stable"
    )

    # The two emissions must be under #ifdef/#else on the BLUFI config so the
    # live value is used when available and "off" only as the compiled-out
    # fallback (not both).
    assert "#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING" in body
    assert "#else" in body[body.index("#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING"):]

    # ap_state key is added from GetApStateString().
    assert 'cJSON_AddStringToObject(root, "ap_state", GetApStateString())' in body, (
        "GetDeviceStatusJson() must expose ap_state from GetApStateString()"
    )


# ---------------------------------------------------------------------------
# WB4: GetApStateString() reports "active" ONLY when the board believes it is in
#      config mode AND the wifi manager confirms config mode; otherwise "off".
#      The timeout teardown drops the AP, so the backend-safe state is "off"
#      after a timeout — never a stale "active".
# ---------------------------------------------------------------------------
def test_wb4_ap_state_string_active_requires_both_flags_else_off():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _func_body(
        wifi_board,
        "const char* WifiBoard::GetApStateString()",
        "void WifiBoard::StartWifiConfigMode()",
    )

    # "active" is gated on BOTH the board flag AND the manager's IsConfigMode().
    assert re.search(
        r"if\s*\(\s*in_config_mode_\s*&&\s*WifiManager::GetInstance\(\)\.IsConfigMode\(\)\s*\)",
        body,
    ), "GetApStateString() must require both in_config_mode_ AND manager IsConfigMode() for active"

    active_idx = body.index('return "active";')
    off_idx = body.index('return "off";')
    # The default fall-through is "off" (after the active branch).
    assert active_idx < off_idx, "the non-active fall-through must return \"off\""


# ---------------------------------------------------------------------------
# WB5: the station connect timeout in wifi_board.cc is 60 seconds. This is the
#      Wi-Fi connect budget after credentials are applied; the timer fires
#      OnWifiConnectTimeout which re-enters config mode. (Distinct from blufi's
#      kConnectTimeoutMs poll bound, which the FW3 test covers.)
# ---------------------------------------------------------------------------
def test_wb5_station_connect_timeout_is_60_seconds():
    wifi_board = read("main/boards/common/wifi_board.cc")

    match = re.search(
        r"constexpr\s+int\s+CONNECT_TIMEOUT_SEC\s*=\s*(\d+)\s*;", wifi_board
    )
    assert match is not None, "CONNECT_TIMEOUT_SEC declaration not found"
    assert int(match.group(1)) == 60, "Wi-Fi connect timeout must be 60s per US-005"

    # The connect timer must actually be armed with this constant (seconds->us).
    assert "esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL)" in wifi_board, (
        "the connect timer must be armed with CONNECT_TIMEOUT_SEC (seconds, in us)"
    )

    # On timeout the station is stopped and config mode is (re)entered.
    timeout_body = _func_body(
        wifi_board,
        "void WifiBoard::OnWifiConnectTimeout(",
        "// ---",
    )
    stop_idx = timeout_body.index("WifiManager::GetInstance().StopStation();")
    reenter_idx = timeout_body.index("StartWifiConfigMode();")
    assert stop_idx < reenter_idx, (
        "connect-timeout must stop the station before re-entering config mode"
    )


# ---------------------------------------------------------------------------
# WB6: NetworkEvent::Connected must NOT POST the authenticated/provisioning
#      report itself — that is owned by the BluFi success branch (single owner)
#      to avoid a duplicate POST + reconnect race. wifi_board.cc may read the
#      token/code only to LOG their emptiness; it must not call Report() here.
#      device_authenticated is therefore reported only after BLE is torn down by
#      the success branch.
# ---------------------------------------------------------------------------
def test_wb6_connected_event_does_not_post_report_blufi_owns_it():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _connected_event_body(wifi_board)

    # Explicit comment-of-record that the report is owned elsewhere.
    assert "report owned by BluFi success branch" in body, (
        "the Connected handler must document that the report is owned by BluFi"
    )
    assert "Do not call Report() here" in body, (
        "the Connected handler must document that it does not own the report"
    )

    # Hard guard: no ProvisioningStatusReporter::Report( call in this handler.
    assert "ProvisioningStatusReporter::Report(" not in body, (
        "NetworkEvent::Connected must NOT POST the provisioning report; the "
        "BluFi success branch is the single owner"
    )
    assert ".Report(" not in body, (
        "NetworkEvent::Connected must not invoke any Report() — single-owner rule"
    )


# ---------------------------------------------------------------------------
# WB7: NetworkEvent::Connected tears BLE down only AFTER cancelling the BLE
#      hard-timeout, so a stale timer callback cannot post a redundant teardown
#      after deinit(). Likewise the AP hard-timeout is cancelled before moving
#      on. This is the wifi_board.cc cancel-before-teardown gate.
# ---------------------------------------------------------------------------
def test_wb7_connected_cancels_timeouts_before_ble_teardown():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _connected_event_body(wifi_board)

    # AP hard-timeout cancelled on connect (provisioning succeeded).
    assert "CancelApSetupTimeout();" in body, (
        "NetworkEvent::Connected must cancel the AP setup hard-timeout"
    )

    # BLE hard-timeout cancelled BEFORE deinit() so the timer cannot re-post a
    # teardown after BLE resources are released.
    assert "blufi.CancelBleSetupTimeout();" in body
    assert "blufi.deinit();" in body
    cancel_idx = body.index("blufi.CancelBleSetupTimeout();")
    deinit_idx = body.index("blufi.deinit();")
    assert cancel_idx < deinit_idx, (
        "CancelBleSetupTimeout() must precede deinit() so a stale BLE timer "
        "cannot post a redundant teardown after deinit()"
    )

    # Connecting/idle bookkeeping: in_config_mode_ is cleared after teardown.
    assert "in_config_mode_ = false;" in body


# ---------------------------------------------------------------------------
# WB8: StartWifiConfigMode() arms the AP-setup hard-timeout in the SoftAP
#      (Hotspot) path so the AP cannot run forever. The arm must come AFTER
#      StartConfigAp() opens the SoftAP (you cannot time out an AP you have not
#      opened). Mirrors the BLE gate.
# ---------------------------------------------------------------------------
def test_wb8_hotspot_path_arms_ap_setup_timeout_after_opening_ap():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    # The hotspot branch opens the AP then arms the hard-timeout.
    assert "wifi_manager.StartConfigAp();" in body
    assert "StartApSetupTimeout(CONFIG_AP_SETUP_TIMEOUT_SEC);" in body
    open_idx = body.index("wifi_manager.StartConfigAp();")
    arm_idx = body.index("StartApSetupTimeout(CONFIG_AP_SETUP_TIMEOUT_SEC);")
    assert open_idx < arm_idx, (
        "AP-setup hard-timeout must be armed AFTER StartConfigAp() opens the AP"
    )


# ---------------------------------------------------------------------------
# WB9: TBot brand on the station SSID prefix and NO Xiaozhi branding anywhere in
#      wifi_board.cc. The BLE advertised name lives in blufi.cpp; cross-check
#      that the documented TBOT-<MAC> prefix is present and no Xiaozhi-Blufi
#      name leaks through. (The brand suite asserts the same prefix from a
#      complementary angle; here we lock it as a wifi_board provisioning-seam
#      regression and add the no-Xiaozhi sweep on wifi_board.cc itself.)
# ---------------------------------------------------------------------------
def test_wb9_tbot_brand_no_xiaozhi_in_wifi_board():
    wifi_board = read("main/boards/common/wifi_board.cc")

    # Station SSID prefix is TBot (config mode AP / station naming).
    assert 'config.ssid_prefix = "TBot";' in wifi_board
    assert 'config.ssid_prefix = "Xiaozhi";' not in wifi_board

    # No Xiaozhi / 小智 branding leaks through the Wi-Fi board.
    for forbidden in ("Xiaozhi", "XiaoZhi", "小智"):
        assert forbidden not in wifi_board, (
            f"wifi_board.cc must not contain forbidden brand text {forbidden!r}"
        )


def test_wb9b_ble_advertised_name_is_tbot_prefixed():
    blufi = read("main/boards/common/blufi.cpp")

    # The MAC fallback advertises the documented TBOT-<MAC> name.
    assert "TBOT-%02X%02X%02X%02X%02X%02X" in blufi, (
        "BLE advertised name must use the documented TBOT-<MAC> format"
    )
    # The eFuse-serial branch must also be TBOT-prefixed (never a bare serial).
    assert 'std::string("TBOT-") + serial' in blufi, (
        "the serial branch must apply the TBOT- prefix so a serial-provisioned "
        "unit is still discoverable by the mobile allowlist"
    )
    # No Xiaozhi-Blufi device name leaks through.
    assert '#define BLUFI_DEVICE_NAME "Xiaozhi-Blufi"' not in blufi
    assert 'esp_ble_gap_set_device_name(device_name.c_str())' in blufi


# ---------------------------------------------------------------------------
# WB10: credential redaction on the wifi_board.cc surface. The Connected handler
#       reads the bootstrap token and provisioning code (to decide nothing — the
#       report is owned by BluFi) but must log ONLY their emptiness as a boolean,
#       never the value. No %s-formatted log line may carry the token, code, a
#       Wi-Fi password, a device secret, or a bearer value.
# ---------------------------------------------------------------------------
def test_wb10_no_credential_value_logged_in_wifi_board():
    wifi_board = read("main/boards/common/wifi_board.cc")

    # The Connected handler logs presence as a boolean only.
    body = _connected_event_body(wifi_board)
    assert "token_empty=%d" in body and "code_empty=%d" in body, (
        "the Connected handler must log token/code presence as a boolean, not "
        "the value"
    )
    assert "(int)token.empty()" in body
    assert "(int)code.empty()" in body

    # No ESP_LOG statement in the whole file may format the raw token/code value
    # (.c_str() of the secret) into the message. Collapse multi-line ESP_LOG
    # calls so a value sitting on a wrapped continuation line is still caught.
    for stmt in re.findall(r"ESP_LOG\w*\((?:[^;]*?)\);", wifi_board, re.DOTALL):
        assert "token.c_str()" not in stmt, "token value must never be logged"
        assert "code.c_str()" not in stmt, "provisioning code value must never be logged"
        assert "GetBootstrapToken().c_str()" not in stmt
        assert "GetProvisioningCode().c_str()" not in stmt

    # Defense-in-depth: no log line mentioning a credential keyword may carry a
    # %s value formatter (a presence/empty boolean uses %d, never %s).
    for line in wifi_board.splitlines():
        low = line.lower()
        if "esp_log" in low and "%s" in line:
            assert "token" not in low, "no %s log line may reference the token"
            assert "passwd" not in low and "password" not in low, (
                "no %s log line may reference a Wi-Fi password"
            )
            assert "secret" not in low, "no %s log line may reference a device secret"
            assert "bearer" not in low and "authorization" not in low, (
                "no %s log line may reference a bearer/authorization value"
            )
        # The provisioning code uses the variable name `code`; guard the %s case
        # narrowly so we do not trip on Lang::CODE (a language code constant).
        if "esp_log" in low and "%s" in line and "code.c_str()" in line:
            raise AssertionError("provisioning code value must never be logged with %s")


# ---------------------------------------------------------------------------
# WB11: StartNetwork() must NOT auto-connect when config mode is already active
#       (a BOOT-triggered re-provisioning during startup). It must short-circuit
#       on in_config_mode_ BEFORE calling TryWifiConnect(), otherwise a startup
#       auto-connect would race the explicit setup window the user just opened.
# ---------------------------------------------------------------------------
def test_wb11_start_network_short_circuits_when_already_in_config_mode():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _func_body(
        wifi_board,
        "void WifiBoard::StartNetwork()",
        "void WifiBoard::TryWifiConnect()",
    )

    guard_idx = body.index("if (in_config_mode_)")
    try_connect_idx = body.index("TryWifiConnect();")
    assert guard_idx < try_connect_idx, (
        "StartNetwork() must check in_config_mode_ before TryWifiConnect()"
    )

    # The guard body returns early (skips auto-connect).
    guard_region = body[guard_idx:try_connect_idx]
    assert "return;" in guard_region, (
        "the in_config_mode_ guard must return early, skipping auto-connect"
    )
    assert "skipped auto-connect because config mode is already active" in guard_region


# ---------------------------------------------------------------------------
# WB12: TryWifiConnect() level-triggered branch: with stored SSIDs it arms the
#       connect timeout and starts the station; with NONE it falls into config
#       mode. This is the retry-safe entry that OnNetworkEvent reuses on every
#       config-mode exit (out-of-order credential delivery converges here).
# ---------------------------------------------------------------------------
def test_wb12_try_wifi_connect_branches_on_stored_ssids():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _func_body(
        wifi_board,
        "void WifiBoard::TryWifiConnect()",
        "void WifiBoard::OnNetworkEvent(",
    )

    assert "bool have_ssid = !ssid_manager.GetSsidList().empty();" in body
    have_idx = body.index("if (have_ssid)")
    # Connect lane: arm timeout + start station.
    arm_idx = body.index("esp_timer_start_once(connect_timer_,")
    start_idx = body.index("WifiManager::GetInstance().StartStation();")
    # Fallback lane: enter config mode.
    cfg_idx = body.index("StartWifiConfigMode();")
    assert have_idx < arm_idx < start_idx < cfg_idx, (
        "TryWifiConnect() must arm the timeout + start station when SSIDs exist, "
        "and fall back to StartWifiConfigMode() when none are stored"
    )


# ---------------------------------------------------------------------------
# WB13: WifiConfigModeExit re-attempts the connection with the new credentials
#       via TryWifiConnect() and cancels the AP hard-timeout first. This is the
#       level-triggered convergence: credentials arriving (config-mode exit)
#       always funnels back through the single connect entry, so out-of-order
#       wifi/token/code/BLE-off delivery still retries cleanly.
# ---------------------------------------------------------------------------
def test_wb13_config_mode_exit_reattempts_connect_after_cancel():
    wifi_board = read("main/boards/common/wifi_board.cc")
    fn = _func_body(
        wifi_board,
        "void WifiBoard::OnNetworkEvent(",
        "void WifiBoard::SetNetworkEventCallback(",
    )
    case_idx = fn.index("case NetworkEvent::WifiConfigModeExit:")
    case_end = fn.index("default:", case_idx)
    body = fn[case_idx:case_end]

    assert "in_config_mode_ = false;" in body
    cancel_idx = body.index("CancelApSetupTimeout();")
    retry_idx = body.index("TryWifiConnect();")
    assert cancel_idx < retry_idx, (
        "config-mode exit must cancel the AP hard-timeout before re-attempting "
        "the connection through the single TryWifiConnect() entry"
    )
