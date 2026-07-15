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
    assert "DispatchPendingTbotClaimFetch(api_url, token);" in refresh_body
    assert "FetchPendingTbotClaimFromDeviceConfig" in function_body(
        source, "void Application::ClaimFetchTask"
    )


def test_activation_done_does_not_leave_wifi_config_mode_after_boot_reprovisioning():
    source = read("main/application.cc")
    activation_done = function_body(source, "void Application::HandleActivationDoneEvent")

    assert "auto state = GetDeviceState();" in activation_done
    assert "state == kDeviceStateWifiConfiguring" in activation_done
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


def test_claim_flow_does_not_spend_bootstrap_token_on_legacy_provisioning_report():
    source = read("main/boards/common/blufi.cpp")
    report_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")

    # The backend bootstrap token is single-use. In the mobile claim flow the
    # robot must spend it on POST /claim/confirm, not on the legacy
    # provisioning-status report that would consume it first and leave mobile
    # stuck at WAITING_PHYSICAL_CONFIRM.
    claim_guard_idx = report_body.index('websocket_settings.GetString("claim_device_id")')
    first_report_start_idx = report_body.index('ProvisioningStatusReporter::Report')
    claim_guard = report_body[:first_report_start_idx]

    assert 'Settings websocket_settings("websocket", false);' in claim_guard
    assert 'websocket_settings.GetString("claim_device_id").empty()' in claim_guard
    assert "Skipping provisioning authenticated report during claim flow" in claim_guard
    assert "return;" in claim_guard[claim_guard_idx:]
    assert claim_guard_idx < first_report_start_idx

