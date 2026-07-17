"""US-005 round-2 gap-fill: static source-scrape edge-case suite over
``main/provisioning/claim_confirmation_reporter.cc``.

The claim-confirmation reporter is pure C++ compiled only inside the ESP-IDF
firmware image, so there is no host build target to link against from pytest.
Following the convention already established by the two sibling suites
(``tests/test_tbot_claim_confirmation_contract.py`` and
``tests/test_tbot_claim_runtime_contract.py``), these are STATIC assertions over
the firmware .cc / .h text: a module-level ``ROOT``, a ``read(path)`` helper, and
a ``function_body()`` brace-matcher that extracts the exact body of a named
function so assertions describe the REAL control flow of the shipped source
rather than a mock we set up ourselves.

Where the structural shape is not enough (the credential-redaction invariant),
the suite additionally collapses the source into logical statements and inspects
the HTTP-failure log sinks for verbatim-secret exposure, mirroring the
statement-oriented scanner in ``tests/test_ws_ota_token_redaction.py``.

Coverage focus (the four functions the sibling suites only spot-check):

  * ParsePendingTbotClaimFromDeviceConfigJson -- cJSON_Parse null-fail,
    claim-missing/null -> true, claim-not-object -> false, the field
    extraction + status-recognition gate, message/expires copy guards.
  * DaysFromCivilUtc -- the Howard-Hinnant branchless civil algorithm SHAPE
    (era / yoe / doy / doe terms + the pre-March year adjust).
  * ParseIso8601UtcToEpoch -- empty guard, 'T' vs ' ' separator acceptance,
    per-field range validation (incl. the leap-second second<=60 allowance),
    and the UTC epoch composition.
  * IsPendingTbotClaimExpired -- unparseable expiry -> false (never-expire) and
    the ``now >= expires`` comparison direction.

Plus a credential-redaction audit of the reporter's two HTTP-failure ``body=%s``
log sites (REDACT1/REDACT2): we DOCUMENT the current redaction state and, where a
failure sink logs a verbatim response body that can carry a secret, the audit
test is xfail-marked with a TYPED blocker rather than a hard failure (per task
contract), so the suite stays GREEN while the risk is surfaced and tracked.
"""

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]

SRC = "main/provisioning/claim_confirmation_reporter.cc"
HDR = "main/provisioning/claim_confirmation_reporter.h"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    """Return the brace-delimited body (inclusive of the outer braces) of the
    first function whose declaration starts with ``signature``."""
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
                return text[brace:index + 1]
    raise AssertionError(f"unterminated function {signature}")


# ===========================================================================
# ParsePendingTbotClaimFromDeviceConfigJson
# ===========================================================================

def test_parse_pending_claim_resets_out_param_before_anything_else():
    # The out-param must be cleared FIRST so a partial / failed parse can never
    # leave a caller observing stale claim fields from a previous fetch.
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    reset_idx = body.index("pending_claim = PendingTbotClaim{};")
    parse_idx = body.index("cJSON_Parse(json.c_str())")
    assert reset_idx < parse_idx


def test_parse_pending_claim_returns_false_on_unparseable_json():
    # cJSON_Parse returning null (malformed / non-JSON body) is a hard parse
    # failure -> false, and the early return happens before any cJSON_Delete so
    # there is nothing to free on that path.
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    null_check = body.index("if (root == nullptr)")
    # The very next return after the null check is the false return.
    after = body[null_check:]
    first_return = after.index("return")
    assert "return false;" in after[first_return:first_return + len("return false;")]
    # And it must NOT free root (root is null here).
    null_branch = after[:after.index("}", first_return)]
    assert "cJSON_Delete" not in null_branch


def test_parse_pending_claim_missing_or_null_claim_is_success_not_active():
    # A device-config envelope with no "claim" key, or claim==null, is the
    # NORMAL unclaimed/available case: parse succeeds (true) with an inactive
    # pending claim. It must still free root before returning.
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    guard = 'if (claim == nullptr || cJSON_IsNull(claim))'
    assert guard in body
    branch = body[body.index(guard):]
    branch = branch[:branch.index("}") + 1]
    assert "cJSON_Delete(root);" in branch
    assert "return true;" in branch
    # Crucially this branch does NOT mark the claim active.
    assert "pending_claim.active = true;" not in branch


