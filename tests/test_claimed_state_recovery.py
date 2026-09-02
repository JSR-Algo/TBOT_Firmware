"""Regression locks for claimed-state recovery after reboot.

The robot can lose the small `tbot_claim.confirmed` marker while still retaining
the backend credentials written by a successful claim confirmation. In that
state it must recover as claimed, otherwise it falls back to "waiting for BluFi
token" and never opens the backend/websocket path.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/application.cc"


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
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_is_device_claimed_recovers_from_backend_credentials():
    body = function_body(SOURCE.read_text(encoding="utf-8"), "bool Application::IsDeviceClaimed")

    assert 'Settings claim_state("tbot_claim", false)' in body
    assert 'claim_state.GetInt("confirmed", 0) != 0' in body

    assert 'Settings backend_settings("backend", false)' in body
    assert 'backend_settings.GetString("device_id")' in body
    assert 'backend_settings.GetString("device_secret")' in body
    assert "!device_id.empty() && !device_secret.empty()" in body

    # A stale marker after an operator/backend unpair must never bypass the
    # credential check and strand the robot in a fake claimed/offline state.
    assert 'if (claim_state.GetInt("confirmed", 0) != 0)' not in body
    assert "claim_confirmed && (device_id.empty() || device_secret.empty())" in body


def test_refresh_claim_fsm_exits_early_on_recovered_claim_signal():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void Application::RefreshPendingTbotClaim")

    recovered_gate = "if (IsDeviceClaimed())"
    assert recovered_gate in body
    recovered_gate_idx = body.index(recovered_gate)
    token_read_idx = body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    ble_state_idx = body.index("Blufi::GetInstance().GetBleState()")
    cancel_expiry_idx = body.index("CancelClaimExpiryTimer();")

    assert recovered_gate_idx < cancel_expiry_idx
    assert recovered_gate_idx < token_read_idx
    assert recovered_gate_idx < ble_state_idx

    recovered_branch = body[recovered_gate_idx:body.index("return;", recovered_gate_idx)]
    assert "StopClaimPoll();" in recovered_branch
    assert "StopBleAdvertising();" in recovered_branch
    assert "EnsureBleAdvertisingForStandby();" not in recovered_branch


def test_claimed_reboot_starts_management_heartbeat_when_activation_reaches_idle():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleActivationDoneEvent")

    idle = body.index("SetDeviceState(kDeviceStateIdle);")
    claimed = body.index("if (ShouldKeepManagementHeartbeat())", idle)
    start = body.index("StartHeartbeat();", claimed)
    dispatch = body.index("DispatchDeviceHeartbeat();", start)

    assert idle < claimed < start < dispatch


def test_reboot_migrates_pre_fix_revoked_claim_into_wifi_setup():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleActivationDoneEvent")

    # Older firmware cleared only device_secret on heartbeat 401, leaving a
    # device_id/factory-test token combination that looked claimed forever.
    stale_gate = "HasStaleRevokedClaimIdentity()"
    assert stale_gate in body
    assert "HandleHeartbeatAuthFailure(401);" in body
    assert body.index(stale_gate) < body.index("SetDeviceState(kDeviceStateIdle);")
    assert body.index("HandleHeartbeatAuthFailure(401);") < body.index(
        "SetDeviceState(kDeviceStateIdle);"
    )

    header = (ROOT / "main/application.h").read_text(encoding="utf-8")
    assert "bool HasStaleRevokedClaimIdentity() const;" in header

    helper = function_body(source, "bool Application::HasStaleRevokedClaimIdentity")
    assert 'backend_settings.GetString("device_id")' in helper
    assert 'backend_settings.GetString("device_secret")' in helper
    assert 'claim_state.GetInt("confirmed", 0)' in helper


def test_unclaimed_reboot_restores_ble_when_token_claim_fetch_is_busy():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void Application::RefreshPendingTbotClaim")

    token_ble = body[
        body.index("!pending_tbot_claim_.active && !token.empty()") :
        body.index("#endif", body.index("!pending_tbot_claim_.active && !token.empty()"))
    ]
    assert "StopBleAdvertising();" in token_ble
    assert "paused_ble_for_fetch = true;" in token_ble

    dispatch = body[body.index("const bool claim_fetch_dispatched") :]
    assert "if (paused_ble_for_fetch && !claim_fetch_dispatched)" in dispatch
    assert "EnsureBleAdvertisingForStandby();" in dispatch


def test_bootstrap_token_claim_preempts_passive_websocket_and_keeps_retrying():
    source = SOURCE.read_text(encoding="utf-8")
    refresh = function_body(source, "void Application::RefreshPendingTbotClaim")
    direct = function_body(
        source, "void Application::DispatchPendingTbotClaimRefreshForSetupGeneration"
    )

    dispatch_at = refresh.index("const bool claim_fetch_dispatched")
    pre_dispatch = refresh[:dispatch_at]
    assert "if (!token.empty() && passive_ws_intent_.load())" in pre_dispatch
    assert "CloseAudioChannelByIntent();" in pre_dispatch

    failed = refresh[refresh.index("if (paused_ble_for_fetch && !claim_fetch_dispatched)") :]
    assert "EnsureBleAdvertisingForStandby();" in failed
    assert "StartClaimPoll();" in failed
    assert "StopClaimPoll();" not in failed[: failed.index("}")]

    direct_failed = direct[direct.index("if (!dispatched)") :]
    assert "if (!token.empty() && passive_ws_intent_.load())" in direct
    assert "CloseAudioChannelByIntent();" in direct
    assert "restore_standby_after_dispatch_failure = true" in direct_failed
    effects = function_body(source, "void Application::ExecuteClaimDeferredEffects")
    assert "StartClaimPoll();" in effects