def test_claim_custom_data_persists_device_id_before_token_side_effects():
    source = read("main/boards/common/blufi.cpp")
    custom_data_body = function_body(source, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")

    prescan_idx = custom_data_body.index("int prescan_offset = 0;")
    prescan_store_idx = custom_data_body.index('websocket_settings.SetString("claim_device_id"', prescan_idx)
    token_branch_idx = custom_data_body.index("if (tag == 0x01)")
    first_report_idx = custom_data_body.index("TryReportProvisioningAuthenticated", token_branch_idx)

    assert prescan_idx < prescan_store_idx < token_branch_idx < first_report_idx
    assert "Claim device_id already persisted by custom-data prescan" in custom_data_body

def test_claim_flow_preserves_nvs_bootstrap_token_until_confirm():
    source = read("main/boards/common/blufi.cpp")
    clear_body = function_body(source, "void Blufi::ClearProvisioningSecrets")

    claim_guard_idx = clear_body.index('websocket_settings.GetString("claim_device_id").empty()')
    erase_idx = clear_body.index('websocket_settings.EraseKey("bootstrap_token")')
    preserve_idx = clear_body.index("Claim flow active; preserving NVS bootstrap token")

    assert claim_guard_idx < erase_idx < preserve_idx


def test_blufi_defers_connected_wifi_claim_refresh_until_after_possible_wifi_frames():
    source = read("main/boards/common/blufi.cpp")
    helper_body = function_body(source, "void Blufi::ScheduleClaimRefreshAfterTokenHandoff")

    assert "kClaimRefreshAfterTokenHandoffDelayMs" in source
    assert "vTaskDelay(pdMS_TO_TICKS(kClaimRefreshAfterTokenHandoffDelayMs));" in helper_body
    assert "m_sta_is_connecting" in helper_body
    connecting_branch = helper_body[helper_body.index("m_sta_is_connecting"):]
    teardown_idx = connecting_branch.index("CompleteSuccessfulProvisioningTeardown")
    assert "return;" in connecting_branch[:teardown_idx]
    assert '"connected_wifi_token_handoff"' in helper_body
    assert "SchedulePendingTbotClaimRefresh();" in helper_body

def test_one_shot_claim_fetch_after_wifi_success_applies_even_without_poll_timer():
    source = read("main/application.cc")
    task_body = function_body(source, "void Application::ClaimFetchTask")

    # SchedulePendingTbotClaimRefresh() can dispatch a one-shot device/config
    # fetch immediately after Wi-Fi provisioning, before StartClaimPoll() has
    # armed claim_poll_active_. Results with a bootstrap token must still apply:
    #  - active claim -> /claim/confirm (mobile "Đang chờ Robot xác thực")
    #  - claim_present=0 -> EnsureBleAdvertisingForStandby (else phone BLE_SCAN_TIMEOUT)
    stale_guard_start = task_body.index("if (!self->claim_poll_active_")
    apply_idx = task_body.index("ApplyPendingTbotClaimFetchResult")
    stale_guard = task_body[stale_guard_start:apply_idx]

    # Drop only when poll is off AND there is no bootstrap token (pure poll tick).
    assert "token.empty()" in stale_guard
    assert "return;" in stale_guard
    # Must not require pending_claim.active — claim_present=0 still needs apply.
    assert "pending_claim.active" not in stale_guard

def test_blufi_waits_for_full_wifi_board_connect_timeout_before_reporting_failure():
    source = read("main/boards/common/blufi.cpp")
    branch_start = source.index("void Blufi::StartStationConnectFromCredentials")
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
    assert "claim_poll_interval_us_ = desired_interval_us;" in start_body
    assert "esp_timer_start_periodic(claim_poll_timer_, claim_poll_interval_us_)" in start_body

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

def test_claimed_devices_leave_claim_fsm_before_standby_ble_advertising():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    ensure_body = function_body(source, "void Application::EnsureBleAdvertisingForStandby")
    stop_ble_body = function_body(source, "void Application::StopBleAdvertising")

    assert "IsDeviceClaimed()" in refresh_body
    assert "StopClaimPoll();" in refresh_body
    assert "StopBleAdvertising();" in refresh_body
    assert "return;" in refresh_body[refresh_body.index("if (IsDeviceClaimed())") :]
    assert "IsDeviceClaimed()" in ensure_body
    assert "StopBleAdvertising();" in ensure_body
    assert "blufi.deinit();" in stop_ble_body

    claimed_idx = refresh_body.index("if (IsDeviceClaimed())")
    standby_idx = refresh_body.index("Blufi::GetInstance().GetBleState()")
    claimed_branch = refresh_body[claimed_idx:refresh_body.index("return;", claimed_idx)]

    assert claimed_idx < standby_idx
    assert "StopClaimPoll();" in claimed_branch
    assert "StopBleAdvertising();" in claimed_branch
    assert "EnsureBleAdvertisingForStandby();" not in claimed_branch
    assert "if (IsDeviceClaimed())" in ensure_body
    assert "StopBleAdvertising();" in ensure_body[:ensure_body.index("if (blufi.GetBleState()")]
    assert "CancelBleSetupTimeout();" in stop_ble_body
    assert "StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);" in ensure_body


def test_factory_test_claimed_is_ota_controlled_and_requires_ws_token():
    ota = read("main/ota.cc")
    app = read("main/application.cc")
    ota_body = function_body(ota, "esp_err_t Ota::CheckVersion")
    claimed_body = function_body(app, "bool Application::IsDeviceClaimed")

    assert 'factory_test_claimed' in ota_body
    assert 'Settings claim_state("tbot_claim", true);' in ota_body
    assert 'claim_state.SetInt("factory_test",' in ota_body
    assert 'claim_state.SetInt("factory_test", 0);' in ota_body
    assert 'websocket_settings.GetString("token")' in claimed_body
    assert 'claim_state.GetInt("factory_test", 0) != 0' in claimed_body
    assert 'factory_test_claimed && !websocket_token.empty()' in claimed_body

def test_factory_test_claimed_nvs_key_fits_esp_idf_limit():
    ota = read("main/ota.cc")
    app = read("main/application.cc")
    ota_body = function_body(ota, "esp_err_t Ota::CheckVersion")
    claimed_body = function_body(app, "bool Application::IsDeviceClaimed")
    websocket_section = ota_body[
        ota_body.index('cJSON *websocket') : ota_body.index('cJSON *api_url')
    ]

    # ESP-IDF NVS keys are limited to 15 chars. The OTA JSON field can keep the
    # descriptive protocol name, but the persisted key must stay short or the
    # device aborts during OTA parsing before it can enter the claimed path.
    assert 'SetInt("factory_test_claimed"' not in ota_body
    assert 'GetInt("factory_test_claimed"' not in claimed_body
    assert 'settings.SetInt(item->string, item->valueint);' not in websocket_section
    assert 'std::strcmp(item->string, "factory_test_claimed") == 0' in websocket_section
    assert 'claim_state.SetInt("factory_test",' in websocket_section
    assert len("factory_test") <= 15


def test_saved_wifi_recovery_public_entry_keeps_claim_gate_inside_application():
    header = read("main/application.h")
    source = read("main/application.cc")
    recovery_body = function_body(source, "void Application::EnsureBleAdvertisingForUnclaimedSavedWifi")

    assert "void EnsureBleAdvertisingForUnclaimedSavedWifi();" in header
    assert "if (IsDeviceClaimed())" in recovery_body
    claimed_branch = recovery_body[recovery_body.index("if (IsDeviceClaimed())"):]
    assert "return;" in claimed_branch[:claimed_branch.index("EnsureBleAdvertisingForStandby();")]
    assert "Stored WiFi exists but device is unclaimed" in recovery_body
    assert "EnsureBleAdvertisingForStandby();" in recovery_body

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
    assert "CompleteSuccessfulProvisioningTeardown" in success_body
    assert '"wifi_credentials_connected"' in success_body
    assert "SchedulePendingTbotClaimRefresh();" in success_body
    teardown_idx = source.index("CompleteSuccessfulProvisioningTeardown", connected_log_idx)
    assert report_idx < connected_log_idx < teardown_idx
    assert teardown_idx < source.index(
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
    assert report_rel < success_body.index("CompleteSuccessfulProvisioningTeardown") < success_body.index(
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
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")
    pending_start = apply_body.index("Fetch succeeded with an active claim")
    pending_body = apply_body[pending_start:]

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
    claim_fetch_body = function_body(source, "void Application::ClaimFetchTask")

    assert "pending_tbot_claim_.active && !token.empty()" in refresh_body
    cached_start = refresh_body.index("pending_tbot_claim_.active && !token.empty()")
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token);")
    assert cached_start < dispatch_idx
    cached_branch = refresh_body[cached_start:dispatch_idx]

    # Once BluFi has delivered a fresh bootstrap token for an already-cached
    # backend claim, do not run another TLS /device/config fetch while BLE is
    # still active. Stop BLE first, then confirm the cached claim directly.
    assert "StopBleAdvertising();" in cached_branch
    assert "ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);" in cached_branch
    assert "FetchPendingTbotClaimFromDeviceConfig" not in cached_branch
    assert "FetchPendingTbotClaimFromDeviceConfig" in claim_fetch_body

def test_unclaimed_standby_without_bootstrap_token_never_polls_backend_config_even_when_ble_is_off():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    fallback_idx = refresh_body.index("FetchBackendApiUrlFromBootstrap(token)")
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token);")
    pre_network = refresh_body[token_read:min(fallback_idx, dispatch_idx)]

    # If BLE init failed or was already off, GetBleState() no longer protects the
    # no-token path. Standby must still reach backend /device/config without an
    # Authorization header so ClaimService.refreshSetupSignalIfUnclaimed keeps the
    # robot visible to the mobile claim list. The low-priority ClaimFetchTask owns
    # the blocking TLS fetch, so this no longer wedges the Application task.
    assert "Skipping claim config fetch until BluFi token handoff" not in pre_network
    assert "if (!pending_tbot_claim_.active && token.empty())" not in pre_network

def test_unclaimed_standby_polls_device_config_without_bootstrap_token_to_refresh_mobile_discovery():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token);")
    pre_fetch = refresh_body[token_read:dispatch_idx]

    ble_state_idx = pre_fetch.index("Blufi::GetInstance().GetBleState()")

    # There must be no generic early-return for an empty bootstrap token before
    # the backend config fetch is dispatched. FetchPendingTbotClaimFromDeviceConfig
    # omits the Authorization header when token is empty, and the backend accepts
    # that as an unclaimed setup-liveness poll.
    assert "if (!pending_tbot_claim_.active && token.empty())" not in pre_fetch[:ble_state_idx]
    assert "Skipping claim config fetch until BluFi token handoff" not in pre_fetch
    assert "Blufi::GetInstance().GetBleState()" in pre_fetch
    assert "DispatchPendingTbotClaimFetch(api_url, token);" in refresh_body

