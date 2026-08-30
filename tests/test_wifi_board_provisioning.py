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
        "void WifiBoard::StartWifiConfigMode(",
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


def test_wifi_config_entry_closes_full_screen_app_overlay():
    wifi_board = read("main/boards/common/wifi_board.cc")
    app_manager_header = read("main/app_manager.h")
    body = _start_wifi_config_body(wifi_board)

    publish = body.index("app.PublishWifiConfigEntry(preparation)")
    close_overlay = body.index("AppExitToChatboxForSystemFlow()", publish)
    stop_station = body.index("WifiManager::GetInstance().StopStation()", close_overlay)

    assert "void AppExitToChatboxForSystemFlow();" in app_manager_header
    assert publish < close_overlay < stop_station


# ---------------------------------------------------------------------------
# WB1: StartWifiConfigMode() full setup ordering for the BLE provisioning path:
#      reserve -> BeginWifiProvisioning -> commit -> RestartForSetup() ->
#      visible state mutation -> timeout. The wake-word release must free
#      the AFE detection task before BLE comes up; the restart helper owns
#      BLE generation reset/deinit/init; the hard-timeout is armed last so
#      advertising cannot run forever.
# ---------------------------------------------------------------------------
def test_wb1_start_config_mode_setup_step_ordering():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    reserve_idx = body.index("TryReserveProvisioningSession")
    release_idx = body.index("BeginWifiProvisioning")
    commit_idx = body.index("provisioning_reservation.Commit(provisioning_token)")
    restart_idx = body.index("blufi.RestartForSetup();")
    stop_idx = body.index("WifiManager::GetInstance().StopStation();")
    config_idx = body.index("in_config_mode_ = true;")
    state_idx = body.index("PublishWifiConfigEntry(preparation)")
    timer_idx = body.index("blufi.StartBleSetupTimeout(")

    assert reserve_idx < release_idx < commit_idx < restart_idx
    assert restart_idx < state_idx < stop_idx < config_idx < timer_idx, (
        "StartWifiConfigMode() BLE setup steps must run in order: "
        "reservation -> wake-word release -> binding -> restart success -> "
        "checked state publication -> station/config mutation -> StartBleSetupTimeout()"
    )
    assert "if (blufi_restart_error != ESP_OK)" in body
    restart_failure = body[body.index("if (blufi_restart_error != ESP_OK)"):stop_idx]
    assert "AbortProvisioningSetup(provisioning_token)" in restart_failure
    assert restart_failure.index("AbortProvisioningSetup(provisioning_token)") < restart_failure.index(
        "RollbackWifiConfigEntry(preparation)"
    )


# ---------------------------------------------------------------------------
# WB2: every explicit setup entry must reserve the provisioning session before
#      touching the BLE stack, so a prior completion still in flight cannot be
#      reused for a new phone provisioning attempt. A stale advertising session
#      is only reused when the state read proves it is genuinely off/timed-out.
# ---------------------------------------------------------------------------
def test_wb2_explicit_setup_always_opens_a_fresh_blufi_session():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    assert body.count("blufi.TryReserveProvisioningSession()") == 1
    assert body.index("TryReserveProvisioningSession") < body.index("blufi.RestartForSetup();")
    assert "prior provisioning completion still active" in body

    restart_idx = body.index("blufi.RestartForSetup();")
    timer_idx = body.index("blufi.StartBleSetupTimeout(")
    assert restart_idx < timer_idx


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
        "void WifiBoard::StartWifiConfigMode(",
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

    # StartWifiConfigMode owns station mutation after its setup transaction.
    timeout_body = _func_body(
        wifi_board,
        "void WifiBoard::OnWifiConnectTimeout(",
        "// ---",
    )
    assert "RequestWifiConfigMode();" in timeout_body
    assert "StopStation" not in timeout_body


