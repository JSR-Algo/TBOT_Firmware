import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
STATE_HEADER = ROOT / "main" / "tbot_connect_state.h"

REQUIRED_STATES = [
    "BOOT",
    "WIFI_NOT_CONFIGURED",
    "WIFI_CONNECTING",
    "WIFI_CONNECTED",
    "BOOTSTRAP_FETCHING",
    "CLAIM_AVAILABLE",
    "CLAIM_WAITING_CONFIRM",
    "CLAIM_CONFIRM_TIMEOUT",
    "CLAIM_CONFIRMED",
    "CLAIMED",
    "QR_CODE_VISIBLE_FALLBACK",
    "PAIRING_CODE_VISIBLE_FALLBACK",
    "BLE_SETUP_ADVERTISING",
    "BLE_SETUP_TIMEOUT",
    "AP_SETUP_ACTIVE",
    "AP_SETUP_TIMEOUT",
    "BACKEND_CONNECTING",
    "ONLINE",
    "OFFLINE_RETRY",
    "OTA_UPDATING",
    "ERROR_RECOVERABLE",
]


def read_state_header() -> str:
    assert STATE_HEADER.exists(), "missing explicit TBOT connection state contract"
    return STATE_HEADER.read_text(encoding="utf-8")


def parse_state_specs(text: str) -> dict[str, str]:
    specs: dict[str, str] = {}
    for match in re.finditer(r"\.state\s*=\s*TbotConnectState::(?P<state>\w+)(?P<body>.*?)(?=\n\s*\{\s*\.state\s*=|\n\};)", text, re.DOTALL):
        specs[match.group("state")] = match.group("body")
    return specs


def field_value(body: str, field: str) -> str:
    match = re.search(rf"\.{field}\s*=\s*([^,\n]+)", body)
    assert match, f"missing {field} field"
    return match.group(1).strip()


def test_state_contract_defines_every_required_robot_state_with_behavior_fields():
    text = read_state_header()
    specs = parse_state_specs(text)

    assert list(specs) == REQUIRED_STATES
    for state in REQUIRED_STATES:
        body = specs[state]
        assert field_value(body, "screen_text") != '""', f"{state} needs screen text"
        assert field_value(body, "timeout_seconds").isdigit(), f"{state} needs numeric timeout"
        assert field_value(body, "retry_policy").startswith("TbotRetryPolicy::"), f"{state} needs retry policy"
        assert field_value(body, "ble_state").startswith("TbotSetupRadioState::"), f"{state} needs BLE state"
        assert field_value(body, "ap_state").startswith("TbotSetupRadioState::"), f"{state} needs AP state"
        assert field_value(body, "recovery_action").startswith("TbotRecoveryAction::"), f"{state} needs recovery action"
        assert ".next_states" in body, f"{state} needs next states"
        assert ".next_state_count" in body, f"{state} needs next-state count"


def test_setup_and_online_invariants_are_encoded_in_state_contract():
    specs = parse_state_specs(read_state_header())

    online = specs["ONLINE"]
    assert field_value(online, "ble_state") == "TbotSetupRadioState::Off"
    assert field_value(online, "ap_state") == "TbotSetupRadioState::Off"
    assert field_value(online, "screen_text") == '"Connected"'

    ble_setup = specs["BLE_SETUP_ADVERTISING"]
    assert field_value(ble_setup, "ble_state") == "TbotSetupRadioState::Advertising"
    assert int(field_value(ble_setup, "timeout_seconds")) == 300
    assert "BLE_SETUP_TIMEOUT" in ble_setup

    ap_setup = specs["AP_SETUP_ACTIVE"]
    assert field_value(ap_setup, "ap_state") == "TbotSetupRadioState::Active"
    assert int(field_value(ap_setup, "timeout_seconds")) == 600
    assert "AP_SETUP_TIMEOUT" in ap_setup

    assert "CLAIM_WAITING_CONFIRM" in specs["CLAIM_AVAILABLE"]
    assert "CLAIM_CONFIRM_TIMEOUT" in specs["CLAIM_WAITING_CONFIRM"]
    assert "BACKEND_CONNECTING" in specs["CLAIMED"]

def test_physical_confirm_state_names_the_user_decision_and_button_action():
    specs = parse_state_specs(read_state_header())
    screen_text = field_value(specs["CLAIM_WAITING_CONFIRM"], "screen_text")

    assert "Allow connection?" in screen_text
    assert "Press button" in screen_text
