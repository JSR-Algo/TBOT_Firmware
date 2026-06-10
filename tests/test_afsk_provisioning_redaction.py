"""US-005 source-level regression suite: the audio-FSK (AFSK) Wi-Fi
provisioning path NEVER leaks the raw Wi-Fi password value.

Scope: main/boards/common/afsk_demod.cc only. This is the legacy
"shout-the-credentials-over-the-speaker" provisioning channel. The device
demodulates an audio-FSK payload of the form "SSID\\npassword" and persists it
via SsidManager. The TBOT security policy is:

  * The Wi-Fi PASSWORD is a secret. Its VALUE must never reach any output sink
    (ESP serial log via ESP_LOG*, the printf family, std::cout, or the robot
    LCD via display->Set*/Show*). It may appear at a sink ONLY as a redacted
    length (e.g. "[redacted len=%u]").
  * The full decoded payload ("SSID\\npassword") is secret-bearing too (it
    contains the password); only its byte count may be logged.
  * The SSID is NOT a secret and MAY appear (it is shown on the LCD on purpose).
  * Redaction must not break the real handoff: the unredacted password VALUE
    must still flow into SsidManager::AddSsid so Wi-Fi actually connects.

These are static assertions over the firmware .cc text (no device needed),
following the python convention in tests/test_afsk_credential_redaction.py,
tests/test_blufi_provisioning_stability.py (FW8) and
tests/test_wifi_provisioning_brand.py: a module-level ROOT, a read(path)
helper, and test_* functions asserting on substrings / regex / structure.

CRITICAL HARNESS PROPERTY (why this file is not line-oriented): a single log
call in this file spans multiple physical lines, with the secret-bearing
argument on a CONTINUATION line that contains no "ESP_LOG" token:

    ESP_LOGI(kLogTag, "WiFi SSID: %s, Password: [redacted len=%u]",
             wifi_ssid.c_str(), (unsigned)wifi_password.length());

A grep/line scan of `"ESP_LOG" and "password"` on the SAME line is BLIND to a
re-introduced leak whose value sits on the continuation line:

    ESP_LOGI(kLogTag, "pw=%s",
             wifi_password.c_str());   // <- line has no ESP_LOG token

So every value-leak check operates on whole logical STATEMENTS, collapsed by
_statements(). test_apr9_* and test_apr10_* are harness self-tests that PROVE
the scanner catches such multi-line re-introductions (the RED in this file's
TDD story): if the collapsing logic regresses to line-oriented, those
self-tests fail loudly instead of the real-source checks silently passing.

Each test is tagged with its invariant id (APR1..APR12). "APR" = Afsk
PRovisioning, to avoid colliding with AFSK* ids in the sibling suite.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

AFSK_SRC = "main/boards/common/afsk_demod.cc"

# ---------------------------------------------------------------------------
# Output sinks that reach the serial console / UART / LCD.
#   * ESP_LOG\w* covers LOGE/LOGW/LOGI/LOGD/LOGV — an error/warn log leaks just
#     as much as an info log, so all levels count.
#   * the printf family + std::cout cover any non-ESP console write.
#   * ->Set\w*( / ->Show\w*( cover the Display API (SetChatMessage, SetStatus,
#     SetEmotion, ShowNotification, ...) — anything rendered on the robot LCD.
# ---------------------------------------------------------------------------
_SINK_RE = re.compile(
    r"\bESP_LOG\w*\s*\("
    r"|\b(?:printf|fprintf|snprintf|sprintf|vprintf|vfprintf|vsnprintf"
    r"|puts|fputs|fwrite)\s*\("
    r"|\bstd::cout\b"
    r"|\bstd::cerr\b"
    r"|->Set\w*\s*\("
    r"|->Show\w*\s*\("
)

# The attacker-supplied secret expressions. Each may appear at a sink ONLY as a
# redacted length; every other shape at a sink is a leak.
_PASSWORD_VAR = "wifi_password"
_PASSWORD_LEN = "wifi_password.length()"
_PAYLOAD_VAR = "decoded_text"            # data_buffer.decoded_text-> ... ("SSID\npassword")
_PAYLOAD_LEN = "decoded_text->length()"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _strip_comments(source: str) -> str:
    """Remove C/C++ comments so commented examples and the literal word
    'password' / 'Password: %s' inside comments never pollute the scan."""
    no_block = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", no_block)


def _statements(source: str):
    """Collapse `source` into logical C++ statements, robust to multi-line calls.

    Order matters:
      1. strip comments WHILE newlines still exist (so // terminates correctly);
      2. blank the CONTENTS of string literals (so a ';' '{' '}' inside a
         format string can't mis-split a statement, and so format-string text
         like "Password: %s" can never match a variable token);
      3. collapse whitespace (joins continuation lines into one fragment);
      4. split on statement / block boundaries (; { }).

    A multi-line `ESP_LOGI(tag,\\n  fmt,\\n  secret);` thus becomes ONE fragment
    whose argument expressions are all visible to the substring checks, while an
    `if (...has_value()) {` header is isolated into its own (non-sink) fragment.
    """
    no_comments = _strip_comments(source)
    blanked = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', no_comments)
    collapsed = re.sub(r"\s+", " ", blanked)
    return [s.strip() for s in re.split(r"[;{}]", collapsed) if s.strip()]


def _sink_statements(source: str):
    return [s for s in _statements(source) if _SINK_RE.search(s)]


def _sink_value_offenders(statements, var_token: str, length_form: str):
    """Sink statements where `var_token` reaches the sink as anything other than
    its permitted redacted `length_form`. The length form is removed first so
    that a legitimate `(unsigned)wifi_password.length()` does not count as a
    leak, but a bare `wifi_password.c_str()` does."""
    offenders = []
    for stmt in statements:
        residual = stmt.replace(length_form, "")
        if var_token in residual:
            offenders.append(stmt)
    return offenders


# ===========================================================================
# APR1: sanity — the audited source exists and contains the AFSK provisioning
#       entry point we are reasoning about. If this file is renamed/moved, the
#       whole suite must fail loudly rather than vacuously pass on empty text.
# ===========================================================================
def test_apr1_afsk_provisioning_source_present():
    src = read(AFSK_SRC)
    assert "ReceiveWifiCredentialsFromAudio" in src
    assert "namespace audio_wifi_config" in src
    # The decoded payload optional is the thing we are guarding around.
    assert "decoded_text" in src


# ===========================================================================
# APR2: the raw PASSWORD value never reaches ANY sink, in any shape, across
#       multi-line statements. Length-only is permitted. (Core invariant.)
# ===========================================================================
def test_apr2_password_value_never_reaches_a_sink():
    offenders = _sink_value_offenders(
        _sink_statements(read(AFSK_SRC)), _PASSWORD_VAR, _PASSWORD_LEN
    )
    assert offenders == [], f"password value reaches a sink: {offenders}"


# ===========================================================================
# APR3: the full decoded "SSID\npassword" PAYLOAD never reaches ANY sink, in
#       any shape. Only its ->length() (byte count) may be logged.
# ===========================================================================
def test_apr3_decoded_payload_value_never_reaches_a_sink():
    offenders = _sink_value_offenders(
        _sink_statements(read(AFSK_SRC)), _PAYLOAD_VAR, _PAYLOAD_LEN
    )
    assert offenders == [], f"decoded payload value reaches a sink: {offenders}"


# ===========================================================================
# APR4: the explicit historical leak shapes are gone. These are the exact
#       cleartext-credential log/display patterns this hardening removed; if any
#       reappears verbatim it is an unambiguous regression.
# ===========================================================================
def test_apr4_historical_cleartext_leak_shapes_absent():
    src = read(AFSK_SRC)
    # raw "...Password: %s", ..., wifi_password value log
    assert 'Password: %s"' not in src
    # full decoded-text dump
    assert 'Received text data: %s"' not in src
    # LCD dump of the raw decoded payload
    assert 'SetChatMessage("system", data_buffer.decoded_text->c_str())' not in src


# ===========================================================================
# APR5: the password value is never materialized via a raw-buffer accessor or
#       copied into an alias (defeats "copy to a temp, log the temp" and the
#       .c_str()/.data() bypass). The ONLY legal references are the LHS split
#       assignment, the .length() redaction, and the AddSsid handoff.
# ===========================================================================
def test_apr5_password_value_never_extracted_or_aliased():
    src = _strip_comments(read(AFSK_SRC))

    assert "wifi_password.c_str()" not in src, "raw c_str accessor on password"
    assert "wifi_password.data()" not in src, "raw data() accessor on password"

    # No statement reads the password into another variable
    # (e.g. `auto pw = wifi_password;` or `std::string x = wifi_password;`).
    assert re.search(r"=\s*wifi_password\b", src) is None, "password copied into an alias"

    # No string concatenation that splices the password value into a message.
    assert re.search(r'(?:"\s*\+\s*wifi_password|wifi_password\s*\+)', src) is None, \
        "password concatenated into a string"


# ===========================================================================
# APR6: the decoded payload's raw bytes are never materialized (only parsed via
#       find/substr/length), defeating "copy the payload, then log it" and any
#       dereference of the optional (`*data_buffer.decoded_text`).
# ===========================================================================
def test_apr6_decoded_payload_chars_never_extracted():
    src = _strip_comments(read(AFSK_SRC))

    assert "decoded_text->c_str()" not in src
    assert "decoded_text->data()" not in src
    assert re.search(r"\*\s*(?:data_buffer\.)?decoded_text\b", src) is None, \
        "optional payload dereferenced (potential raw dump)"


# ===========================================================================
# APR7: POSITIVE — the password IS logged, but as a redacted length only. This
#       proves the redaction exists (not merely that the leak is absent: a file
#       that logs nothing would pass APR2 vacuously; this asserts the intended,
#       safe disclosure shape is actually present).
# ===========================================================================
def test_apr7_password_logged_as_redacted_length_only():
    src = read(AFSK_SRC)
    assert re.search(r"Password:\s*\[redacted len=%u\]", src), \
        "expected a length-only redaction marker for the password"
    assert "(unsigned)wifi_password.length()" in src, \
        "the redacted length must be the real password length"


# ===========================================================================
# APR8: POSITIVE — the received payload is logged as a byte COUNT, never the
#       text. Same anti-vacuity guard for the payload sink.
# ===========================================================================
def test_apr8_payload_logged_as_byte_count_only():
    src = read(AFSK_SRC)
    assert "Received credential payload" in src
    assert "(unsigned)data_buffer.decoded_text->length()" in src


# ===========================================================================
# APR9: SSID is NOT a secret and IS allowed at a sink. This asserts the policy
#       boundary is drawn at the RIGHT place — the LCD shows the SSID on
#       purpose. (If a future "redact everything" overcorrection blanked the
#       SSID display, this would catch it; conversely it documents that an
#       SSID at a sink is intentional, not an oversight.)
# ===========================================================================
def test_apr9_ssid_is_allowed_at_a_sink():
    src = read(AFSK_SRC)
    # The SSID is rendered to the LCD via its c_str() — explicitly allowed.
    assert 'display->SetChatMessage("system", wifi_ssid.c_str())' in src
    # And the SSID appears in the same log line as the redacted password.
    assert re.search(r"WiFi SSID:\s*%s.*Password:\s*\[redacted", read(AFSK_SRC))


# ===========================================================================
# APR10: redaction must NOT break the handoff — the UNREDACTED password value
#        still flows into SsidManager so Wi-Fi can actually connect. A redaction
#        that also dropped the value from AddSsid would be a silent functional
#        regression (device "provisions" but never connects). This asserts the
#        secret is consumed by the persistence API while NEVER touching a sink.
# ===========================================================================
def test_apr10_password_value_still_handed_to_ssid_manager():
    src = _strip_comments(read(AFSK_SRC))
    # The split derives both fields from the decoded payload around the newline.
    assert "wifi_ssid = data_buffer.decoded_text->substr(0, newline_position)" in src
    assert "wifi_password = data_buffer.decoded_text->substr(newline_position + 1)" in src
    # The real (unredacted) password value is persisted, not a redacted form.
    m = re.search(r"AddSsid\(\s*([^,]+?)\s*,\s*([^)]+?)\s*\)", src)
    assert m is not None, "AddSsid handoff not found"
    ssid_arg, pw_arg = m.group(1).strip(), m.group(2).strip()
    assert ssid_arg == "wifi_ssid"
    assert pw_arg == "wifi_password", "password must be persisted as its real value"
    # AddSsid is the SsidManager persistence call, not a sink.
    assert not _SINK_RE.search("ssid_manager.AddSsid(wifi_ssid, wifi_password)")


# ===========================================================================
# APR11: lifecycle — on a successful decode the optional payload is RESET (the
#        secret-bearing blob is cleared from the buffer after use) and the
#        function returns. This bounds how long the decoded password lingers in
#        memory and prevents re-processing a stale secret.
# ===========================================================================
def test_apr11_decoded_payload_reset_after_use():
    src = _strip_comments(read(AFSK_SRC))
    assert "data_buffer.decoded_text.reset()" in src, \
        "decoded payload optional must be cleared after a successful decode"
    # The success path persists, exits config AP, then returns.
    success_block = src[src.index("AddSsid"):]
    assert "StopConfigAp()" in success_block
    assert "return;" in success_block


# ===========================================================================
# APR12: every sink statement in the file that references either secret token
#        does so ONLY in its redacted form. This is the EXHAUSTIVE form of
#        APR2/APR3: instead of trusting the offender list, it enumerates every
#        sink touching a secret and asserts the redaction marker is present in
#        that exact statement — so a NEW sink call that happens to also contain
#        the secret value cannot slip through.
# ===========================================================================
def test_apr12_every_secret_bearing_sink_is_redacted():
    sinks = _sink_statements(read(AFSK_SRC))
    secret_sinks = [s for s in sinks if (_PASSWORD_VAR in s or _PAYLOAD_VAR in s)]
    # There is at least one sink that talks about the secrets (the redaction log
    # lines) — otherwise the positive APR7/APR8 markers would be unreachable.
    assert secret_sinks, "expected the redacted log lines among the sinks"
    for stmt in secret_sinks:
        touches_password = _PASSWORD_VAR in stmt
        touches_payload = _PAYLOAD_VAR in stmt
        if touches_password:
            assert _PASSWORD_LEN in stmt, f"password sink not length-redacted: {stmt}"
        if touches_payload:
            assert _PAYLOAD_LEN in stmt, f"payload sink not count-redacted: {stmt}"


# ===========================================================================
# HARNESS SELF-TESTS (the RED proof). These do NOT read the real source; they
# feed the scanner synthetic leaks and assert it CATCHES them. If the statement
# collapser ever regresses to line-oriented, these fail — guaranteeing the
# green of APR2/APR3/APR12 above is real coverage, not a blind spot.
# ===========================================================================
def test_aprh1_scanner_catches_multiline_password_and_payload_leaks():
    synthetic = (
        "void f() {\n"
        '    ESP_LOGI(kLogTag, "raw=%s",\n'
        "             data_buffer.decoded_text->c_str());\n"   # payload leak, continuation line
        '    ESP_LOGI(kLogTag, "pw=%s",\n'
        "             wifi_password.c_str());\n"                # password leak, continuation line
        "    display->SetStatus(wifi_password.data());\n"      # password leak via LCD
        "    printf(\"%s\", wifi_password.c_str());\n"          # password leak via printf
        "}\n"
    )
    sinks = _sink_statements(synthetic)
    pw_offenders = _sink_value_offenders(sinks, _PASSWORD_VAR, _PASSWORD_LEN)
    payload_offenders = _sink_value_offenders(sinks, _PAYLOAD_VAR, _PAYLOAD_LEN)

    # password leaked 3 ways (multi-line ESP_LOGI, SetStatus(.data()), printf).
    assert len(pw_offenders) == 3, pw_offenders
    # full payload leaked once (the multi-line ESP_LOGI continuation line).
    assert len(payload_offenders) == 1, payload_offenders


def test_aprh2_scanner_does_not_flag_legitimate_redaction_or_format_text():
    # A safe redaction line and a format string that literally says "Password:"
    # must NOT be flagged: the value is only the .length(), and the word
    # Password is inside a (blanked) string literal.
    synthetic = (
        "void f() {\n"
        '    ESP_LOGI(kLogTag, "WiFi SSID: %s, Password: [redacted len=%u]",\n'
        "             wifi_ssid.c_str(), (unsigned)wifi_password.length());\n"
        '    ESP_LOGI(kLogTag, "Received credential payload (%u bytes)",\n'
        "             (unsigned)data_buffer.decoded_text->length());\n"
        '    display->SetChatMessage("system", wifi_ssid.c_str());\n'  # SSID allowed
        "}\n"
    )
    sinks = _sink_statements(synthetic)
    assert _sink_value_offenders(sinks, _PASSWORD_VAR, _PASSWORD_LEN) == []
    assert _sink_value_offenders(sinks, _PAYLOAD_VAR, _PAYLOAD_LEN) == []


def test_aprh3_string_literal_blanking_prevents_false_positive_on_format_text():
    # The literal token "wifi_password" appearing INSIDE a format string must not
    # be mistaken for a value reference once literals are blanked.
    synthetic = 'ESP_LOGI(kLogTag, "field name is wifi_password but value is %u", n);'
    sinks = _sink_statements(synthetic)
    assert _sink_value_offenders(sinks, _PASSWORD_VAR, _PASSWORD_LEN) == [], \
        "format-string mention of the token must not be a false leak"
