"""Source-level regression tests locking the WebSocket/OTA bearer-token
redaction invariant.

The WS/OTA auth token is an RS256 JWT (header.payload.signature). A prior
revision logged the substring AFTER THE LAST '.', which is the SIGNATURE
segment of the live bearer token -- not a timestamp, despite the "ts"/"token_ts"
field names. Two firmware files were affected:

  * main/protocols/websocket_protocol.cc -- TokenTimestampForLog() returned
    token.substr(token.rfind('.') + 1) and an ESP_LOGI emitted it as
    "... token_ts=%s".
  * main/ota.cc -- std::strrchr(item->valuestring, '.') + 1 emitted as "ts=%s".

TBOT policy: no credential, nor ANY segment of one, may reach a log sink. The
only token-derived value permitted in a log on this path is its presence
(empty / non-empty). Isolating the last-dot segment of a token is exactly the
leak primitive, so in these two auth files it must not exist at all -- if the
signature segment is never computed, it can never be logged.

This guards the WebSocket/OTA bearer-auth path, NOT the BLE/BluFi US-005
provisioning path (that path is covered by tests/test_blufi_provisioning_stability.py
FW8/FW9 and tests/test_afsk_credential_redaction.py).

These are static assertions over the firmware .cc text (no device needed),
following the convention in tests/test_blufi_provisioning_stability.py and
tests/test_afsk_credential_redaction.py: module-level ROOT, a read(path)
helper, statement-oriented sink scanning robust to multi-line log calls, and
test_* functions tagged with their invariant id (TOK1..TOK7).

Scanning is STATEMENT-oriented, not line-oriented: the websocket auth-identity
ESP_LOGI spans two physical lines (the value arguments sit on a continuation
line that contains no "ESP_LOG" token), and the ota strrchr-based leak split
the computation across two statements. A line-oriented scan is blind to the
continuation line; _statements() collapses logical statements first so the
argument expressions are visible to every check. TOK7 guards this property of
the harness itself against a multi-line reintroduction of both shapes.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

WS_SRC = "main/protocols/websocket_protocol.cc"
OTA_SRC = "main/ota.cc"

# Output sinks that reach the serial console / UART. ESP_LOG* covers
# LOGE/LOGW/LOGI/LOGD/LOGV (errors and warnings are just as visible as info).
_SINK_RE = re.compile(
    r"\bESP_LOG\w*\s*\("
    r"|\b(?:printf|fprintf|snprintf|vprintf|vfprintf|puts|fputs|fwrite)\s*\("
    r"|\bstd::cout\b"
)

# Computing the substring / pointer AFTER THE LAST '.' of a value is precisely
# how the JWT signature segment was isolated for logging. In these two auth
# files that primitive has no legitimate use, so its mere presence -- in any
# shape, anywhere in the file -- is a regression.
_LAST_DOT_EXTRACT_RE = re.compile(
    r"strrchr\s*\([^;]*?,\s*'\.'\s*\)"      # C:   char* to the last '.'  (+1 == signature)
    r"|\.rfind\s*\(\s*'\.'\s*\)"             # C++: index of the last '.' (substr(+1) == signature)
    r"|\.rfind\s*\(\s*\"\.\"\s*\)"
)


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _strip_comments(source: str) -> str:
    """Remove C/C++ comments so commented examples and the literal token names
    inside comments never pollute the scan."""
    no_block = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", no_block)


def _statements(source: str):
    """Logical C++ statements, robust to multi-line calls.

    Comments are stripped first (while newlines still exist, so // works), then
    string-literal contents are blanked (so format-string text like "token_ts=%s"
    cannot match a variable token and any ; { } inside a literal cannot mis-split
    a statement), whitespace is collapsed, and the text is split on
    statement/block boundaries (; { }). A multi-line
        ESP_LOGI(TAG, "...token_empty=%d",
                 device_id.c_str(), client_id.c_str(), token.empty());
    thus becomes a single fragment whose argument expressions are visible to the
    substring checks.
    """
    no_comments = _strip_comments(source)
    blanked = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', no_comments)
    collapsed = re.sub(r"\s+", " ", blanked)
    return [s.strip() for s in re.split(r"[;{}]", collapsed) if s.strip()]


def _sink_statements(source: str):
    return [s for s in _statements(source) if _SINK_RE.search(s)]


# ---------------------------------------------------------------------------
# TOK1: websocket_protocol.cc -- the leaking helper and its log field are gone
#       (documents the exact regression that was fixed).
# ---------------------------------------------------------------------------
def test_tok1_websocket_timestamp_helper_and_field_removed():
    src = read(WS_SRC)

    # The helper that returned token.substr(rfind('.') + 1) must be gone.
    assert "TokenTimestampForLog" not in src
    # The "token_ts=%s" log field (and any token_ts variable) must be gone.
    assert "token_ts" not in src


# ---------------------------------------------------------------------------
# TOK2: websocket_protocol.cc -- the auth-identity log emits token PRESENCE
#       only (positive assertion), and that statement carries no token segment.
# ---------------------------------------------------------------------------
def test_tok2_websocket_logs_token_presence_only():
    src = read(WS_SRC)

    # Presence-only logging is retained.
    assert "token_empty=%d" in src

    # Locate the collapsed auth-identity sink statement and confirm it passes
    # only token.empty() -- never a sliced/extracted token segment.
    auth_stmts = [s for s in _sink_statements(src) if "device-id" in s or "token.empty()" in s]
    assert auth_stmts, "auth-identity log statement not found"
    for stmt in auth_stmts:
        assert "token.empty()" in stmt
        assert ".substr(" not in stmt
        assert _LAST_DOT_EXTRACT_RE.search(stmt) is None


# ---------------------------------------------------------------------------
# TOK3: ota.cc -- the strrchr-based last-dot extraction and its log field gone.
# ---------------------------------------------------------------------------
def test_tok3_ota_strrchr_dot_extraction_removed():
    src = read(OTA_SRC)

    assert "token_ts" not in src
    # The only std::strrchr in this file was the leak primitive.
    assert "strrchr" not in _strip_comments(src)


# ---------------------------------------------------------------------------
# TOK4: ota.cc -- the websocket-token branch logs PRESENCE only (positive),
#       and never the "ts=%s" segment.
# ---------------------------------------------------------------------------
def test_tok4_ota_logs_token_presence_only():
    src = read(OTA_SRC)

    assert "Received websocket token: empty=%d" in src
    assert "ts=%s" not in src

    # The collapsed token-log statement passes only the presence test. The
    # format string is blanked by _statements(), so match on the value arg.
    tok_stmts = [s for s in _sink_statements(src) if "valuestring[0] ==" in s]
    assert tok_stmts, "websocket-token presence log statement not found"
    for stmt in tok_stmts:
        assert ".substr(" not in stmt
        assert _LAST_DOT_EXTRACT_RE.search(stmt) is None


# ---------------------------------------------------------------------------
# TOK5: GENERALIZED -- neither file computes the last-dot token segment in any
#       shape. If the signature segment is never isolated, it cannot be logged.
# ---------------------------------------------------------------------------
def test_tok5_no_last_dot_segment_extraction_in_either_file():
    for path in (WS_SRC, OTA_SRC):
        src = _strip_comments(read(path))
        match = _LAST_DOT_EXTRACT_RE.search(src)
        assert match is None, f"{path}: last-dot token-segment extraction reintroduced: {match.group(0)!r}"


# ---------------------------------------------------------------------------
# TOK6: SINK-ORIENTED -- no ESP_LOG/printf sink in either file slices a value
#       or isolates a last-dot segment (multi-line robust, defense in depth).
# ---------------------------------------------------------------------------
def test_tok6_no_sink_statement_slices_or_extracts_a_segment():
    for path in (WS_SRC, OTA_SRC):
        offenders = [
            s for s in _sink_statements(read(path))
            if ".substr(" in s or _LAST_DOT_EXTRACT_RE.search(s)
        ]
        assert offenders == [], f"{path}: sink emits a sliced/extracted segment: {offenders}"


# ---------------------------------------------------------------------------
# TOK7: HARNESS SELF-TEST. The detector + statement collapser must catch a
#       multi-line reintroduction of BOTH shapes -- the C++ substr(rfind('.')+1)
#       form and the C strrchr(token, '.')+1 form -- the precise blind spots a
#       line-oriented scanner had.
# ---------------------------------------------------------------------------
def test_tok7_scanner_detects_multiline_reintroduction():
    synthetic_cpp = (
        "void f() {\n"
        '    ESP_LOGI(TAG, "token_ts=%s",\n'
        "             token.substr(token.rfind('.') + 1).c_str());\n"
        "}\n"
    )
    synthetic_c = (
        "void g() {\n"
        "    const char* token_ts = std::strrchr(item->valuestring, '.');\n"
        '    ESP_LOGI(TAG, "ts=%s",\n'
        '             token_ts ? token_ts + 1 : "missing");\n'
        "}\n"
    )

    # The last-dot extraction primitive is detected in both shapes.
    assert _LAST_DOT_EXTRACT_RE.search(_strip_comments(synthetic_cpp)) is not None
    assert _LAST_DOT_EXTRACT_RE.search(_strip_comments(synthetic_c)) is not None

    # The sink-statement scan flags the inlined C++ slice on the continuation
    # line (the substr reaches the logger directly).
    cpp_offenders = [
        s for s in _sink_statements(synthetic_cpp)
        if ".substr(" in s or _LAST_DOT_EXTRACT_RE.search(s)
    ]
    assert len(cpp_offenders) == 1, cpp_offenders