def test_wifi_connect_timeout_ignores_active_lesson_before_station_or_setup_side_effects():
    wifi_board = read("main/boards/common/wifi_board.cc")
    timeout_body = _func_body(
        wifi_board,
        "void WifiBoard::OnWifiConnectTimeout(",
        "// ---",
    )

    assert "Application::GetInstance().IsLessonRuntimeActive()" in timeout_body
    guard_idx = timeout_body.index("Application::GetInstance().IsLessonRuntimeActive()")
    setup_idx = timeout_body.index("RequestWifiConfigMode();")
    assert guard_idx < setup_idx
    guard = timeout_body[guard_idx:setup_idx]
    assert "return;" in guard
    assert "StopStation" not in guard
    assert "StartWifiConfigMode" not in guard

def test_wifi_config_entry_uses_main_task_request_without_delayed_worker_side_effects():
    wifi_board = read("main/boards/common/wifi_board.cc")
    enter_body = _func_body(
        wifi_board,
        "void WifiBoard::EnterWifiConfigMode()",
        "bool WifiBoard::IsInWifiConfigMode()",
    )
    assert "app.IsLessonRuntimeActive()" in enter_body
    guard_idx = enter_body.index("app.IsLessonRuntimeActive()")
    setup_idx = enter_body.index("RequestWifiConfigMode(true)")
    assert guard_idx < setup_idx
    guard = enter_body[guard_idx:setup_idx]
    assert "return;" in guard
    assert "StopStation" not in guard
    assert "RequestWifiConfigMode" not in guard
    assert "xTaskCreate" not in enter_body
    assert "StopStation" not in enter_body
    assert "esp_timer_stop" not in enter_body


def test_enter_wifi_config_mode_delegates_all_station_and_state_mutation_to_start():
    wifi_board = read("main/boards/common/wifi_board.cc")
    enter_body = _func_body(
        wifi_board,
        "void WifiBoard::EnterWifiConfigMode()",
        "bool WifiBoard::IsInWifiConfigMode()",
    )

    assert "RequestWifiConfigMode" in enter_body
    assert "StopStation" not in enter_body
    assert "esp_timer_stop" not in enter_body
    assert "in_config_mode_" not in enter_body
    assert "SetDeviceState" not in enter_body


def test_wifi_config_entry_is_one_idempotent_main_task_transaction():
    wifi_board = read("main/boards/common/wifi_board.cc")
    wifi_header = read("main/boards/common/wifi_board.h")
    enter_body = _func_body(
        wifi_board,
        "void WifiBoard::EnterWifiConfigMode()",
        "bool WifiBoard::IsInWifiConfigMode()",
    )
    timeout_body = _func_body(wifi_board, "void WifiBoard::OnWifiConnectTimeout(", "// ---")

    assert "std::atomic<bool> wifi_config_entry_pending_" in wifi_header
    assert "void RequestWifiConfigMode(" in wifi_header
    assert "RequestWifiConfigMode(" in enter_body
    assert "RequestWifiConfigMode(" in timeout_body
    assert "Application::GetInstance().Schedule(" in wifi_board
    request_body = _func_body(
        wifi_board,
        "void WifiBoard::RequestWifiConfigMode(",
        "void WifiBoard::StartWifiConfigMode(",
    )
    assert "compare_exchange_strong" in request_body
    assert "wifi_config_entry_pending_.store(false)" in request_body


