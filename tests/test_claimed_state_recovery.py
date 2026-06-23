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


def test_refresh_claim_standby_gate_uses_recovered_claim_signal():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void Application::RefreshPendingTbotClaim")

    standby = 'if (!pending_tbot_claim_.active && token.empty())'
    assert standby in body
    standby_idx = body.index(standby)
    recovered_gate = "if (IsDeviceClaimed())"
    assert recovered_gate in body
    recovered_gate_idx = body.index(recovered_gate)
    assert recovered_gate_idx < standby_idx

    recovered_branch = body[recovered_gate_idx:standby_idx]
    assert "StopClaimPoll();" in recovered_branch
    assert "StopBleAdvertising();" in recovered_branch
    assert "return;" in recovered_branch