def test_unclaimed_standby_with_bootstrap_token_stops_ble_before_claim_fetch():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    fallback_idx = refresh_body.index("FetchBackendApiUrlFromBootstrap(token)")
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token);")
    pre_network = refresh_body[token_read:min(fallback_idx, dispatch_idx)]

    # Token present: do not strand mobile on WAITING_PHYSICAL_CONFIRM if BLE did
    # not cleanly deinit after Wi-Fi success. Stop BLE first, then fall through
    # to the backend claim fetch/confirm path so TLS never overlaps BLE. Empty
    # token is now allowed to fall through to the config poll for setup liveness.
    assert "Blufi::GetInstance().GetBleState()" in pre_network
    assert "pending_tbot_claim_.active" in pre_network
    assert "Skipping claim config fetch until BluFi token handoff" not in pre_network
    assert "if (!pending_tbot_claim_.active && token.empty())" not in pre_network
    ble_branch_start = pre_network.index("Blufi::GetInstance().GetBleState()")
    ble_branch = pre_network[ble_branch_start:]
    token_present_branch = ble_branch[ble_branch.index("Bootstrap token present but BLE still active"):]

    assert "CancelBleSetupTimeout();" in token_present_branch
    assert "StopBleAdvertising();" in token_present_branch
    assert "return;" not in token_present_branch[:token_present_branch.index("StopBleAdvertising();")]