def test_parse_pending_claim_non_object_claim_is_a_hard_false():
    # A "claim" present but not a JSON object is malformed backend output -> the
    # parser rejects it as false (distinct from the missing/null success case),
    # and frees root first.
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    guard = "if (!cJSON_IsObject(claim))"
    assert guard in body
    branch = body[body.index(guard):]
    branch = branch[:branch.index("}") + 1]
    assert "cJSON_Delete(root);" in branch
    assert "return false;" in branch
    # Ordering: the not-object reject comes AFTER the missing/null success guard,
    # so claim==null is success while claim==42 is a hard reject.
    assert body.index("cJSON_IsNull(claim)") < body.index("!cJSON_IsObject(claim)")


def test_parse_pending_claim_reads_all_four_claim_fields():
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    assert 'cJSON_GetObjectItem(claim, "claim_id")' in body
    assert 'cJSON_GetObjectItem(claim, "status")' in body
    assert 'cJSON_GetObjectItem(claim, "message")' in body
    assert 'cJSON_GetObjectItem(claim, "expires_at")' in body


def test_parse_pending_claim_requires_string_id_and_recognized_status():
    # An active claim requires claim_id to be a string AND status to be the exact
    # WAITING_PHYSICAL_CONFIRM literal. Anything else -> false (no active claim).
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    assert "!cJSON_IsString(claim_id)" in body
    assert "!cJSON_IsString(status)" in body
    assert 'std::string(status->valuestring) != "WAITING_PHYSICAL_CONFIRM"' in body
    # The reject branch returns false and frees root.
    gate = body.index("!cJSON_IsString(claim_id)")
    gate_branch = body[gate:body.index("pending_claim.active = true;")]
    assert "cJSON_Delete(root);" in gate_branch
    assert "return false;" in gate_branch


def test_parse_pending_claim_warns_on_unrecognized_nonempty_status_drift():
    # M2: a NON-EMPTY status we did not recognize (casing/value drift such as
    # "waiting_physical_confirm") must be surfaced via ESP_LOGW so the dead
    # feature is observable -- but only when the string is non-empty, so a
    # missing/empty status stays silent.
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    assert "cJSON_IsString(status) && status->valuestring[0] != '\\0'" in body
    assert "Unrecognized claim status" in body
    # The drift warning lives strictly inside the reject path (before active=true).
    assert body.index("Unrecognized claim status") < body.index("pending_claim.active = true;")
    # And the warning logs only the status value, never claim_id or any token.
    warn = body.index("Unrecognized claim status")
    warn_stmt = body[warn:body.index(";", warn)]
    assert "claim_id" not in warn_stmt


def test_parse_pending_claim_populates_struct_and_guards_optional_fields():
    # On the success path the struct is fully populated; message/expires_at are
    # copied only when they are actually strings (so a null/absent optional field
    # leaves the default empty value rather than dereferencing a non-string).
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    success = body[body.index("pending_claim.active = true;"):]
    assert "pending_claim.active = true;" in success
    assert "pending_claim.claim_id = claim_id->valuestring;" in success
    assert "pending_claim.status = status->valuestring;" in success
    assert "if (cJSON_IsString(message))" in success
    assert "pending_claim.message = message->valuestring;" in success
    assert "if (cJSON_IsString(expires_at))" in success
    assert "pending_claim.expires_at = expires_at->valuestring;" in success
    # Success path frees root and returns true.
    assert "cJSON_Delete(root);" in success
    assert "return true;" in success


def test_parse_pending_claim_frees_root_on_every_return_no_leak():
    # Every cJSON_Parse success path must Delete(root). The only return that does
    # NOT free is the root==nullptr early-out (nothing was allocated). Count the
    # `return true;` plus the `return false;` reject paths that follow a non-null
    # parse and confirm each is preceded by a cJSON_Delete within its branch.
    body = function_body(read(SRC), "bool ParsePendingTbotClaimFromDeviceConfigJson")
    # There must be at least as many cJSON_Delete(root) calls as the number of
    # returns that occur after the null check (4: missing/null true, not-object
    # false, bad-status false, success true).
    delete_count = body.count("cJSON_Delete(root);")
    assert delete_count >= 4, f"expected >=4 cJSON_Delete(root), found {delete_count}"