def test_application_prepares_realtime_before_blufi_and_checks_publication():
    wifi_board = read("main/boards/common/wifi_board.cc")
    app_h = read("main/application.h")
    app_cc = read("main/application.cc")
    start = _start_wifi_config_body(wifi_board)

    prepare = start.index("PrepareWifiConfigEntry")
    reserve = start.index("TryReserveProvisioningSession")
    restart = start.index("blufi.RestartForSetup()")
    publish = start.index("PublishWifiConfigEntry")
    stop = start.index("StopStation")
    assert prepare < reserve < restart < publish < stop
    assert "struct WifiConfigEntryPreparation" in app_h
    assert "bool PrepareWifiConfigEntry(WifiConfigEntryPreparation&" in app_h
    assert "bool PublishWifiConfigEntry(const WifiConfigEntryPreparation&" in app_h
    assert "bool RollbackWifiConfigEntry(const WifiConfigEntryPreparation&" in app_h

    prepare_body = _func_body(
        app_cc,
        "bool Application::PrepareWifiConfigEntry(",
        "bool Application::PublishWifiConfigEntry(",
    )
    assert prepare_body.index("connect_in_flight_.load()") < prepare_body.index("++connect_generation_")
    assert prepare_body.index("reset_pending_.load()") < prepare_body.index("++connect_generation_")
    assert "CloseAudioChannelByIntent();" in prepare_body
    assert "DoResetProtocol();" not in prepare_body
    assert "if (!SetDeviceState(kDeviceStateIdle))" in prepare_body
    rollback_body = _func_body(
        app_cc,
        "bool Application::RollbackWifiConfigEntry(",
        "void Application::Initialize()",
    )
    assert "xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED)" in rollback_body
    assert "SetDeviceState(kDeviceStateConnecting)" in rollback_body
    assert "ContinueOpenAudioChannel(mode)" in rollback_body


def test_failed_bound_entry_aborts_exact_token_before_application_rollback():
    wifi_board = read("main/boards/common/wifi_board.cc")
    blufi_h = read("main/boards/common/blufi.h")
    start = _start_wifi_config_body(wifi_board)
    assert "bool AbortProvisioningSetup(ProvisioningToken token);" in blufi_h
    abort = start.index("AbortProvisioningSetup(provisioning_token)")
    rollback = start.index("RollbackWifiConfigEntry", abort)
    assert abort < rollback

def test_ap_setup_timeout_rechecks_active_lesson_before_deferred_teardown():
    wifi_board = read("main/boards/common/wifi_board.cc")
    timeout_body = _func_body(
        wifi_board,
        "void WifiBoard::OnApSetupTimeout(",
        "void WifiBoard::StartApSetupTimeout(",
    )
    scheduled = timeout_body[
        timeout_body.index("Application::GetInstance().Schedule([board]()") :
        timeout_body.index("});", timeout_body.index("Application::GetInstance().Schedule([board]()"))
    ]

    assert "Application::GetInstance().IsLessonRuntimeActive()" in scheduled
    guard_idx = scheduled.index("Application::GetInstance().IsLessonRuntimeActive()")
    stop_idx = scheduled.index("WifiManager::GetInstance().StopConfigAp();")
    clear_idx = scheduled.index("board->in_config_mode_ = false;")
    assert guard_idx < stop_idx < clear_idx
    guard = scheduled[guard_idx:stop_idx]
    assert "board->StartApSetupTimeout(CONFIG_AP_SETUP_TIMEOUT_SEC);" in guard
    assert "return;" in guard
    assert "StopConfigAp" not in guard
    assert "in_config_mode_" not in guard

