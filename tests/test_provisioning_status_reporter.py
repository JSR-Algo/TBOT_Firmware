"""Source-level regression tests locking US-005 provisioning-status-REPORTER invariants.

These are static assertions over the firmware .cc/.h text (no device needed),
following the convention in tests/test_blufi_provisioning_stability.py:
module-level ROOT, a read(path) helper, and test_* functions asserting on
substrings / regex / relative ordering of real markers in the source.

Scope: main/provisioning/provisioning_status_reporter.cc (+ its .h contract).
The reporter is the single firmware surface that POSTs the bootstrap token
(Bearer credential) and, on a device_authenticated report, the provisioning
code (inside the request body). It is therefore the highest-leakage spot in the
US-005 lane: a single stray %s would put a credential into the device log.

Each test is tagged with its invariant id (RPT1..RPTn) so a failure is
traceable back to the audited invariant.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

REPORTER = "main/provisioning/provisioning_status_reporter.cc"
HEADER = "main/provisioning/provisioning_status_reporter.h"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _report_fn_body(src: str) -> str:
    """The body of ProvisioningStatusReporter::Report (the only function)."""
    start = src.index("bool ProvisioningStatusReporter::Report(")
    return src[start:]


def _esp_log_statements(src: str) -> list:
    """All ESP_LOG* statements, collapsed across physical lines to ');'.

    ESP_LOG calls in this file wrap across multiple physical lines, so a
    line-by-line scan can miss a credential argument sitting on a later line.
    """
    return re.findall(r"ESP_LOG\w*\((?:[^;]*?)\);", src, re.DOTALL)


# ---------------------------------------------------------------------------
# RPT1: device_authenticated body carries status="device_authenticated" AND
#       the provisioning "code" field. (Backend matches the code; an absent
#       code means the claim can never confirm.)
# ---------------------------------------------------------------------------
def test_rpt1_device_authenticated_body_has_status_and_code():
    src = read(REPORTER)
    body = _report_fn_body(src)

    auth_idx = body.index("Status::DeviceAuthenticated")

    # The device_authenticated branch sets status="device_authenticated".
    assert 'cJSON_AddStringToObject(root, "status", "device_authenticated");' in body
    status_idx = body.index(
        'cJSON_AddStringToObject(root, "status", "device_authenticated");'
    )

    # ...and it adds a "code" field built from the `code` argument.
    assert 'cJSON_AddStringToObject(root, "code", code.c_str());' in body
    code_idx = body.index('cJSON_AddStringToObject(root, "code", code.c_str());')

    # Both live inside the DeviceAuthenticated branch (after the branch test,
    # status before code, code immediately after status).
    assert auth_idx < status_idx < code_idx

    # The code field belongs ONLY to the authenticated branch: the failed branch
    # (which starts at the else) must not also emit a "code" object.
    else_idx = body.index("} else {", auth_idx)
    assert code_idx < else_idx, "code must be added in the DeviceAuthenticated branch"
    failed_branch = body[else_idx:body.index("char* raw", else_idx)]
    assert '"code"' not in failed_branch, (
        "the failed branch must not emit a provisioning code"
    )


# ---------------------------------------------------------------------------
# RPT2: failed body carries status="failed" and a "reason"; the reason defaults
#       to "wifi_connect_failed" when the caller passes an empty reason.
# ---------------------------------------------------------------------------
def test_rpt2_failed_body_has_status_and_default_reason():
    src = read(REPORTER)
    body = _report_fn_body(src)

    else_idx = body.index("} else {")
    failed_branch = body[else_idx:body.index("char* raw", else_idx)]

    # Failed branch sets status="failed".
    assert 'cJSON_AddStringToObject(root, "status", "failed");' in failed_branch

    # The default-reason fallback is exactly "wifi_connect_failed" and is applied
    # only when the supplied reason is empty (ternary on reason.empty()).
    assert re.search(
        r'reason\.empty\(\)\s*\?\s*"wifi_connect_failed"\s*:\s*reason',
        failed_branch,
    ), "failed branch must default the reason to wifi_connect_failed when empty"

    # The resolved reason is the value put into the body (not the literal only).
    assert 'cJSON_AddStringToObject(root, "reason", r.c_str());' in failed_branch


# ---------------------------------------------------------------------------
# RPT3: the Authorization: Bearer <token> header is set on the request, but the
#       token value is NEVER passed to any logger.
# ---------------------------------------------------------------------------
def test_rpt3_authorization_bearer_set_but_token_never_logged():
    src = read(REPORTER)
    body = _report_fn_body(src)

    # The Bearer header IS set from the token (functional requirement).
    assert 'http->SetHeader("Authorization", "Bearer " + token);' in body

    # The token must NEVER be formatted into a log line. Check both the raw text
    # and every collapsed ESP_LOG statement (calls span multiple lines).
    assert "token.c_str()" not in src
    for stmt in _esp_log_statements(src):
        assert "token.c_str()" not in stmt, f"token leaked in log: {stmt!r}"
        assert "Bearer" not in stmt, f"Authorization value leaked in log: {stmt!r}"
        # No bare `token` argument either (e.g. an overload taking std::string).
        assert not re.search(r"[,(]\s*token\s*[,)]", stmt), (
            f"token passed as a log argument: {stmt!r}"
        )


# ---------------------------------------------------------------------------
# RPT4: no partial-token logging tricks. A "log just the first 8 chars" prefix
#       is still a credential leak; ban token_prefix and the %.8s formatter.
# ---------------------------------------------------------------------------
def test_rpt4_no_partial_token_logging():
    src = read(REPORTER)
    assert "token_prefix" not in src
    assert "%.8s" not in src
    # Any width-bounded %s applied to the token would also be a partial leak.
    for stmt in _esp_log_statements(src):
        assert "token.substr" not in stmt
        assert "token.front" not in stmt


# ---------------------------------------------------------------------------
# RPT5: the per-attempt "Reporting attempt=" diagnostic logs only non-secret
#       routing fields plus body_len — never the request body itself (which
#       carries the provisioning code on a device_authenticated report).
# ---------------------------------------------------------------------------
def test_rpt5_reporting_attempt_log_uses_body_len_only():
    src = read(REPORTER)

    # The diagnostic exists and reports a body LENGTH, not the body bytes.
    attempt_log = next(
        (s for s in _esp_log_statements(src) if "Reporting attempt=" in s), None
    )
    assert attempt_log is not None, '"Reporting attempt=" diagnostic not found'

    # It logs body_len ...
    assert "body_len=" in attempt_log
    assert "body.size()" in attempt_log
    # ... and must NOT format the body contents.
    assert "body.c_str()" not in attempt_log, "request body leaked into attempt log"
    assert "%.8s" not in attempt_log
    # Defense-in-depth at file scope: the request body string is never c_str()'d
    # into any log line anywhere in the reporter.
    assert "body.c_str()" not in src


# ---------------------------------------------------------------------------
# RPT6: the non-2xx failure log reports resp_len only — never the response body
#       bytes (a misbehaving backend could reflect the submitted request body,
#       and thus the provisioning code, in its error response).
# ---------------------------------------------------------------------------
def test_rpt6_non_2xx_log_uses_resp_len_only():
    src = read(REPORTER)

    fail_log = next(
        (s for s in _esp_log_statements(src) if "report failed" in s and "HTTP" in s),
        None,
    )
    assert fail_log is not None, "non-2xx failure log not found"

    # It logs resp_len ...
    assert "resp_len=" in fail_log
    assert "resp_body.size()" in fail_log
    # ... and must NOT log the response body verbatim.
    assert "resp_body.c_str()" not in fail_log, "response body leaked into failure log"

    # File scope: the response body is never c_str()'d into a log anywhere.
    assert "resp_body.c_str()" not in src
    for stmt in _esp_log_statements(src):
        assert "resp_body.c_str()" not in stmt


# ---------------------------------------------------------------------------
# RPT7: the retry loop runs exactly kMaxAttempts times, kMaxAttempts == 3, and
#       the back-off table has one delay per inter-attempt gap (kMaxAttempts-1).
# ---------------------------------------------------------------------------
def test_rpt7_retry_count_equals_kmaxattempts():
    src = read(REPORTER)

    # kMaxAttempts is 3 (matches the .h contract: "3 attempts").
    m = re.search(r"constexpr\s+int\s+kMaxAttempts\s*=\s*(\d+)\s*;", src)
    assert m is not None, "kMaxAttempts declaration not found"
    assert int(m.group(1)) == 3

    # The loop is bounded by kMaxAttempts.
    assert re.search(
        r"for\s*\(\s*int\s+attempt\s*=\s*0\s*;\s*attempt\s*<\s*kMaxAttempts\s*;",
        src,
    ), "the report loop must be bounded by kMaxAttempts"

    # The back-off table must be large enough to index safely. The loop reads
    # kRetryDelaysMs[attempt - 1] for attempt in [1, kMaxAttempts), so the
    # largest index accessed is kMaxAttempts-2; the table must therefore hold at
    # least kMaxAttempts-1 entries or the back-off read goes out of bounds.
    table = re.search(r"kRetryDelaysMs\[\]\s*=\s*\{([^}]*)\}", src)
    assert table is not None, "kRetryDelaysMs table not found"
    delays = [d.strip() for d in table.group(1).split(",") if d.strip()]
    assert len(delays) >= int(m.group(1)) - 1, (
        "the back-off table must have at least kMaxAttempts-1 entries so the "
        "kRetryDelaysMs[attempt-1] read never goes out of bounds"
    )
    # Exponential back-off shape across the entries that are actually used
    # (attempt-1 for attempt in [1, kMaxAttempts) => the first kMaxAttempts-1):
    # each used delay is strictly larger than the previous one.
    used = [int(d) for d in delays[: int(m.group(1)) - 1]]
    assert used == sorted(used) and len(set(used)) == len(used), (
        "the used back-off delays must be strictly increasing (exponential)"
    )

    # The pre-attempt delay is gated on attempt > 0 (no delay before attempt 1)
    # and indexes the table at attempt-1 (so it never reads out of bounds).
    assert "if (attempt > 0)" in src
    assert "kRetryDelaysMs[attempt - 1]" in src

    # The exhaustion log names kMaxAttempts (not a hardcoded number).
    assert 'ESP_LOGE(TAG, "All %d provisioning report attempts failed", kMaxAttempts);' in src


def test_rpt8_authenticated_response_is_persisted_before_report_success():
    src = read(REPORTER)
    body = _report_fn_body(src)

    assert 'resp_body = http->ReadAll();' in body
    persist_idx = body.index('PersistProvisioningCredentialResponse(resp_body)')
    success_idx = body.index('return true;', persist_idx)
    assert persist_idx < success_idx


# ---------------------------------------------------------------------------
# RPT8: Report() returns true ONLY on a 2xx status code. Every other exit
#       (empty token, retries exhausted) returns false; non-2xx never returns
#       true, and the 2xx success path is the only `return true` after the
#       request is sent.
# ---------------------------------------------------------------------------
def test_rpt8_returns_true_only_on_2xx():
    src = read(REPORTER)
    body = _report_fn_body(src)

    # The success guard is an explicit 2xx range check.
    assert "status_code >= 200 && status_code < 300" in body

    # The 2xx branch is the success exit.
    success_guard = body.index("if (status_code >= 200 && status_code < 300) {")
    success_return = body.index("return true;", success_guard)
    # The success `return true` lives inside the 2xx guard block.
    guard_block_end = body.index("}", success_return)
    assert success_guard < success_return < guard_block_end

    # The empty-token guard returns false (cannot report without a credential).
    assert "Bootstrap token is empty" in body
    token_guard = body.index("Bootstrap token is empty")
    assert body.index("return false;", token_guard) < body.index("cJSON_CreateObject")

    # The terminal (all-attempts-failed) exit returns false.
    exhausted = body.index("All %d provisioning report attempts failed")
    assert "return false;" in body[exhausted:], (
        "the exhausted-retries path must return false"
    )

    # There is exactly ONE `return true;` that is reachable after the POST: the
    # 2xx branch. (Counting guards against a stray success return on a non-2xx
    # path.) Note the build-disabled early return true at the top is excluded by
    # scoping to the loop region.
    loop_idx = body.index("for (int attempt")
    loop_region = body[loop_idx:]
    assert loop_region.count("return true;") == 1, (
        "exactly one return true must be reachable inside the retry loop (2xx only)"
    )

    # A non-2xx status reads the response body and falls through to retry/fail —
    # it never returns true. The failure-log/will-retry marker sits AFTER the
    # success return, confirming non-2xx does not short-circuit to success.
    fail_marker = body.index("Provisioning status report failed")
    assert success_return < fail_marker, (
        "the success return must precede the non-2xx retry path"
    )


# ---------------------------------------------------------------------------
# RPT9: build-time disable short-circuits the whole function (returns true and
#       NEVER constructs the credential-bearing body / opens an HTTP request)
#       when CONFIG_TBOT_PROVISIONING_REPORT_ENABLED is undefined.
# ---------------------------------------------------------------------------
def test_rpt9_build_disabled_short_circuits_before_any_body_or_post():
    src = read(REPORTER)
    body = _report_fn_body(src)

    disable_idx = body.index("#ifndef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED")
    disable_return = body.index("return true;", disable_idx)
    endif_idx = body.index("#endif", disable_idx)

    # The early return true is inside the #ifndef guard, before the #endif.
    assert disable_idx < disable_return < endif_idx

    # And it short-circuits BEFORE the body is ever built or any HTTP opened, so
    # a disabled build cannot leak or transmit credentials.
    assert disable_return < body.index("cJSON_CreateObject")
    assert disable_return < body.index('http->Open("POST"')


# ---------------------------------------------------------------------------
# RPT10: the request body is COPIED per attempt before SetContent(std::move),
#        so a retry re-sends the same payload (a moved-from body would send an
#        empty/garbage payload on attempt 2+ and the backend would reject it).
# ---------------------------------------------------------------------------
def test_rpt10_body_copied_per_attempt_for_safe_retry():
    src = read(REPORTER)
    body = _report_fn_body(src)

    loop_idx = body.index("for (int attempt")
    loop_region = body[loop_idx:]

    # A fresh copy of the immutable body is made inside the loop ...
    assert "std::string body_copy = body;" in loop_region
    copy_idx = loop_region.index("std::string body_copy = body;")
    # ... and only the COPY is moved into SetContent (the source `body` survives).
    assert "http->SetContent(std::move(body_copy));" in loop_region
    move_idx = loop_region.index("http->SetContent(std::move(body_copy));")
    assert copy_idx < move_idx

    # The original body is never itself moved (would break retry).
    assert "std::move(body)" not in src


# ---------------------------------------------------------------------------
# RPT11: the credential surfaces (Wi-Fi password, device secret, bearer) never
#        appear in this file's logs. The reporter never even handles the Wi-Fi
#        password, so a password token in a log here would be a regression.
# ---------------------------------------------------------------------------
def test_rpt11_no_credential_tokens_in_any_log():
    src = read(REPORTER)

    banned_substrings = (
        "password",
        "passwd",
        "secret",
        "bearer",
        "authorization",
    )
    for stmt in _esp_log_statements(src):
        low = stmt.lower()
        for banned in banned_substrings:
            assert banned not in low, (
                f"log statement references credential surface {banned!r}: {stmt!r}"
            )


# ---------------------------------------------------------------------------
# RPT12: cJSON lifetime — every allocated root/print buffer is freed on the
#        body-building path (no leak that would, over retries from callers,
#        exhaust heap on a memory-tight device).
# ---------------------------------------------------------------------------
def test_rpt12_cjson_buffers_freed():
    src = read(REPORTER)
    body = _report_fn_body(src)

    # The print buffer is freed and the tree deleted right after the body string
    # is captured (before the request loop).
    create_idx = body.index("cJSON_CreateObject")
    free_idx = body.index("cJSON_free(raw);")
    delete_idx = body.index("cJSON_Delete(root);")
    loop_idx = body.index("for (int attempt")
    assert create_idx < free_idx < loop_idx
    assert create_idx < delete_idx < loop_idx


# ---------------------------------------------------------------------------
# RPT13: the .h contract the reporter implements stays in sync with the .cc:
#        3 retry attempts, Bearer auth, HTTP client id 2, and the documented
#        body shapes (status + code / status + reason).
# ---------------------------------------------------------------------------
def test_rpt13_header_contract_matches_implementation():
    hdr = read(HEADER)
    src = read(REPORTER)

    # Header documents 3 attempts; implementation uses kMaxAttempts == 3.
    assert "3 attempts" in hdr
    assert re.search(r"kMaxAttempts\s*=\s*3\s*;", src)

    # Header documents Bearer auth; implementation sets the Bearer header.
    assert "Bearer" in hdr
    assert '"Bearer " + token' in src

    # Header documents HTTP client id 2; implementation creates client 2.
    assert "client id 2" in hdr
    assert "network->CreateHttp(2)" in src

    # Header documents both body shapes; implementation emits both.
    assert '"status":"device_authenticated"' in hdr
    assert '"status":"failed"' in hdr
    assert '"device_authenticated"' in src
    assert '"failed"' in src

    # Header's Status enum matches what the implementation branches on.
    assert "DeviceAuthenticated" in hdr and "Status::DeviceAuthenticated" in src
    assert "Failed" in hdr
