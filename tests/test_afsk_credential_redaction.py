"""Source-level regression tests locking the AFSK audio Wi-Fi provisioning
credential-redaction invariants.

TBOT policy: no raw secret values are logged or displayed. The audio-FSK
provisioning path in main/boards/common/afsk_demod.cc decodes a payload of the
form "SSID\\npassword"; neither the full payload nor the password value may ever
reach the ESP serial log (any ESP_LOG* / printf-family / std::cout), nor the
robot LCD (any display->Set*/Show* call), in any shape (.c_str(), .data(),
dereference, copy, concatenation). Only a redacted length is permitted.

These are static assertions over the firmware .cc text (no device needed),
following the convention in tests/test_blufi_provisioning_stability.py (see FW8)
and tests/test_wifi_provisioning_brand.py: module-level ROOT, a read(path)
helper, and test_* functions asserting on substrings / regex / structure.

Scanning is STATEMENT-oriented, not line-oriented: afsk_demod.cc's log calls
span multiple physical lines (the secret-bearing argument sits on a
continuation line that contains no "ESP_LOG" token). A line-oriented scan is
blind to that continuation line, so a multi-line
    ESP_LOGI(kLogTag, "raw=%s",
             data_buffer.decoded_text->c_str());
would pass while leaking the full payload. _statements() collapses logical
statements first so the argument expressions are visible to every check.
test_afsk9_* guards this property of the harness itself.

Each test is tagged with its invariant id (AFSK1..AFSK9).
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

AFSK_SRC = "main/boards/common/afsk_demod.cc"

# Output sinks that reach the serial console / UART / LCD. ESP_LOG* covers
# LOGE/LOGW/LOGI/LOGD/LOGV (errors and warnings are just as visible as info).
_SINK_RE = re.compile(
    r"\bESP_LOG\w*\s*\("
    r"|\b(?:printf|fprintf|snprintf|vprintf|vfprintf|puts|fputs|fwrite)\s*\("
    r"|\bstd::cout\b"
    r"|->Set\w*\s*\("
    r"|->Show\w*\s*\("
)

# The two attacker-supplied secret expressions that must never reach a sink as a
# value. Each may appear in a sink ONLY as a redacted length.
_PASSWORD_VAR = "wifi_password"
_PASSWORD_LEN = "wifi_password.length()"
_PAYLOAD_VAR = "decoded_text"               # data_buffer.decoded_text-> ... ("SSID\npassword" blob)
_PAYLOAD_LEN = "decoded_text->length()"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _strip_comments(source: str) -> str:
    """Remove C/C++ comments so commented examples and the literal word
    'password' inside comments never pollute the scan."""
    no_block = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", no_block)


def _statements(source: str):
    """Logical C++ statements, robust to multi-line calls.

    Comments are stripped first (while newlines still exist, so // works), then
    string-literal contents are blanked (so any ; { } inside a literal cannot
    mis-split a statement, and format-string text never matches a variable
    token), whitespace is collapsed, and the text is split on statement/block
    boundaries (; { }). A multi-line `ESP_LOGI(tag,\\n  fmt,\\n  secret);` thus
    becomes a single fragment whose argument expressions are visible to the
    substring checks, while a `if (...decoded_text.has_value()) {` block header
    is isolated into its own (non-sink) fragment rather than bleeding into the
    statement that follows it.
    """
    no_comments = _strip_comments(source)
    blanked = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', no_comments)
    collapsed = re.sub(r"\s+", " ", blanked)
    return [s.strip() for s in re.split(r"[;{}]", collapsed) if s.strip()]


def _sink_statements(source: str):
    return [s for s in _statements(source) if _SINK_RE.search(s)]


def _sink_value_offenders(statements, var_token: str, length_form: str):
    """Sink statements where `var_token` reaches the sink as anything other than
    its permitted redacted `length_form`."""
    offenders = []
    for stmt in statements:
        residual = stmt.replace(length_form, "")
        if var_token in residual:
            offenders.append(stmt)
    return offenders


# ---------------------------------------------------------------------------
# AFSK1: the two historical leaking log lines are gone (documents the exact
#        regression that was fixed; the class is covered structurally below).
# ---------------------------------------------------------------------------
def test_afsk1_old_cleartext_credential_logs_are_removed():
    src = read(AFSK_SRC)

    # The full decoded-text dump ("SSID\npassword") must not be logged verbatim.
    assert 'ESP_LOGI(kLogTag, "Received text data: %s"' not in src
    # The explicit raw-password log ("... Password: %s", ..., wifi_password) is gone.
    assert 'Password: %s"' not in src
    # The historical LCD dump of the raw payload is gone.
    assert 'SetChatMessage("system", data_buffer.decoded_text->c_str())' not in src


# ---------------------------------------------------------------------------
# AFSK2: the raw password value never reaches ANY sink (log/printf/display),
#        in any shape, across multi-line statements. Length-only is permitted.
# ---------------------------------------------------------------------------
def test_afsk2_password_value_never_reaches_a_sink():
    offenders = _sink_value_offenders(
        _sink_statements(read(AFSK_SRC)), _PASSWORD_VAR, _PASSWORD_LEN
    )
    assert offenders == [], f"password value reaches a sink: {offenders}"


# ---------------------------------------------------------------------------
# AFSK3: the full decoded "SSID\npassword" payload never reaches ANY sink,
#        in any shape. Only its ->length() may be logged.
# ---------------------------------------------------------------------------
def test_afsk3_decoded_payload_never_reaches_a_sink():
    offenders = _sink_value_offenders(
        _sink_statements(read(AFSK_SRC)), _PAYLOAD_VAR, _PAYLOAD_LEN
    )
    assert offenders == [], f"decoded payload reaches a sink: {offenders}"


# ---------------------------------------------------------------------------
# AFSK4: the password value is never materialized or copied (defeats the
#        "copy into an alias, then log the alias" and .data() bypasses).
# ---------------------------------------------------------------------------
def test_afsk4_password_value_never_extracted_or_copied():
    src = _strip_comments(read(AFSK_SRC))

    # No raw-buffer accessor on the password anywhere in the file.
    assert "wifi_password.c_str()" not in src
    assert "wifi_password.data()" not in src

    # No statement copies the password value FROM the variable (RHS use). The
    # only legitimate references are `wifi_password = ...substr(...)` (LHS
    # assignment), `wifi_password.length()`, and `AddSsid(..., wifi_password)`.
    assert re.search(r"=\s*wifi_password\b", src) is None, "password copied into an alias"


# ---------------------------------------------------------------------------
# AFSK5: the decoded payload's raw bytes are never materialized (only parsed
#        via find/substr/length), defeating "copy the payload then log it".
# ---------------------------------------------------------------------------
def test_afsk5_decoded_payload_chars_never_extracted():
    src = _strip_comments(read(AFSK_SRC))

    assert "decoded_text->c_str()" not in src
    assert "decoded_text->data()" not in src
    # No dereference of the optional payload (e.g. `*data_buffer.decoded_text`).
    assert re.search(r"\*\s*(?:data_buffer\.)?decoded_text\b", src) is None


# ---------------------------------------------------------------------------
# AFSK6: the password IS logged as a redacted length (positive assertion).
# ---------------------------------------------------------------------------
def test_afsk6_password_is_redacted_to_length_only():
    src = read(AFSK_SRC)

    assert re.search(r"Password:\s*\[redacted len=%u\]", src)
    assert "(unsigned)wifi_password.length()" in src


# ---------------------------------------------------------------------------
# AFSK7: the received payload is logged as a byte count, not the text.
# ---------------------------------------------------------------------------
def test_afsk7_received_payload_logged_as_byte_count_only():
    src = read(AFSK_SRC)

    assert "Received credential payload" in src
    assert "(unsigned)data_buffer.decoded_text->length()" in src


# ---------------------------------------------------------------------------
# AFSK8: the LCD shows only the non-secret SSID.
# ---------------------------------------------------------------------------
def test_afsk8_lcd_shows_only_non_secret_ssid():
    src = read(AFSK_SRC)

    assert 'display->SetChatMessage("system", wifi_ssid.c_str())' in src


# ---------------------------------------------------------------------------
# AFSK9: HARNESS SELF-TEST. The statement scanner must collapse multi-line
#        calls so a continuation-line leak is caught — the precise blind spot a
#        line-oriented scanner had. Both a multi-line payload dump and a
#        multi-line password dump must be detected.
# ---------------------------------------------------------------------------
def test_afsk9_scanner_detects_multiline_reintroduction():
    synthetic = (
        "void f() {\n"
        '    ESP_LOGI(kLogTag, "raw=%s",\n'
        "             data_buffer.decoded_text->c_str());\n"
        '    ESP_LOGI(kLogTag, "pw=%s",\n'
        "             wifi_password.c_str());\n"
        '    display->SetStatus(wifi_password.data());\n'
        "}\n"
    )
    sinks = _sink_statements(synthetic)

    pw_offenders = _sink_value_offenders(sinks, _PASSWORD_VAR, _PASSWORD_LEN)
    payload_offenders = _sink_value_offenders(sinks, _PAYLOAD_VAR, _PAYLOAD_LEN)

    # password leaked via multi-line ESP_LOGI AND via SetStatus(.data()).
    assert len(pw_offenders) == 2, pw_offenders
    # full payload leaked via the multi-line ESP_LOGI continuation line.
    assert len(payload_offenders) == 1, payload_offenders
