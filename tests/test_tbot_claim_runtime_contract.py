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


def test_application_polls_pending_claim_after_activation():
    header = read("main/application.h")
    source = read("main/application.cc")
    activation_done = function_body(source, "void Application::HandleActivationDoneEvent")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")

    assert '#include "claim_confirmation_reporter.h"' in header
    assert "PendingTbotClaim pending_tbot_claim_" in header
    assert "std::string pending_tbot_claim_api_url_" in header
    assert "std::string pending_tbot_claim_token_" in header
    assert "RefreshPendingTbotClaim();" in activation_done
    assert 'Settings backend_settings("backend", false);' in refresh_body
    assert 'backend_settings.GetString("api_url")' in refresh_body
    assert "FetchPendingTbotClaimFromDeviceConfig" in refresh_body


def test_activation_done_does_not_leave_wifi_config_mode_after_boot_reprovisioning():
    source = read("main/application.cc")
    activation_done = function_body(source, "void Application::HandleActivationDoneEvent")

    assert "GetDeviceState() == kDeviceStateWifiConfiguring" in activation_done
    assert "Activation done ignored because WiFi config mode is active" in activation_done
    assert activation_done.index("kDeviceStateWifiConfiguring") < activation_done.index("SetDeviceState(kDeviceStateIdle)")


def test_application_button_confirms_pending_claim_before_chat_toggle():
    source = read("main/application.cc")
    toggle_body = function_body(source, "void Application::HandleToggleChatEvent")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")

    assert "ConfirmPendingTbotClaim()" in toggle_body
    assert "ClaimConfirmationReporter::Confirm(pending_tbot_claim_" in confirm_body
    assert "pending_tbot_claim_api_url_" in confirm_body
    assert "pending_tbot_claim_token_" in confirm_body
    assert "CloseAudioChannelByIntent();" in confirm_body
    assert "pending_tbot_claim_ = PendingTbotClaim{}" in confirm_body

    close_body = function_body(source, "void Application::CloseAudioChannelByIntent")
    assert "online_intent_.store(false)" in close_body
    assert "protocol_->CloseAudioChannel" in close_body