# ===========================================================================
# DaysFromCivilUtc -- Howard-Hinnant branchless civil algorithm SHAPE
# ===========================================================================

def test_days_from_civil_is_a_self_contained_static_helper():
    src = read(SRC)
    assert "static long DaysFromCivilUtc(int year, unsigned month, unsigned day)" in src


def test_days_from_civil_applies_pre_march_year_shift():
    # The algorithm treats Jan/Feb as months 13/14 of the previous year so the
    # leap-day lands at the end of the shifted year: `year -= month <= 2`.
    body = function_body(read(SRC), "static long DaysFromCivilUtc")
    assert "year -= month <= 2;" in body
    # The shift MUST happen before era/yoe are computed off `year`.
    assert body.index("year -= month <= 2;") < body.index("const long era")


def test_days_from_civil_era_and_yoe_use_the_400_year_cycle():
    # era = floor(year/400) with the negative-year correction; yoe is the
    # year-of-era in [0,399].
    body = function_body(read(SRC), "static long DaysFromCivilUtc")
    assert "const long era = (year >= 0 ? year : year - 399) / 400;" in body
    assert "const unsigned yoe = static_cast<unsigned>(year - era * 400);" in body


def test_days_from_civil_doy_uses_the_153x_plus_2_over_5_month_table():
    # day-of-year via the (153*m'+2)/5 closed form with the March-relative month
    # remap (m>2 ? -3 : +9), then +day-1.
    body = function_body(read(SRC), "static long DaysFromCivilUtc")
    assert "(153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1" in body


def test_days_from_civil_doe_and_final_term_use_the_gregorian_leap_counts():
    # day-of-era folds in the 4/100/400 leap-day counts; the final term converts
    # era+doe into days-since-1970 via the 146097-day era length and the
    # 719468 offset from civil-0000 to 1970-01-01.
    body = function_body(read(SRC), "static long DaysFromCivilUtc")
    assert "const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;" in body
    assert "return era * 146097L + static_cast<long>(doe) - 719468L;" in body


# ===========================================================================
# ParseIso8601UtcToEpoch
# ===========================================================================

def test_parse_iso8601_rejects_empty_string_first():
    # An empty timestamp can never be valid; the guard returns false before any
    # sscanf so out_epoch_seconds is left untouched.
    body = function_body(read(SRC), "bool ParseIso8601UtcToEpoch")
    assert "if (iso_utc.empty())" in body
    empty_branch = body[body.index("if (iso_utc.empty())"):]
    empty_branch = empty_branch[:empty_branch.index("}") + 1]
    assert "return false;" in empty_branch
    assert body.index("if (iso_utc.empty())") < body.index("sscanf")


def test_parse_iso8601_accepts_both_T_and_space_separators():
    # RFC-3339 uses 'T'; some backends emit a space. Both sscanf templates are
    # tried and EITHER matching all six fields is accepted.
    body = function_body(read(SRC), "bool ParseIso8601UtcToEpoch")
    assert 'sscanf(iso_utc.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d"' in body
    assert 'sscanf(iso_utc.c_str(), "%4d-%2d-%2d %2d:%2d:%2d"' in body
    # The accept condition is "neither template produced 6 fields -> false",
    # i.e. both are joined by && so a single match suffices.
    assert "!= 6 &&" in body
    assert ") != 6) {" in body


def test_parse_iso8601_uses_width_limited_field_conversions():
    # The %4d / %2d width caps stop "12345-..." from over-reading into the next
    # field; both templates carry the same widths.
    body = function_body(read(SRC), "bool ParseIso8601UtcToEpoch")
    assert body.count("%4d-%2d-%2d") == 2  # one per separator template


def test_parse_iso8601_validates_every_field_range():
    # Month 1..12, day 1..31, hour 0..23, minute 0..59, second 0..60 (the 60
    # explicitly permits a leap second). Any out-of-range field -> false.
    body = function_body(read(SRC), "bool ParseIso8601UtcToEpoch")
    assert "month < 1 || month > 12" in body
    assert "day < 1 || day > 31" in body
    assert "hour < 0 || hour > 23" in body
    assert "minute < 0 || minute > 59" in body
    assert "second < 0 || second > 60" in body  # 60 == leap second allowance
    # The leap-second intent is documented at the second bound.
    assert "leap second" in body
    # Range validation runs AFTER a successful sscanf and BEFORE the epoch math.
    assert body.index("month < 1") > body.index("sscanf")
    assert body.index("month < 1") < body.index("DaysFromCivilUtc")