def test_auth_rejected_device_config_clears_stale_bootstrap_token_without_server_unavailable_copy():
    source = read("main/application.cc")
    reporter_header = read("main/provisioning/claim_confirmation_reporter.h")
    claim_fetch_body = function_body(source, "void Application::ClaimFetchTask")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")

    fetch_idx = claim_fetch_body.index("FetchPendingTbotClaimFromDeviceConfig")
    failure_copy_idx = apply_body.index("Lang::Strings::SERVER_UNAVAILABLE_RETRYING")
    auth_branch_start = apply_body.index("device_config_status == 401")
    auth_branch = apply_body[auth_branch_start:failure_copy_idx]

    # A consumed/stale bootstrap token makes the backend return 401/403. That is
    # not a server outage and must not increment into the Vietnamese
    # "Máy chủ không khả dụng" copy. Clear the stale token, reopen claimable BLE,
    # and wait for the phone to deliver a fresh attempt token.
    assert "int device_config_status" in claim_fetch_body[:fetch_idx]
    assert "int* http_status_code" in reporter_header
    assert "device_config_status == 401" in auth_branch
    assert "device_config_status == 403" in auth_branch
    assert "Device config rejected bootstrap token" in auth_branch
    assert 'websocket_settings.SetString("bootstrap_token", "");' in auth_branch
    assert "pending_tbot_claim_token_.clear();" in auth_branch
    assert "pending_tbot_claim_ = PendingTbotClaim{};" in auth_branch
    assert "claim_fetch_failures_ = 0;" in auth_branch
    assert "claim_substate_ = TbotClaimSubstate::AvailableStandby;" in auth_branch
    assert "EnsureBleAdvertisingForStandby();" in auth_branch
    assert "StartClaimPoll();" in auth_branch
    assert "return;" in auth_branch
    assert auth_branch_start < failure_copy_idx

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
