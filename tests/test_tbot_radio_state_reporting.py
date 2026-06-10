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


def test_wifi_board_status_reports_ble_and_ap_radio_states():
    wifi_board = read("main/boards/common/wifi_board.cc")
    status_body = function_body(wifi_board, "std::string WifiBoard::GetDeviceStatusJson()")

    assert 'cJSON_AddStringToObject(root, "ble_state", Blufi::GetInstance().GetBleStateString());' in status_body
    assert 'cJSON_AddStringToObject(root, "ble_state", "off");' in status_body
    assert 'cJSON_AddStringToObject(root, "ap_state", GetApStateString());' in status_body


def test_ap_state_reporting_is_backend_safe_after_timeout_teardown():
    wifi_board = read("main/boards/common/wifi_board.cc")
    ap_state_body = function_body(wifi_board, "const char* WifiBoard::GetApStateString() const")

    assert 'return "active";' in ap_state_body
    assert 'return "off";' in ap_state_body
    assert 'return "timeout";' not in ap_state_body