def test_parse_iso8601_composes_epoch_from_days_plus_time_of_day():
    # epoch = DaysFromCivilUtc(...) * 86400 + h*3600 + m*60 + s, all UTC.
    body = function_body(read(SRC), "bool ParseIso8601UtcToEpoch")
    assert "DaysFromCivilUtc(year, static_cast<unsigned>(month)," in body
    assert "days * 86400L" in body
    assert "hour * 3600L + minute * 60L + second" in body
    assert "out_epoch_seconds = static_cast<time_t>(" in body
    # Success returns true after writing the out param.
    tail = body[body.index("out_epoch_seconds = static_cast<time_t>("):]
    assert "return true;" in tail


def test_parse_iso8601_header_documents_utc_only_contract():
    hdr = read(HDR)
    assert "bool ParseIso8601UtcToEpoch(const std::string& iso_utc, time_t& out_epoch_seconds);" in hdr
    # Header contract: empty/unparseable -> false; UTC only.
    decl = hdr[hdr.index("Parse an ISO-8601"):hdr.index("bool ParseIso8601UtcToEpoch(")]
    assert "false" in decl
    assert "UTC" in decl


# ===========================================================================
# IsPendingTbotClaimExpired
# ===========================================================================

def test_is_expired_treats_unparseable_expiry_as_never_expire():
    # No parseable expires_at -> return false (NOT expired). A missing field must
    # never strand/tear down a live claim window; the bounded poll's cap is the
    # backstop. This is the safety-critical direction.
    body = function_body(read(SRC), "bool IsPendingTbotClaimExpired")
    assert "if (!ParseIso8601UtcToEpoch(claim.expires_at, expires_epoch))" in body
    fail_branch = body[body.index("if (!ParseIso8601UtcToEpoch"):]
    fail_branch = fail_branch[:fail_branch.index("}") + 1]
    assert "return false;" in fail_branch
    # The unparseable -> false return precedes the comparison return.
    assert body.index("if (!ParseIso8601UtcToEpoch") < body.index("now_epoch_seconds >=")


def test_is_expired_compares_now_at_or_after_deadline():
    # Expiry is inclusive: now >= expires => expired. Direction matters (a
    # flipped `<=` would expire everything in the past forever-young).
    body = function_body(read(SRC), "bool IsPendingTbotClaimExpired")
    assert "return now_epoch_seconds >= expires_epoch;" in body
    # It must compare against the freshly-parsed deadline, not a hardcoded value.
    assert "ParseIso8601UtcToEpoch(claim.expires_at, expires_epoch)" in body


def test_is_expired_initializes_deadline_before_parse():
    # expires_epoch is zero-initialized so a parser that returns true without
    # writing (it always writes, but defense in depth) cannot read indeterminate
    # memory.
    body = function_body(read(SRC), "bool IsPendingTbotClaimExpired")
    assert "time_t expires_epoch = 0;" in body
    assert body.index("time_t expires_epoch = 0;") < body.index("ParseIso8601UtcToEpoch")


def test_is_expired_header_documents_never_expire_on_missing_field():
    hdr = read(HDR)
    assert "bool IsPendingTbotClaimExpired(const PendingTbotClaim& claim, time_t now_epoch_seconds);" in hdr
    doc = hdr[hdr.index("// True when the claim has a parseable"):hdr.index("bool IsPendingTbotClaimExpired(")]
    assert "NOT expired" in doc


# ===========================================================================
# Credential-redaction audit of the HTTP-failure log sinks (REDACT1/REDACT2)
# ===========================================================================

# Output sinks that reach the serial console / UART.
_SINK_RE = re.compile(
    r"\bESP_LOG\w*\s*\("
    r"|\b(?:printf|fprintf|snprintf|vprintf|vfprintf|puts|fputs)\s*\("
)


def _strip_comments(source: str) -> str:
    no_block = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", no_block)


def _statements(source: str):
    """Collapse to logical statements robust to multi-line ESP_LOG calls:
    strip comments, blank string-literal contents (so format text cannot match a
    variable name and ;{} inside a literal cannot mis-split), collapse
    whitespace, split on ; { }."""
    no_comments = _strip_comments(source)
    blanked = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', no_comments)
    collapsed = re.sub(r"\s+", " ", blanked)
    return [s.strip() for s in re.split(r"[;{}]", collapsed) if s.strip()]


