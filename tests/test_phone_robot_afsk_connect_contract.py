"""Contract tests for the phone-to-robot AFSK Wi-Fi connect flow.

The browser page in scripts/sonic_wifi_config.html is the phone-side encoder.
The robot decoder is main/boards/common/afsk_demod.cc. These tests bind the two
files together so a UI-side packet-format edit cannot drift from the firmware
parser that persists credentials and exits config mode.
"""

import re
from pathlib import Path

from repo_paths import resolve_robot_path

ROOT = Path(__file__).resolve().parents[1]
MOBILE_ULTRASONIC = resolve_robot_path(
    "esp32-server/main/manager-mobile/src/pages/device-config/components/ultrasonic-config.vue",
    ROOT,
)


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _normalize(source: str) -> str:
    return re.sub(r"\s+", " ", source).strip()


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def _byte_to_msb_bits(value: int) -> str:
    return ", ".join(str((value >> shift) & 1) for shift in range(7, -1, -1))


def test_phone_encoder_preserves_exact_wifi_field_values_before_encoding():
    html = read("scripts/sonic_wifi_config.html")
    generate = _function_body(html, "function generate()")

    assert "document.getElementById('ssid').value.trim()" not in generate
    assert "document.getElementById('pwd').value.trim()" not in generate
    assert "const ssid = document.getElementById('ssid').value;" in generate
    assert "const pwd = document.getElementById('pwd').value;" in generate


def test_phone_frame_format_matches_robot_afsk_parser_constants():
    html = _normalize(read("scripts/sonic_wifi_config.html"))
    afsk = read("main/boards/common/afsk_demod.cc")
    header = _normalize(read("main/boards/common/afsk_demod.h"))

    # Same tone/bit parameters on both sides; browser output is 44.1 kHz WAV,
    # but the transmitted mark/space/bit-rate are the firmware contract.
    assert "const MARK = 1800;" in html
    assert "const SPACE = 1500;" in html
    assert "const BIT_RATE = 100;" in html
    assert re.search(r"kMarkFrequency\s*=\s*1800", header)
    assert re.search(r"kSpaceFrequency\s*=\s*1500", header)
    assert re.search(r"kBitRate\s*=\s*100", header)

    # Phone sends 0x01,0x02 + UTF-8("SSID\npassword") + checksum + 0x03,0x04.
    assert "const START_BYTES = [0x01, 0x02];" in html
    assert "const END_BYTES = [0x03, 0x04];" in html
    assert "const dataStr = ssid + '\\n' + pwd;" in html
    assert "new TextEncoder().encode(dataStr)" in html
    assert "const fullBytes = [...START_BYTES, ...textBytes, checksum(textBytes), ...END_BYTES];" in html

    # Robot default start/end bit patterns are the MSB-first forms of those bytes.
    assert _byte_to_msb_bits(0x01) in afsk
    assert _byte_to_msb_bits(0x02) in afsk
    assert _byte_to_msb_bits(0x03) in afsk
    assert _byte_to_msb_bits(0x04) in afsk


def test_phone_checksum_and_bit_order_match_robot_decoder():
    html = read("scripts/sonic_wifi_config.html")
    afsk = read("main/boards/common/afsk_demod.cc")

    checksum_js = _normalize(_function_body(html, "function checksum(data)"))
    to_bits_js = _normalize(_function_body(html, "function toBits(byte)"))
    checksum_cpp = _normalize(_function_body(afsk, "AudioDataBuffer::CalculateChecksum"))
    bits_cpp = _normalize(_function_body(afsk, "AudioDataBuffer::ConvertBitsToBytes"))

    assert "return data.reduce((sum, b) => (sum + b) & 0xff, 0);" in checksum_js
    assert re.search(r"checksum\s*\+=\s*static_cast<uint8_t>\(\s*character\s*\)", checksum_cpp)

    assert re.search(r"for \(let i = 7; i >= 0; i--\)", to_bits_js)
    assert "bits.push((byte >> i) & 1);" in to_bits_js
    assert re.search(r"bits\[\s*i\s*\*\s*8\s*\+\s*j\s*\]\s*<<\s*\(\s*7\s*-\s*j\s*\)", bits_cpp)


def test_robot_persists_decoded_phone_credentials_then_exits_config_mode():
    afsk = _normalize(read("main/boards/common/afsk_demod.cc"))

    assert "newline_position = data_buffer.decoded_text->find('\\n')" in afsk
    assert "wifi_ssid = data_buffer.decoded_text->substr(0, newline_position)" in afsk
    assert "wifi_password = data_buffer.decoded_text->substr(newline_position + 1)" in afsk

    save_idx = afsk.index("ssid_manager.AddSsid(wifi_ssid, wifi_password)")
    stop_idx = afsk.index("wifi_manager->StopConfigAp()")
    reset_idx = afsk.index("data_buffer.decoded_text.reset()")
    return_idx = afsk.index("return", reset_idx)
    assert save_idx < stop_idx < reset_idx < return_idx


def test_mobile_ultrasonic_encoder_uses_same_frame_format_without_value_trimming():
    vue = _normalize(MOBILE_ULTRASONIC.read_text(encoding="utf-8"))

    assert "const MARK = 1800" in vue
    assert "const SPACE = 1500" in vue
    assert "const BIT_RATE = 100" in vue
    assert "const START_BYTES = [0x01, 0x02]" in vue
    assert "const END_BYTES = [0x03, 0x04]" in vue
    assert "const dataStr = `${props.selectedNetwork.ssid}\\n${props.password}`" in vue
    assert "const fullBytes = [...START_BYTES, ...textBytes, checksum(textBytes), ...END_BYTES]" in vue
    assert ".trim()" not in vue[vue.index("async function generateAndPlay"): vue.index("function buildWavOptimized")]


def test_mobile_ultrasonic_flow_never_logs_raw_wifi_password_value():
    vue = MOBILE_ULTRASONIC.read_text(encoding="utf-8")

    console_statements = [line.strip() for line in vue.splitlines() if "console." in line]
    offenders = [statement for statement in console_statements if "props.password" in statement]

    assert offenders == [], f"raw WiFi password reaches mobile console: {offenders}"