def test_blufi_persists_claim_bootstrap_token_without_fetching_while_ble_is_active():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    custom_data_body = function_body(source, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")

    assert 'Settings websocket_settings("websocket", true);' in custom_data_body
    assert 'websocket_settings.SetString("bootstrap_token", bootstrap_token_);' in custom_data_body
    token_branch = custom_data_body[custom_data_body.index("if (tag == 0x01)") : custom_data_body.index("} else if (tag == 0x02)")]
    assert "WifiManager::GetInstance().IsConnected()" in token_branch
    assert "ScheduleClaimRefreshAfterTokenHandoff();" in token_branch
    assert "self->deinit();" not in token_branch
    assert "SchedulePendingTbotClaimRefresh();" not in token_branch
    assert "void ScheduleClaimRefreshAfterTokenHandoff();" in header
    assert "tag == 0x01" in custom_data_body


def test_blufi_defers_connected_wifi_claim_refresh_until_after_possible_wifi_frames():
    source = read("main/boards/common/blufi.cpp")
    helper_body = function_body(source, "void Blufi::ScheduleClaimRefreshAfterTokenHandoff")

    assert "kClaimRefreshAfterTokenHandoffDelayMs" in source
    assert "vTaskDelay(pdMS_TO_TICKS(kClaimRefreshAfterTokenHandoffDelayMs));" in helper_body
    assert "m_sta_is_connecting" in helper_body
    connecting_branch = helper_body[helper_body.index("m_sta_is_connecting"):]
    assert "return;" in connecting_branch[:connecting_branch.index("CancelBleSetupTimeout();")]
    assert "CancelBleSetupTimeout();" in helper_body
    assert "deinit();" in helper_body
    assert "SchedulePendingTbotClaimRefresh();" in helper_body

def test_blufi_waits_for_full_wifi_board_connect_timeout_before_reporting_failure():
    source = read("main/boards/common/blufi.cpp")
    branch_start = source.index("BLUFI request wifi connect to AP via esp-wifi-connect")
    failure_log_idx = source.index("Failed to connect to WiFi via esp-wifi-connect", branch_start)
    connect_body = source[branch_start:failure_log_idx]

    assert "constexpr int kConnectTimeoutMs = 60000;" in connect_body

def test_application_exposes_scheduled_pending_claim_refresh_for_late_ble_token():
    header = read("main/application.h")
    source = read("main/application.cc")
    schedule_body = function_body(source, "void Application::SchedulePendingTbotClaimRefresh")

    assert "void SchedulePendingTbotClaimRefresh();" in header
    assert "Schedule([this]()" in schedule_body
    assert "RefreshPendingTbotClaim();" in schedule_body


def test_application_runs_a_bounded_claim_poll_not_a_single_shot():
    # C4: the one-shot /device/config fetch becomes a BOUNDED poll (cadence +
    # 5-minute window cap), with an "if unowned -> claimable standby" trigger.
    header = read("main/application.h")
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    start_body = function_body(source, "void Application::StartClaimPoll")
    tick_body = function_body(source, "void Application::PollPendingTbotClaimTick")

    # Periodic timer + window cap constants exist (no tight loop).
    assert "esp_timer_handle_t claim_poll_timer_" in header
    assert "kClaimPollIntervalUs" in source
    assert "kClaimPollWindowMs" in source
    assert "esp_timer_start_periodic(claim_poll_timer_, kClaimPollIntervalUs)" in start_body

    # Unowned -> claimable standby trigger lives in the refresh path.
    assert "TbotClaimSubstate::AvailableStandby" in refresh_body
    assert "StartClaimPoll();" in refresh_body

    # The window cap stops the poll and surfaces a timeout (no infinite poll).
    assert "claim_poll_started_ms_" in tick_body
    assert "kClaimPollWindowMs" in tick_body
    assert "HandleClaimConfirmTimeout();" in tick_body

def test_unclaimed_standby_poll_window_does_not_expire_ble_advertising():
    # The 5-minute cap belongs to an active backend claim-confirm window. A robot
    # that is merely unclaimed/available must keep polling + BLE advertising so a
    # late phone scan can still find it; otherwise the screen falls to "Setup
    # expired" and the robot becomes unpairable without a reboot.
    source = read("main/application.cc")
    tick_body = function_body(source, "void Application::PollPendingTbotClaimTick")

    elapsed_idx = tick_body.index("now_ms - claim_poll_started_ms_ >= kClaimPollWindowMs")
    timeout_idx = tick_body.index("HandleClaimConfirmTimeout();", elapsed_idx)
    elapsed_branch = tick_body[elapsed_idx:timeout_idx]

    assert "pending_tbot_claim_.active" in elapsed_branch
    assert "claim_poll_started_ms_ = now_ms;" in elapsed_branch
    assert "RefreshPendingTbotClaim();" in elapsed_branch

def test_claimed_online_keeps_audio_stable_by_not_advertising_ble_until_boot_config():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    ensure_body = function_body(source, "void Application::EnsureBleAdvertisingForStandby")
    stop_ble_body = function_body(source, "void Application::StopBleAdvertising")

    claimed_online_idx = refresh_body.index("online_intent_.load() && IsDeviceClaimed()")
    claimed_online_branch = refresh_body[claimed_online_idx:refresh_body.index("return;", claimed_online_idx)]

    assert "StopClaimPoll();" in claimed_online_branch
    assert "StopBleAdvertising();" in claimed_online_branch
    assert "EnsureBleAdvertisingForStandby();" not in claimed_online_branch
    assert "if (IsDeviceClaimed())" in ensure_body
    assert "StopBleAdvertising();" in ensure_body[:ensure_body.index("if (blufi.GetBleState()")]
    assert "CancelBleSetupTimeout();" in stop_ble_body
    assert "StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);" in ensure_body


def test_application_enforces_local_claim_expiry_against_a_clock():
    # C4: expires_at must be compared to a clock and drive CLAIM_CONFIRM_TIMEOUT.
    source = read("main/application.cc")
    arm_body = function_body(source, "void Application::ArmClaimExpiryTimer")
    timeout_body = function_body(source, "void Application::HandleClaimConfirmTimeout")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")

    assert "ParseIso8601UtcToEpoch(pending_tbot_claim_.expires_at" in arm_body
    assert "time(nullptr)" in arm_body
    assert "esp_timer_start_once(claim_expiry_timer_" in arm_body
    # Tapping an already-expired window must not blind-confirm.
    assert "IsPendingTbotClaimExpired(pending_tbot_claim_, time(nullptr))" in confirm_body
    # Timeout renders the spec copy "Setup expired".
    assert "TbotClaimSubstate::ConfirmTimeout" in timeout_body
    assert "Lang::Strings::SETUP_EXPIRED" in timeout_body

def test_blufi_token_only_claim_path_does_not_report_device_authenticated_before_button_press():
    source = read("main/boards/common/blufi.cpp")
    success_start = source.index("if (wifi.IsConnected())")
    success_end = source.index("Failed to connect to WiFi via esp-wifi-connect", success_start)
    success_body = source[success_start:success_end]
    helper_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")
    skip_idx = helper_body.index("token_empty || code_empty")
    report_idx = helper_body.index("ProvisioningStatusReporter::Status::DeviceAuthenticated")

    assert "ProvisioningStatusReporter::Status::DeviceAuthenticated" not in success_body
    assert "TryReportProvisioningAuthenticated(\"wifi_success_after_ble_teardown\");" in success_body
    assert skip_idx < report_idx
    assert "Reporting provisioning authenticated skipped" in helper_body
    assert "ClearProvisioningSecrets();" in helper_body

def test_blufi_wifi_success_tears_down_ble_before_claim_refresh():
    # Real ESP32-S3 hardware can connect TCP but fail mbedTLS AES allocation if
    # the claim HTTPS poll runs while BluFi/BLE is still active. After the phone
    # has received the Wi-Fi success report, tear BLE down before refreshing the
    # pending claim.
    source = read("main/boards/common/blufi.cpp")
    branch_start = source.index("if (wifi.IsConnected())")
    report_idx = source.index("esp_blufi_send_wifi_conn_report", branch_start)
    connected_log_idx = source.index('"connected to WiFi"', report_idx)
    failure_log_idx = source.index("Failed to connect to WiFi via esp-wifi-connect", connected_log_idx)
    success_body = source[branch_start:failure_log_idx]

    assert "esp_blufi_send_wifi_conn_report" in success_body
    assert "esp_blufi_disconnect();" in success_body
    assert "CancelBleSetupTimeout();" in success_body
    assert '"WiFi provisioned; stopping BLE before claim refresh"' in success_body
    assert "self->deinit();" in success_body
    assert "SchedulePendingTbotClaimRefresh();" in success_body
    assert report_idx < connected_log_idx < source.index("self->deinit();", connected_log_idx)
    assert source.index("self->deinit();", connected_log_idx) < source.index(
        "SchedulePendingTbotClaimRefresh();", connected_log_idx
    )

def test_blufi_reports_device_authenticated_only_after_ble_teardown():
    # ESP32-S3 cannot reliably run provisioning HTTPS while the BluFi/BLE stack
    # is still up; BLE+TLS can fail internal AES allocation and leave mobile
    # polling forever for the robot's device_authenticated callback.
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    success_start = source.index("if (wifi.IsConnected())")
    report_idx = source.index("esp_blufi_send_wifi_conn_report", success_start)
    failure_idx = source.index("Failed to connect to WiFi via esp-wifi-connect", report_idx)
    success_body = source[success_start:failure_idx]
    helper_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")
    custom_data_body = function_body(source, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")
    report_rel = success_body.index("esp_blufi_send_wifi_conn_report")

    assert "void TryReportProvisioningAuthenticated(const char* reason);" in header
    assert "TryReportProvisioningAuthenticated(\"wifi_success_after_ble_teardown\");" in success_body
    assert report_rel < success_body.index("self->deinit();") < success_body.index(
        "TryReportProvisioningAuthenticated(\"wifi_success_after_ble_teardown\");"
    )
    assert "ProvisioningStatusReporter::Status::DeviceAuthenticated" in helper_body
    assert "ClearProvisioningSecrets();" in helper_body
    assert "Reporting provisioning authenticated skipped" in helper_body
    assert custom_data_body.count("TryReportProvisioningAuthenticated") >= 2

def test_claim_confirm_uses_only_ble_bootstrap_token_not_realtime_ws_token():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")

    assert 'websocket_settings.GetString("bootstrap_token")' in refresh_body
    # websocket.token is the realtime/OTA token. It is not a claim bootstrap
    # token and can be stale/consumed, so the claim-confirm path must never fall
    # back to it.
    assert 'websocket_settings.GetString("token")' not in refresh_body

def test_pending_claim_without_bootstrap_token_keeps_ble_open_and_does_not_confirm():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    pending_start = refresh_body.index("Fetch succeeded with an active claim")
    pending_body = refresh_body[pending_start:]

    assert "if (token.empty())" in pending_body
    branch_start = pending_body.index("if (token.empty())")
    empty_token_branch = pending_body[branch_start:pending_body.index("StopClaimPoll();", branch_start)]
    assert "EnsureBleAdvertisingForStandby();" in empty_token_branch
    assert "StartClaimPoll();" in empty_token_branch
    assert "return;" in empty_token_branch
    assert "StopBleAdvertising();" not in empty_token_branch
    assert "ConfirmPendingTbotClaim" not in empty_token_branch

def test_failed_claim_confirm_clears_bootstrap_token_before_reopening_ble():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    retry_start = refresh_body.index("Auto-confirm POST did not land")
    retry_body = refresh_body[retry_start:]

    assert 'Settings websocket_settings("websocket", true);' in retry_body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in retry_body
    assert "pending_tbot_claim_token_.clear();" in retry_body
    assert "EnsureBleAdvertisingForStandby();" in retry_body

def test_successful_claim_confirm_clears_consumed_bootstrap_token_before_reboot():
    # The BluFi bootstrap token is attempt-scoped. After /claim/confirm succeeds
    # the device persists device_secret/ws_url, so leaving the consumed bootstrap
    # token in NVS makes the next reboot poll /device/config with stale auth and
    # can show a false backend retry state before WS comes online.
    source = read("main/application.cc")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")

    success_start = confirm_body.index("CancelClaimExpiryTimer();")
    success_end = confirm_body.index("pending_tbot_claim_ = PendingTbotClaim{}", success_start)
    success_body = confirm_body[success_start:success_end]

    assert 'Settings websocket_settings("websocket", true);' in success_body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in success_body
    assert "pending_tbot_claim_token_.clear();" in confirm_body[success_start:]

def test_button_confirm_without_bootstrap_token_waits_for_ble_instead_of_failing():
    source = read("main/application.cc")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")

    assert "pending_tbot_claim_token_.empty()" in confirm_body
    wait_branch = confirm_body[confirm_body.index("pending_tbot_claim_token_.empty()"):confirm_body.index(
        "ClaimConfirmationReporter::Confirm"
    )]
    assert "EnsureBleAdvertisingForStandby();" in wait_branch
    assert "StartClaimPoll();" in wait_branch
    assert "RenderClaimSubstate(claim_substate_);" in wait_branch
    assert "Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTION_CONFIRM_FAILED" not in wait_branch

def test_button_confirm_failure_clears_token_and_reopens_ble_for_retry():
    # Manual BOOT confirm uses ConfirmPendingTbotClaim() directly. If that POST
    # fails with a stale/consumed bootstrap token, it must recover the same way
    # as auto-confirm: clear attempt auth, keep the pending claim cached, reopen
    # BLE, and poll so the phone can send a fresh token.
    source = read("main/application.cc")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")
    failure_start = confirm_body.index("if (!confirmed)")
    failure_end = confirm_body.index("CancelClaimExpiryTimer();", failure_start)
    failure_body = confirm_body[failure_start:failure_end]

    assert 'Settings websocket_settings("websocket", true);' in failure_body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in failure_body
    assert "pending_tbot_claim_token_.clear();" in failure_body
    assert "pending_tbot_claim_ = PendingTbotClaim{}" not in failure_body
    assert "claim_substate_ = TbotClaimSubstate::AvailableStandby;" in failure_body
    assert "EnsureBleAdvertisingForStandby();" in failure_body
    assert "StartClaimPoll();" in failure_body

def test_cached_pending_claim_with_late_ble_token_confirms_without_refetching_config():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")

    assert "pending_tbot_claim_.active && !token.empty()" in refresh_body
    cached_start = refresh_body.index("pending_tbot_claim_.active && !token.empty()")
    fetch_idx = refresh_body.index("FetchPendingTbotClaimFromDeviceConfig")
    assert cached_start < fetch_idx
    cached_branch = refresh_body[cached_start:fetch_idx]

    # Once BluFi has delivered a fresh bootstrap token for an already-cached
    # backend claim, do not run another TLS /device/config fetch while BLE is
    # still active. Stop BLE first, then confirm the cached claim directly.
    assert "StopBleAdvertising();" in cached_branch
    assert "ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);" in cached_branch
    assert "FetchPendingTbotClaimFromDeviceConfig" not in cached_branch

def test_unclaimed_standby_does_not_poll_https_while_ble_is_active_without_bootstrap_token():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    fetch_idx = refresh_body.index("FetchPendingTbotClaimFromDeviceConfig")
    pre_fetch = refresh_body[token_read:fetch_idx]

    assert "Blufi::GetInstance().GetBleState()" in pre_fetch
    assert "Skipping claim config fetch while BLE is active" in pre_fetch
    ble_branch_start = pre_fetch.index("Blufi::GetInstance().GetBleState()")
    ble_branch = pre_fetch[ble_branch_start:]
    assert "StartClaimPoll();" in ble_branch
    assert ble_branch.index("Blufi::GetInstance().GetBleState()") < ble_branch.index("StartClaimPoll();")

def test_unclaimed_standby_does_not_poll_https_while_ble_is_active_with_stale_bootstrap_token():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    fallback_idx = refresh_body.index("FetchBackendApiUrlFromBootstrap(token)")
    fetch_idx = refresh_body.index("FetchPendingTbotClaimFromDeviceConfig")
    pre_network = refresh_body[token_read:min(fallback_idx, fetch_idx)]

    # A stale or just-arrived BluFi bootstrap token in NVS must not force a TLS
    # /device/config fetch while the BLE stack is still advertising/connected;
    # on ESP32-S3 that BLE+TLS overlap fails mbedTLS AES allocation. The token
    # handoff helper stops BLE first, then schedules the claim refresh.
    assert "Blufi::GetInstance().GetBleState()" in pre_network
    assert "pending_tbot_claim_.active" in pre_network
    assert "Skipping claim config fetch while BLE is active" in pre_network
    assert "StartClaimPoll();" in pre_network
    ble_branch_start = pre_network.index("Blufi::GetInstance().GetBleState()")
    ble_branch = pre_network[ble_branch_start:]
    assert "token.empty()" not in ble_branch[:ble_branch.index("StartClaimPoll();")]

def test_failed_auto_confirm_keeps_cached_pending_claim_for_late_ble_token():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    marker = "Auto-confirm POST did not land"
    retry_starts = []
    index = refresh_body.find(marker)
    while index != -1:
        retry_starts.append(index)
        index = refresh_body.find(marker, index + len(marker))

    assert retry_starts
    for retry_start in retry_starts:
        retry_end = refresh_body.index("StartClaimPoll();", retry_start)
        retry_body = refresh_body[retry_start:retry_end]

        assert 'websocket_settings.SetString("bootstrap_token", "");' in retry_body
        assert "pending_tbot_claim_token_.clear();" in retry_body
        assert "pending_tbot_claim_ = PendingTbotClaim{}" not in retry_body
        assert "pending_tbot_claim_api_url_.clear();" not in retry_body