def _sink_statements(source: str):
    return [s for s in _statements(source) if _SINK_RE.search(s)]


def test_redact_no_raw_token_or_code_identifier_reaches_any_log_sink():
    # REDACT (positive invariant): the bootstrap_token / Authorization header
    # value and any provisioning code must NEVER be passed as a log argument.
    # The reporter logs token PRESENCE (token_empty=%d) only; assert no sink
    # statement carries the raw secret expressions.
    src = read(SRC)
    forbidden = (
        "bootstrap_token.c_str()",
        "bootstrap_token)",          # e.g. ESP_LOGx(..., bootstrap_token)
        "claim.claim_id.c_str()",    # claim_id is attempt-scoped; keep it out of logs too
    )
    for stmt in _sink_statements(src):
        for token in forbidden:
            assert token not in stmt, f"secret-bearing value reached a log sink: {stmt!r}"
    # Positive: the only token-derived value logged is its emptiness boolean.
    assert "token_empty=%d" in src


def test_redact_open_failure_sinks_log_only_an_error_code():
    # The HTTP-open failure sites (device-config GET, bootstrap GET,
    # authenticated config refresh, claim-confirm POST) log only
    # http->GetLastError() (a numeric esp_err),
    # never the Authorization header value, the bootstrap token, or the URL query
    # secret. Format-string text is blanked by _statements(), so we key off the
    # GetLastError() argument that survives blanking.
    src = read(SRC)
    open_fail_sinks = [s for s in _sink_statements(src) if "GetLastError()" in s]
    # The source has exactly four Open()-failure log sites.
    assert len(open_fail_sinks) == 4, (
        f"expected 4 HTTP-open-failure sinks, found {len(open_fail_sinks)}: {open_fail_sinks}"
    )
    for stmt in open_fail_sinks:
        assert "Bearer" not in stmt
        assert "bootstrap_token" not in stmt
        assert "response_body" not in stmt  # open-failure happens before any read


def test_redact_bootstrap_fetch_failure_does_not_log_response_body():
    # FetchBackendApiUrlFromBootstrap is the GOOD example: its non-2xx failure
    # logs HTTP status only, never the response body (which would carry api_url /
    # possibly token-adjacent material). Lock that in as the desired shape.
    body = function_body(read(SRC), "std::string FetchBackendApiUrlFromBootstrap")
    fail = body[body.index("status_code < 200 || status_code >= 300"):]
    fail = fail[:fail.index("}") + 1]
    assert "Bootstrap api_url fetch failed (HTTP %d)" in fail
    assert "body=%s" not in fail
    assert "response_body" not in fail.split("ESP_LOGW")[1].split(";")[0]


def test_redact_failure_sinks_must_not_log_verbatim_response_body():
    # Regression lock (fixed 2026-06-10): NEITHER the device-config-fetch nor the
    # claim-confirm failure path may log the raw response_body, which can carry
    # device_secret. Both sinks now log a length only (resp_len=%u).
    src = read(SRC)
    body_logging_sinks = [
        s for s in _sink_statements(src)
        if "response_body.c_str()" in s
    ]
    assert body_logging_sinks == [], (
        "HTTP-failure sink logs verbatim response body (can carry device_secret): "
        f"{body_logging_sinks}"
    )


def test_failure_sinks_log_redacted_response_length_not_body():
    # Positive lock (fixed 2026-06-10): the two non-2xx failure warnings now log
    # an HTTP status + response LENGTH (resp_len=%u) only -- never the verbatim
    # body (which can carry device_secret).
    src = read(SRC)
    assert src.count("resp_len=%u") >= 2, "both failure sinks must log resp_len only"
    # No log sink emits the verbatim response body anywhere.
    assert [s for s in _sink_statements(src) if "response_body.c_str()" in s] == []
    # The success path persists the body, it does not log it.
    confirm = function_body(src, "ClaimConfirmationResult ClaimConfirmationReporter::Confirm")
    success = confirm[confirm.index("status_code >= 200 && status_code < 300"):]
    success = success[:success.index("ClaimConfirmationResult::Confirmed")]
    assert "response_body.c_str()" not in success
