"""Contract tests for the adversarial-review fixes to the connection flow.

These text-scrape source (they do NOT compile firmware) and assert that the
review's recommended fixes EXIST:
  * B1 — a 5s HTTP timeout on every claim/device-config/heartbeat client so a
    slow backend can never freeze the priority-10 main Application task;
  * H1 — the ConfirmTimeout -> AvailableStandby recovery (button tap re-enters
    the bounded standby poll instead of dead-ending on "Setup expired");
  * H2 — StopHeartbeat() wired to the disconnect/error/wifi-config paths and the
    heartbeat sender gated on a live online DeviceState (not just a token);
  * H3 — HandleStateChangedEvent routes the contract-owned states through
    TbotConnectMapper::Resolve so screen copy comes from kTbotConnectStateSpecs;
  * H4 — a /v1/device/bootstrap api_url fallback + an observable empty-api_url
    state after activation;
  * M1 — localized claim Alerts (no raw English literals);
  * M2 — an ESP_LOGW on an unrecognized (drifted) claim status;
  * M3 — the wall-clock claim-expiry arm is guarded on a synced clock;
  * L2 — repeated fetch failure renders "Server unavailable. Retrying...";
  * OQ1 — the main-task-only serialization assumption is documented.

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
# B1 — every claim/device-config/heartbeat HTTP client caps the blocking Open()
# ---------------------------------------------------------------------------

def test_heartbeat_http_client_caps_blocking_open_to_5s():
    source = read("main/application.cc")
    send_body = function_body(source, "int Application::SendDeviceHeartbeat")
    # The timeout must be set on the heartbeat client BEFORE Open() so a slow
    # backend cannot stall audio on the main task.
    assert "http->SetTimeout(5000);" in send_body
    assert send_body.index("http->SetTimeout(5000);") < send_body.index('http->Open("POST", url)')


def test_claim_and_device_config_http_clients_cap_blocking_open_to_5s():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    fetch_body = function_body(source, "bool FetchPendingTbotClaimFromDeviceConfig")
    confirm_body = function_body(source, "ClaimConfirmationResult ClaimConfirmationReporter::Confirm")

    assert "http->SetTimeout(5000);" in fetch_body
    assert fetch_body.index("http->SetTimeout(5000);") < fetch_body.index('http->Open("GET", url)')

    assert "http->SetTimeout(5000);" in confirm_body
    assert confirm_body.index("http->SetTimeout(5000);") < confirm_body.index('http->Open("POST", url)')


def test_shared_http_client_avoids_lazy_iostream_locale_initialization():
    source = read("components/esp-ml307/src/http_client.cc")

    # OTA, claim and heartbeat requests may begin on separate FreeRTOS tasks.
    # ESP libstdc++ lazy locale initialization can corrupt the heap when their
    # first stringstream constructions race, so this shared client stays locale-free.
    assert "#include <sstream>" not in source
    assert "std::ostringstream" not in source
    assert "std::istringstream" not in source


# ---------------------------------------------------------------------------
# H1 — ConfirmTimeout is not a dead-end (button tap -> AvailableStandby)
# ---------------------------------------------------------------------------

def test_confirm_timeout_button_tap_reenters_claim_standby_poll():
    source = read("main/application.cc")
    toggle_body = function_body(source, "void Application::HandleToggleChatEvent")

    # A tap while in ConfirmTimeout must route to claim-retry, NOT the talk path.
    assert "claim_substate_ == TbotClaimSubstate::ConfirmTimeout" in toggle_body
    assert "RefreshPendingTbotClaim();" in toggle_body
    # The retry branch must come before the protocol/talk path so the tap never
    # falls through to opening an audio channel from the expired screen.
    assert toggle_body.index("TbotClaimSubstate::ConfirmTimeout") < toggle_body.index(
        "Protocol not initialized"
    )


def test_confirm_timeout_to_available_standby_transition_is_in_the_contract():
    # The mapper contract row for CLAIM_CONFIRM_TIMEOUT must list CLAIM_AVAILABLE
    # as a next state so the recovery is contract-backed, not ad-hoc.
    state_header = read("main/tbot_connect_state.h")
    start = state_header.index("TbotConnectState::CLAIM_CONFIRM_TIMEOUT")
    nxt = state_header.index(".next_states", start)
    end = state_header.index("}", nxt)
    next_states = state_header[nxt:end]
    assert "TbotConnectState::CLAIM_AVAILABLE" in next_states


# ---------------------------------------------------------------------------
# H2 — heartbeat is stopped on disconnect/error/wifi-config and start-gated
# ---------------------------------------------------------------------------

def test_heartbeat_is_stopped_on_network_loss_and_setup_entry():
    source = read("main/application.cc")
    disconnect_body = function_body(source, "void Application::HandleNetworkDisconnectedEvent")
    state_changed_body = function_body(source, "void Application::HandleStateChangedEvent")

    # Disconnect path stops the heartbeat.
    assert "StopHeartbeat();" in disconnect_body
    # Entry into Wi-Fi configuring stops the heartbeat (it is not a live session).
    wifi_case_start = state_changed_body.index("case kDeviceStateWifiConfiguring:")
    wifi_case = state_changed_body[wifi_case_start:state_changed_body.index("break;", wifi_case_start)]
    assert "StopHeartbeat();" in wifi_case
    assert "display->SetStatus(connect_copy);" in wifi_case


def test_backend_error_preserves_only_claimed_idle_management_heartbeat():
    source = read("main/application.cc")
    proto_start = source.index("protocol_->OnConnected(")
    network_error_start = source.index("protocol_->OnNetworkError(", proto_start)
    incoming_audio_start = source.index("protocol_->OnIncomingAudio(", network_error_start)
    connected = source[proto_start:network_error_start]
    network_error = source[network_error_start:incoming_audio_start]

    assert "StartHeartbeat();" in connected
    assert "DispatchDeviceHeartbeat();" in connected
    assert "ShouldKeepManagementHeartbeat()" in network_error
    assert "StartHeartbeat();" in network_error
    assert "DispatchDeviceHeartbeat();" in network_error
    assert "StopHeartbeat();" in network_error
    assert network_error.index("ShouldKeepManagementHeartbeat()") < network_error.index(
        "StartHeartbeat();"
    ) < network_error.index("StopHeartbeat();")


def test_heartbeat_sender_is_gated_on_a_live_online_device_state():
    source = read("main/application.cc")
    dispatch_body = function_body(source, "void Application::DispatchDeviceHeartbeat")

    # Must check a live online DeviceState, not merely token presence.
    assert "GetDeviceState()" in dispatch_body
    assert "kDeviceStateIdle" in dispatch_body
    assert "kDeviceStateListening" in dispatch_body
    assert "kDeviceStateSpeaking" in dispatch_body
    # The state gate runs before work can be queued to the persistent worker.
    assert dispatch_body.index("kDeviceStateSpeaking") < dispatch_body.index("xQueueSend")


def test_heartbeat_auth_failure_clears_stale_claim_credentials_and_reboots_into_setup():
    source = read("main/application.cc")
    header = read("main/application.h")
    heartbeat_task_body = function_body(source, "void Application::HeartbeatTask")
    recovery_body = function_body(source, "void Application::HandleHeartbeatAuthFailure")

    # A 401/403 heartbeat means the backend rejects the locally-persisted
    # device_secret or claimed flag. Keeping the robot locally claimed here
    # reproduces the field symptom: robot says connected, mobile keeps waiting.
    assert "void HandleHeartbeatAuthFailure(int status_code);" in header
    assert "status_code == 401 || status_code == 403" in heartbeat_task_body
    assert "HandleHeartbeatAuthFailure(status_code);" in heartbeat_task_body
    assert heartbeat_task_body.index("status_code == 401 || status_code == 403") > heartbeat_task_body.index("Schedule(")
    assert "vTaskDelete" not in heartbeat_task_body

    assert 'Settings backend_settings("backend", true);' in recovery_body
    assert 'backend_settings.SetString("device_secret", "");' in recovery_body
    assert 'Settings claim_state("tbot_claim", true);' in recovery_body
    assert 'claim_state.SetInt("confirmed", 0);' in recovery_body
    assert "pending_tbot_claim_ = PendingTbotClaim{};" in recovery_body
    assert "claim_substate_ = TbotClaimSubstate::AvailableStandby;" in recovery_body
    assert "StopHeartbeat();" in recovery_body
    assert "CloseAudioChannelByIntent();" in recovery_body
    assert 'backend_settings.SetString("device_id", "");' in recovery_body
    assert 'websocket_settings.SetString("token", "");' in recovery_body
    assert 'websocket_settings.SetString("url", "");' in recovery_body
    assert "SsidManager::GetInstance().ForceClearAndCancelTransaction()" in recovery_body
    assert "esp_restart();" in recovery_body

# ---------------------------------------------------------------------------
# H3 — all-21 mapper routing in HandleStateChangedEvent (text-scrapable)
# ---------------------------------------------------------------------------

def test_state_changed_event_resolves_copy_through_the_connect_mapper():
    source = read("main/application.cc")
    body = function_body(source, "void Application::HandleStateChangedEvent")

    # The render resolves the live runtime through the mapper instead of using
    # hand-coded xiaozhi literals as the source of truth.
    assert "TbotConnectMapper::Resolve(" in body
    assert "ConnectStateScreenCopy(" in body
    assert "claim_substate_" in body
    assert "GetBleSubstate()" in body
    assert "backend_offline_.load()" in body

    # The contract-owned DeviceStates set their status from the resolved copy.
    for device_state in (
        "kDeviceStateStarting",        # BOOT
        "kDeviceStateActivating",      # BOOTSTRAP_FETCHING
        "kDeviceStateConnecting",      # BACKEND_CONNECTING
        "kDeviceStateIdle",            # ONLINE / OFFLINE_RETRY / claim overlay
        "kDeviceStateUpgrading",       # OTA_UPDATING
        "kDeviceStateFatalError",      # ERROR_RECOVERABLE
    ):
        assert device_state in body, f"HandleStateChangedEvent missing {device_state} case"
    # The redirected status renders use the mapper-derived copy.
    assert "display->SetStatus(connect_copy);" in body


def test_connect_state_copy_helper_reads_from_the_contract_table():
    source = read("main/application.cc")
    helper = function_body(source, "static const char* ConnectStateScreenCopy")

    # Localized override where a key exists, contract screen_text as the fallback
    # (so copy never drifts from kTbotConnectStateSpecs).
    assert "spec->state" in helper
    assert "TbotConnectState::ONLINE" in helper
    assert "TbotConnectState::BACKEND_CONNECTING" in helper
    assert "TbotConnectState::OTA_UPDATING" in helper
    assert "return spec->screen_text;" in helper


# ---------------------------------------------------------------------------
# H4 — bootstrap api_url fallback + observable empty-api_url state
# ---------------------------------------------------------------------------

def test_backend_api_url_has_a_bootstrap_fallback_when_ota_omits_it():
    header = read("main/provisioning/claim_confirmation_reporter.h")
    reporter = read("main/provisioning/claim_confirmation_reporter.cc")
    app = read("main/application.cc")

    assert "std::string FetchBackendApiUrlFromBootstrap(" in header
    fetch_body = function_body(reporter, "std::string FetchBackendApiUrlFromBootstrap")
    url_body = function_body(reporter, "std::string BuildTbotDeviceBootstrapUrl")

    # Bootstrap URL is derived from the compiled provisioning-status surface.
    assert "CONFIG_PROVISIONING_STATUS_URL" in url_body
    assert "bootstrap" in url_body
    # Fetch reads api_url and persists it into Settings("backend").
    assert 'cJSON_GetObjectItem(root, "api_url")' in fetch_body
    assert 'Settings settings("backend", true);' in fetch_body
    assert 'settings.SetString("api_url"' in fetch_body
    assert "http->SetTimeout(5000);" in fetch_body

    # RefreshPendingTbotClaim uses the fallback only when api_url is empty.
    refresh_body = function_body(app, "void Application::RefreshPendingTbotClaim")
    assert "FetchBackendApiUrlFromBootstrap(token)" in refresh_body


def test_empty_backend_api_url_is_observable_after_activation():
    app = read("main/application.cc")
    refresh_body = function_body(app, "void Application::RefreshPendingTbotClaim")

    # A still-empty api_url after the fallback warns AND surfaces an Alert (not a
    # silent skip).
    assert "ESP_LOGW(TAG, \"No backend api_url" in refresh_body
    assert "Alert(Lang::Strings::TBOT_CONNECT" in refresh_body
    # The dead-feature handling still stops the poll and clears the substate.
    assert "StopClaimPoll();" in refresh_body
    assert "TbotClaimSubstate::None" in refresh_body


# ---------------------------------------------------------------------------
# M1 — claim Alerts are localized (no raw English literals)
# ---------------------------------------------------------------------------

def test_claim_flow_alerts_are_localized():
    app = read("main/application.cc")

    # No raw English claim literals remain.
    assert '"TBot Connect"' not in app
    assert '"Connected."' not in app
    assert '"Connection confirmation failed. Try again."' not in app

    # The localized keys are used instead.
    assert "Lang::Strings::TBOT_CONNECT" in app
    assert "Lang::Strings::CONNECTED" in app
    assert "Lang::Strings::CONNECTION_CONFIRM_FAILED" in app
    assert "Lang::Strings::PRESS_BUTTON_TO_CONFIRM" in app


def test_new_localized_keys_exist_in_assets_and_generated_header():
    en = read("main/assets/locales/en-US/language.json")
    vi = read("main/assets/locales/vi-VN/language.json")
    generator = read("scripts/gen_lang.py")

    required = {
        "TBOT_CONNECT": "TBot Connect",
        "CONNECTED": "Connected.",
        "CONNECTION_CONFIRM_FAILED": "Connection confirmation failed. Try again.",
    }
    assert "key.upper()" in generator
    for key, en_text in required.items():
        assert f'"{key}": "{en_text}"' in en, f"en-US missing {key}"
        assert f'"{key}":' in vi, f"vi-VN missing {key}"


# ---------------------------------------------------------------------------
# M2 — drifted claim status is observable
# ---------------------------------------------------------------------------

def test_unrecognized_claim_status_is_logged():
    reporter = read("main/provisioning/claim_confirmation_reporter.cc")
    parse_body = function_body(reporter, "bool ParsePendingTbotClaimFromDeviceConfigJson")

    assert "Unrecognized claim status" in parse_body
    assert "ESP_LOGW(TAG," in parse_body
    # Still requires the exact expected wire value for the active path.
    assert "WAITING_PHYSICAL_CONFIRM" in parse_body


# ---------------------------------------------------------------------------
# M3 — wall-clock claim expiry is only armed when the clock is real
# ---------------------------------------------------------------------------

def test_claim_expiry_arm_is_guarded_on_a_synced_clock():
    app = read("main/application.cc")
    arm_body = function_body(app, "void Application::ArmClaimExpiryTimer")

    assert "has_server_time_" in arm_body
    # A 2024-01-01 sanity floor guards against a 1970 clock arming decades out.
    assert "1704067200" in arm_body
    assert "relying on poll-window cap" in arm_body
    # The monotonic poll-cap backstop is untouched (still parses + arms once when
    # the clock is good).
    assert "esp_timer_start_once(claim_expiry_timer_" in arm_body


# ---------------------------------------------------------------------------
# L2 — repeated fetch failure shows the retry copy, not "Ready to connect"
# ---------------------------------------------------------------------------

def test_repeated_fetch_failure_renders_server_unavailable_copy():
    app = read("main/application.cc")
    header = read("main/application.h")
    apply_body = function_body(app, "void Application::ApplyPendingTbotClaimFetchResult")

    # A consecutive-failure counter distinguishes "backend down" from "no claim".
    assert "claim_fetch_failures_" in header
    assert "++claim_fetch_failures_;" in apply_body
    assert "Lang::Strings::SERVER_UNAVAILABLE_RETRYING" in apply_body
    # A successful fetch resets the streak (so a single transient miss is benign).
    assert "claim_fetch_failures_ = 0;" in apply_body


def test_successful_fetch_after_server_retry_restores_ready_to_connect_copy():
    app = read("main/application.cc")
    apply_body = function_body(app, "void Application::ApplyPendingTbotClaimFetchResult")

    # Once the backend recovers and /device/config succeeds with no active claim,
    # the visible retry banner must be replaced by the claimable standby copy.
    # Otherwise the robot stays stuck on "Server unavailable. Retrying..." even
    # though it is again scannable/connectable from the phone.
    assert "had_claim_fetch_failures" in apply_body
    assert "had_claim_fetch_failures" in apply_body[apply_body.index("claim_fetch_failures_ = 0;"):]
    assert "RenderClaimSubstate(claim_substate_);" in apply_body


def test_boot_tap_on_unclaimed_idle_reenters_phone_scan_standby_not_audio():
    app = read("main/application.cc")
    toggle_body = function_body(app, "void Application::HandleToggleChatEvent")

    # Product contract: if the robot is not owned yet, BOOT is a setup/scan
    # affordance. It must refresh claimable standby + BLE advertising instead of
    # falling through to the realtime-audio open path.
    assert "!IsDeviceClaimed()" in toggle_body
    assert "TbotClaimSubstate::AvailableStandby" in toggle_body
    unclaimed_idx = toggle_body.index("!IsDeviceClaimed()")
    audio_idx = toggle_body.index("Protocol not initialized")
    assert unclaimed_idx < audio_idx
    assert "RefreshPendingTbotClaim();" in toggle_body[unclaimed_idx:audio_idx]

def test_boot_tap_on_unclaimed_offline_retry_reopens_phone_scan_standby():
    app = read("main/application.cc")
    toggle_body = function_body(app, "void Application::HandleToggleChatEvent")

    # OFFLINE_RETRY is visible as "Server unavailable. Retrying..." while the
    # robot is still unclaimed. A BOOT tap in that state is a setup affordance:
    # clear the stale offline overlay, render claimable standby, and refresh the
    # BLE-backed claim poll before any chat/reconnect path can run.
    assert "backend_offline_.load()" in toggle_body
    offline_idx = toggle_body.index("backend_offline_.load()")
    audio_idx = toggle_body.index("Protocol not initialized")
    assert offline_idx < audio_idx
    offline_branch = toggle_body[offline_idx:audio_idx]
    assert "!IsDeviceClaimed()" in offline_branch
    assert "backend_offline_.store(false)" in offline_branch
    assert "TbotClaimSubstate::AvailableStandby" in offline_branch
    assert "RenderClaimSubstate(claim_substate_);" in offline_branch
    assert "RefreshPendingTbotClaim();" in offline_branch


def test_claim_poll_can_run_while_online_until_device_is_claimed():
    app = read("main/application.cc")
    start_body = function_body(app, "void Application::StartClaimPoll")

    # A realtime/session intent alone is not ownership. Online-but-unclaimed
    # robots must keep polling /device/config so the phone's claim request is
    # observed. Only claimed robots may suppress the blocking claim poll.
    assert "online_intent_.load() && IsDeviceClaimed()" in start_body
    assert "return;" in start_body[start_body.index("online_intent_.load() && IsDeviceClaimed()"):]


def test_wifi_config_mode_suppresses_claim_backend_poll():
    app = read("main/application.cc")
    refresh_body = function_body(app, "void Application::RefreshPendingTbotClaim")
    state_changed_body = function_body(app, "void Application::HandleStateChangedEvent")

    # BOOT/explicit Wi-Fi setup stops the station and opens BluFi. A stale claim
    # poll must not fetch /device/config in that state, because it overwrites the
    # setup/BluFi screen with "Server unavailable. Retrying..." while the robot is
    # intentionally waiting for mobile provisioning.
    assert "GetDeviceState() == kDeviceStateWifiConfiguring" in refresh_body
    assert refresh_body.index("kDeviceStateWifiConfiguring") < refresh_body.index(
        "DispatchPendingTbotClaimFetch"
    )

    wifi_case_start = state_changed_body.index("case kDeviceStateWifiConfiguring:")
    wifi_case = state_changed_body[wifi_case_start:state_changed_body.index("break;", wifi_case_start)]
    assert "StopClaimPoll();" in wifi_case

def test_wifi_config_mode_ignores_stale_network_connected_and_ws_close_callbacks():
    app = read("main/application.cc")
    network_body = function_body(app, "void Application::HandleNetworkConnectedEvent")

    # Entering explicit BOOT Wi-Fi setup can race with stale STA connected / WS
    # closed events from the old online session. Those callbacks must not leave
    # setup mode and render ONLINE / "Connected" before the phone finishes.
    wifi_idx = network_body.index("state == kDeviceStateWifiConfiguring")
    activating_idx = network_body.index("SetDeviceState(kDeviceStateActivating)")
    assert wifi_idx < activating_idx
    wifi_branch = network_body[wifi_idx:activating_idx]
    assert "return;" in wifi_branch

    closed_start = app.index("protocol_->OnAudioChannelClosed")
    closed_end = app.index("protocol_->OnIncomingJson", closed_start)
    closed_body = app[closed_start:closed_end]
    wifi_close_idx = closed_body.index("kDeviceStateWifiConfiguring")
    idle_idx = closed_body.index("SetDeviceState(kDeviceStateIdle)")
    reconnect_idx = closed_body.index("ScheduleReconnect")
    assert wifi_close_idx < idle_idx
    assert wifi_close_idx < reconnect_idx
    wifi_close_branch = closed_body[wifi_close_idx:idle_idx]
    assert "return;" in wifi_close_branch


def test_ota_retry_does_not_steal_wifi_config_screen_or_alert_transient_failures():
    app = read("main/application.cc")
    check_body = function_body(app, "void Application::CheckNewVersion")

    wifi_idx = check_body.index("kDeviceStateWifiConfiguring")
    status_idx = check_body.index("display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION)")
    assert wifi_idx < status_idx

    final_idx = check_body.index("attempt + 1 >= kOtaCheckMaxAttempts")
    retry_idx = check_body.index("const int retry_delay", final_idx)
    assert check_body.count("Alert(") == 1
    assert final_idx < check_body.index("Alert(") < retry_idx
    assert "Alert(" not in check_body[retry_idx:]


def test_ota_retry_phase_has_fixed_attempts_delays_and_budget_then_returns():
    app = read("main/application.cc")
    check_body = function_body(app, "void Application::CheckNewVersion")

    assert "kOtaCheckMaxAttempts = 3" in app
    assert "kOtaRetryDelaysSeconds[] = {2, 4}" in app
    assert "static constexpr int kHttpTimeoutMs = 8000" in read("main/ota.h")
    assert "kOtaCheckPhaseBudgetMs" in app
    assert "kOtaCheckMaxAttempts * Ota::kHttpTimeoutMs" in app
    assert "static_assert(kOtaCheckPhaseBudgetMs <= 60000" in app
    assert "attempt < kOtaCheckMaxAttempts" in check_body
    assert "kOtaRetryDelaysSeconds[attempt]" in check_body
    assert "retry_delay *= 2" not in check_body
    assert "MAX_RETRY" not in check_body

    final_failure = check_body[check_body.index("if (attempt + 1 >= kOtaCheckMaxAttempts)") :]
    assert final_failure.count("Alert(") == 1
    assert "return;" in final_failure[: final_failure.index("const int retry_delay")]


def test_ota_retry_wait_aborts_for_idle_wifi_config_and_audio_test():
    app = read("main/application.cc")
    check_body = function_body(app, "void Application::CheckNewVersion")
    wait_start = check_body.index("for (int elapsed_seconds")
    wait_end = check_body.index("if (ota_->HasNewVersion())", wait_start)
    wait_body = check_body[wait_start:wait_end]

    assert "kDeviceStateWifiConfiguring" in wait_body
    assert "kDeviceStateAudioTesting" in wait_body
    assert "kDeviceStateIdle" in wait_body
    assert wait_body.count("return;") >= 2

# ---------------------------------------------------------------------------
# OQ1 — main-task-only serialization assumption is documented
# ---------------------------------------------------------------------------

def test_main_task_only_serialization_is_documented():
    header = read("main/application.h")
    # The note must sit near the claim state members.
    note_idx = header.index("main-task-only")
    members_idx = header.index("PendingTbotClaim pending_tbot_claim_")
    assert abs(note_idx - members_idx) < 600
    assert "OQ1" in header