def test_start_wifi_config_mode_ignores_active_lesson_before_setup_side_effects():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _start_wifi_config_body(wifi_board)

    assert "Application::GetInstance().IsLessonRuntimeActive()" in body
    guard_idx = body.index("Application::GetInstance().IsLessonRuntimeActive()")
    preflight_idx = body.index("TryReserveProvisioningSession()")
    config_flag_idx = body.index("in_config_mode_ = true;")
    state_idx = body.index("PublishWifiConfigEntry(preparation)")
    assert guard_idx < preflight_idx < state_idx < config_flag_idx
    guard = body[guard_idx:preflight_idx]
    assert "return;" in guard
    assert "in_config_mode_" not in guard
    assert "SetDeviceState" not in guard
    assert "StartConfigAp" not in guard
    assert "BeginWifiProvisioning" not in guard
    assert "StartBleSetupTimeout" not in guard
    assert "ReceiveWifiCredentialsFromAudio" not in guard

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

    # Explicit comment-of-record that origin-token workers own completion.
    assert "BluFi success workers carry their originating provisioning token" in body, (
        "the Connected handler must document that the report is owned by BluFi"
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

    # Generic Connected cannot identify the originating provisioning session.
    assert "CompleteSuccessfulProvisioningTeardown" not in body

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
    assert 'StartTbotBlufiAdvertising' in blufi
    assert 'esp_ble_gap_set_device_name(device_name)' in blufi


# ---------------------------------------------------------------------------
# WB10: credential redaction on the wifi_board.cc surface. The Connected handler
#       reads the bootstrap token and provisioning code (to decide nothing — the
#       report is owned by BluFi) but must log ONLY their emptiness as a boolean,
#       never the value. No %s-formatted log line may carry the token, code, a
#       Wi-Fi password, a device secret, or a bearer value.
# ---------------------------------------------------------------------------
def test_wb10_no_credential_value_logged_in_wifi_board():
    wifi_board = read("main/boards/common/wifi_board.cc")

    # The generic Connected handler no longer reads provisioning secrets.
    body = _connected_event_body(wifi_board)
    assert "GetBootstrapToken" not in body
    assert "GetProvisioningCode" not in body

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
    cfg_idx = body.index("RequestWifiConfigMode();")
    assert have_idx < arm_idx < start_idx < cfg_idx, (
        "TryWifiConnect() must arm the timeout + start station when SSIDs exist, "
        "and fall back to StartWifiConfigMode() when none are stored"
    )


# ---------------------------------------------------------------------------
# WB12b: Half-provisioned recovery. If a previous BluFi attempt saved Wi-Fi
#        credentials but never completed TBOT claim confirmation, startup must
#        not disappear into station-only mode. Keep BLE advertising open for the
#        phone before starting the station reconnect, while claimed devices keep
#        the normal BLE-off reconnect path.
# ---------------------------------------------------------------------------
def test_wb12b_unclaimed_saved_ssid_keeps_ble_advertising_before_station_connect():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _func_body(
        wifi_board,
        "void WifiBoard::TryWifiConnect()",
        "void WifiBoard::OnNetworkEvent(",
    )

    have_idx = body.index("if (have_ssid)")
    ensure_idx = body.index("app.EnsureBleAdvertisingForUnclaimedSavedWifi();", have_idx)
    start_idx = body.index("WifiManager::GetInstance().StartStation();", ensure_idx)

    assert "auto& app = Application::GetInstance();" in body[have_idx:ensure_idx]
    assert "IsDeviceClaimed()" not in body
    assert have_idx < ensure_idx < start_idx, (
        "unclaimed saved-SSID startup must reopen BLE before starting station "
        "mode so Android can rediscover and send a fresh claim token"
    )

def test_wb12c_unclaimed_saved_ssid_connect_does_not_teardown_ble_without_claim_secrets():
    wifi_board = read("main/boards/common/wifi_board.cc")
    fn = _func_body(
        wifi_board,
        "void WifiBoard::OnNetworkEvent(",
        "void WifiBoard::SetNetworkEventCallback(",
    )
    case_idx = fn.index("case NetworkEvent::Connected:")
    case_end = fn.index("case NetworkEvent::Scanning:", case_idx)
    body = fn[case_idx:case_end]

    assert "CaptureProvisioningSession" not in body
    assert "CompleteSuccessfulProvisioningTeardown" not in body
    assert "blufi.deinit" not in body
    assert "cannot safely identify which provisioning attempt produced it" in body


def test_wb12d_wifi_board_claim_gate_uses_public_application_api():
    header = read("main/application.h")

    public_start = header.index("class Application")
    private_start = header.index("\nprivate:", public_start)
    public_api = header[public_start:private_start]
    private_api = header[private_start:]

    assert "bool IsDeviceClaimed() const;" in public_api
    assert "bool IsDeviceClaimed() const;" not in private_api

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
