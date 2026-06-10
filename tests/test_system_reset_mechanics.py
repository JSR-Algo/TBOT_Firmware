"""Static source-scrape mechanics suite for SystemReset (US-005 round-2 gap fill).

This locks the *control-flow contract* of main/boards/common/system_reset.cc that
no other firmware unit covers today: which branch erases NVS, which keeps it, and
the exact ordering / short-circuit / error semantics of the cloud-ownership
release before a physical factory reset.

Why source-scrape rather than a linked unit test? SystemReset pulls in ESP-IDF
(nvs_flash, gpio, esp_partition, freertos) and Board singletons that cannot be
exercised off-target in this repo's harness. The risk we are actually defending
against is a *logic regression in the reset decision tree* (e.g. someone moving
ResetNvsFlash() out of the success branch, or dropping the non-2xx guard so a
robot wipes its credentials while the backend still owns it -> next phone claim
fails with DEVICE_ALREADY_OWNED). A structural scrape of the real branches
catches exactly that class of regression and runs in CI without hardware.

SECRET SAFETY: device_secret is read from NVS and sent as the X-Device-Token
header. These tests assert on the *header wiring and ordering*, never on any
secret value, and they assert the source NEVER logs the secret variable.
"""

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = "main/boards/common/system_reset.cc"


# --------------------------------------------------------------------------- #
# Source-reading helpers (mirrors the existing factory-reset contract suite).
# --------------------------------------------------------------------------- #
def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    """Return the brace-balanced body (including the outer braces) of a function.

    Raises if the signature is missing or the braces never balance, so a test
    asserting against a body fails loudly instead of silently matching nothing.
    """
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


@pytest.fixture(scope="module")
def source() -> str:
    return read(SOURCE_PATH)


@pytest.fixture(scope="module")
def check_buttons(source: str) -> str:
    return function_body(source, "void SystemReset::CheckButtons")


@pytest.fixture(scope="module")
def release_ownership(source: str) -> str:
    return function_body(source, "bool SystemReset::ReleaseCloudOwnership")


@pytest.fixture(scope="module")
def build_url(source: str) -> str:
    return function_body(source, "static std::string BuildFactoryResetUrl")


@pytest.fixture(scope="module")
def url_encode(source: str) -> str:
    return function_body(source, "static std::string UrlEncodeQueryParam")


@pytest.fixture(scope="module")
def reset_nvs(source: str) -> str:
    return function_body(source, "void SystemReset::ResetNvsFlash")


@pytest.fixture(scope="module")
def reset_to_factory(source: str) -> str:
    return function_body(source, "void SystemReset::ResetToFactory")


@pytest.fixture(scope="module")
def restart_in_seconds(source: str) -> str:
    return function_body(source, "void SystemReset::RestartInSeconds")


# --------------------------------------------------------------------------- #
# Sanity: the helpers actually carve the real functions out.
# --------------------------------------------------------------------------- #
def test_source_file_exists_and_nonempty(source: str):
    assert len(source) > 0
    assert "class" not in source  # .cc, not the header


def test_all_target_functions_are_present(source: str):
    for signature in (
        "void SystemReset::CheckButtons",
        "bool SystemReset::ReleaseCloudOwnership",
        "static std::string BuildFactoryResetUrl",
        "static std::string UrlEncodeQueryParam",
        "void SystemReset::ResetNvsFlash",
        "void SystemReset::ResetToFactory",
        "void SystemReset::RestartInSeconds",
    ):
        body = function_body(source, signature)
        assert body.startswith("{") and body.endswith("}")


def test_function_body_balances_braces(check_buttons: str):
    assert check_buttons.count("{") == check_buttons.count("}")


# =========================================================================== #
# CheckButtons -- the reset decision tree.
# =========================================================================== #
def test_check_buttons_factory_pin_gated_on_level_zero(check_buttons: str):
    # Active-low button: a press reads level 0.
    assert "if (gpio_get_level(reset_factory_pin_) == 0)" in check_buttons


def test_check_buttons_factory_success_branch_erases_then_factory(check_buttons: str):
    # On a *successful* cloud release, NVS is wiped and then otadata reset.
    assert "if (ReleaseCloudOwnership())" in check_buttons
    success_region = check_buttons[
        check_buttons.index("if (ReleaseCloudOwnership())") : check_buttons.index("} else")
    ]
    assert "ResetNvsFlash();" in success_region
    assert "ResetToFactory();" in success_region
    # Ordering: erase local NVS, then flip otadata to factory.
    assert success_region.index("ResetNvsFlash();") < success_region.index("ResetToFactory();")


def test_check_buttons_factory_release_fails_branch_keeps_nvs_and_retries(check_buttons: str):
    # The whole point of the round-1 fix: when cloud release fails we must NOT
    # erase NVS (so the credentialed retry can run) -- we reboot and retry.
    #
    # Scope strictly to the else block of the *factory* if, i.e. from `} else`
    # up to the start of the second, independent NVS-only `if` (which legitimately
    # calls ResetNvsFlash and must not be counted as part of the failure branch).
    else_start = check_buttons.index("} else")
    nvs_if_start = check_buttons.index("if (gpio_get_level(reset_nvs_pin_) == 0)")
    assert else_start < nvs_if_start  # else block precedes the NVS-only if
    else_region = check_buttons[else_start:nvs_if_start]
    assert "RestartInSeconds(3);" in else_region
    # The failure branch must not wipe NVS or jump straight to factory.
    assert "ResetNvsFlash();" not in else_region
    assert "ResetToFactory();" not in else_region
    # And it must log the *intent to keep credentials* (not the credentials).
    assert "keeping NVS credentials" in else_region


def test_check_buttons_factory_branch_calls_release_before_any_erase(check_buttons: str):
    # Defends the security-critical ordering: ownership release is attempted
    # before ResetNvsFlash() can run.
    assert check_buttons.index("ReleaseCloudOwnership()") < check_buttons.index("ResetNvsFlash();")


def test_check_buttons_nvs_only_branch_gated_and_does_not_release_cloud(check_buttons: str):
    # The second button is a *local* NVS reset: it erases NVS but must NOT
    # attempt a cloud ownership release (that path is factory-reset only).
    nvs_region = check_buttons[check_buttons.index("if (gpio_get_level(reset_nvs_pin_) == 0)") :]
    assert "ResetNvsFlash();" in nvs_region
    assert "ReleaseCloudOwnership()" not in nvs_region
    assert "ResetToFactory();" not in nvs_region


def test_check_buttons_has_two_independent_pin_checks(check_buttons: str):
    # Factory pin and NVS pin are checked in two separate, non-nested ifs.
    assert check_buttons.count("gpio_get_level(reset_factory_pin_) == 0") == 1
    assert check_buttons.count("gpio_get_level(reset_nvs_pin_) == 0") == 1


def test_check_buttons_nvs_only_branch_does_not_restart(check_buttons: str):
    # A pure NVS reset re-inits NVS in place; it must not reboot the device.
    nvs_region = check_buttons[check_buttons.index("if (gpio_get_level(reset_nvs_pin_) == 0)") :]
    assert "RestartInSeconds" not in nvs_region


# =========================================================================== #
# ReleaseCloudOwnership -- short-circuit, build, transport, status.
# =========================================================================== #
def test_release_reads_creds_from_backend_namespace(release_ownership: str):
    assert 'Settings backend_settings("backend", false)' in release_ownership
    assert 'backend_settings.GetString("api_url")' in release_ownership
    assert 'backend_settings.GetString("device_secret")' in release_ownership
    assert "Board::GetInstance().GetUuid()" in release_ownership


def test_release_short_circuits_true_when_all_creds_absent(release_ownership: str):
    # No cloud creds == device was never claimed -> local reset is allowed to
    # proceed (return true), and crucially no network call is attempted.
    guard = "if (api_url.empty() || device_secret.empty() || device_id.empty())"
    assert guard in release_ownership
    guard_region = release_ownership[
        release_ownership.index(guard) : release_ownership.index(guard) + 250
    ]
    assert "return true;" in guard_region
    # The short-circuit return precedes any HTTP construction.
    assert release_ownership.index(guard) < release_ownership.index("CreateHttp")


def test_release_returns_false_when_built_url_is_empty(release_ownership: str):
    assert "const std::string url = BuildFactoryResetUrl(api_url, device_id);" in release_ownership
    url_guard_region = release_ownership[
        release_ownership.index("if (url.empty())") : release_ownership.index("CreateHttp")
    ]
    assert "return false;" in url_guard_region


def test_release_returns_false_when_http_client_is_null(release_ownership: str):
    assert "auto http = network->CreateHttp(2);" in release_ownership
    null_region = release_ownership[
        release_ownership.index("if (!http)") : release_ownership.index("if (!http)") + 200
    ]
    assert "return false;" in null_region
    # Null check happens before any header is set.
    assert release_ownership.index("if (!http)") < release_ownership.index("SetHeader")


def test_release_sets_all_required_headers(release_ownership: str):
    assert 'http->SetHeader("X-Device-Token", device_secret)' in release_ownership
    assert 'http->SetHeader("X-Device-Id", device_id)' in release_ownership
    assert 'http->SetHeader("Content-Type", "application/json")' in release_ownership
    assert 'http->SetHeader("Device-Id", SystemInfo::GetMacAddress())' in release_ownership
    assert 'http->SetHeader("User-Agent", SystemInfo::GetUserAgent())' in release_ownership


def test_release_sets_timeout_before_open(release_ownership: str):
    assert "http->SetTimeout(5000);" in release_ownership
    assert release_ownership.index("http->SetTimeout(5000);") < release_ownership.index('Open("POST"')


def test_release_posts_empty_json_body(release_ownership: str):
    # Body is "{}" moved into SetContent before Open.
    assert 'std::string body = "{}";' in release_ownership
    assert "http->SetContent(std::move(body));" in release_ownership
    assert release_ownership.index("SetContent") < release_ownership.index('Open("POST"')


def test_release_opens_post_to_built_url(release_ownership: str):
    assert 'http->Open("POST", url)' in release_ownership


def test_release_returns_false_and_closes_on_open_failure(release_ownership: str):
    open_region = release_ownership[
        release_ownership.index('if (!http->Open("POST", url))') :
    ]
    # Must close the client and bail on a failed open.
    fail_block = open_region[: open_region.index("return false;") + len("return false;")]
    assert "http->Close();" in fail_block
    assert "return false;" in fail_block


def test_release_treats_below_200_as_failure(release_ownership: str):
    # The status guard is the credential-safety gate: only a 2xx releases.
    assert "if (status_code < 200 || status_code >= 300)" in release_ownership


def test_release_treats_300_and_above_as_failure(release_ownership: str):
    guard = "if (status_code < 200 || status_code >= 300)"
    guard_region = release_ownership[
        release_ownership.index(guard) : release_ownership.index(guard) + 200
    ]
    assert "return false;" in guard_region


def test_release_reads_status_then_closes_before_evaluating(release_ownership: str):
    # GetStatusCode() is read, then Close(), then the 2xx check -- so the socket
    # is always released regardless of status.
    assert "const int status_code = http->GetStatusCode();" in release_ownership
    idx_status = release_ownership.index("const int status_code = http->GetStatusCode();")
    idx_close = release_ownership.index("http->Close();", idx_status)
    idx_guard = release_ownership.index("if (status_code < 200")
    assert idx_status < idx_close < idx_guard


def test_release_returns_true_only_on_2xx(release_ownership: str):
    # The final return true must come *after* the status guard, i.e. only a 2xx
    # that fell through the guard returns success.
    guard_idx = release_ownership.index("if (status_code < 200 || status_code >= 300)")
    # Find the final return true (success path), which is the last `return true;`.
    last_true = release_ownership.rindex("return true;")
    assert last_true > guard_idx


def test_release_logs_open_error_via_getlasterror_not_secret(release_ownership: str):
    # Error path logs the transport error code, never the token.
    assert "http->GetLastError()" in release_ownership


# =========================================================================== #
# BuildFactoryResetUrl -- normalization + path construction.
# =========================================================================== #
def test_build_url_returns_empty_when_api_url_or_device_id_empty(build_url: str):
    assert "if (api_url.empty() || device_id.empty())" in build_url
    guard_region = build_url[
        build_url.index("if (api_url.empty() || device_id.empty())") :
    ]
    assert 'return "";' in guard_region
    assert build_url.index("return \"\";") < build_url.index("UrlEncodeQueryParam")


def test_build_url_strips_trailing_slashes(build_url: str):
    # All trailing slashes are popped (loop, not a single trim).
    assert "while (!base.empty() && base.back() == '/')" in build_url
    assert "base.pop_back();" in build_url


def test_build_url_inserts_v1_only_when_missing(build_url: str):
    assert 'if (base.find("/v1") == std::string::npos)' in build_url
    insert_region = build_url[
        build_url.index('if (base.find("/v1") == std::string::npos)') :
    ]
    assert 'base += "/v1";' in insert_region


def test_build_url_assembles_factory_reset_path_with_encoded_device_id(build_url: str):
    assert '"/devices/" + UrlEncodeQueryParam(device_id) + "/factory-reset"' in build_url
    # device_id is encoded, base is concatenated raw (already normalized).
    assert "return base +" in build_url


def test_build_url_normalizes_before_appending_v1(build_url: str):
    # Trailing-slash strip must run before the /v1 presence check, otherwise a
    # URL ending in "/v1/" would be mis-detected. Assert ordering.
    assert build_url.index("base.pop_back();") < build_url.index('base.find("/v1")')


# =========================================================================== #
# UrlEncodeQueryParam -- RFC3986 unreserved passthrough vs percent-encode.
# =========================================================================== #
def test_url_encode_passes_unreserved_chars_through(url_encode: str):
    # The unreserved set: ALPHA / DIGIT / - / _ / . / ~
    assert "(c >= 'A' && c <= 'Z')" in url_encode
    assert "(c >= 'a' && c <= 'z')" in url_encode
    assert "(c >= '0' && c <= '9')" in url_encode
    assert "c == '-'" in url_encode
    assert "c == '_'" in url_encode
    assert "c == '.'" in url_encode
    assert "c == '~'" in url_encode
    # Passthrough pushes the raw char and continues.
    pass_region = url_encode[: url_encode.index("encoded.push_back('%')")]
    assert "encoded.push_back(static_cast<char>(c));" in pass_region
    assert "continue;" in pass_region


def test_url_encode_percent_encodes_everything_else(url_encode: str):
    assert "encoded.push_back('%');" in url_encode
    # High nibble then low nibble from a hex table.
    assert "hex[(c >> 4) & 0x0f]" in url_encode
    assert "hex[c & 0x0f]" in url_encode
    assert 'static const char* hex = "0123456789ABCDEF";' in url_encode


def test_url_encode_iterates_as_unsigned_char(url_encode: str):
    # Iterating over unsigned char avoids sign-extension bugs on bytes >= 0x80,
    # which would otherwise mis-encode UTF-8 / high-bit device ids.
    assert "for (unsigned char c : value)" in url_encode


def test_url_encode_reserves_capacity(url_encode: str):
    assert "encoded.reserve(value.size());" in url_encode


# --- Boundary check: model the C++ predicate in Python and assert the exact
# --- unreserved set, so a future edit that (e.g.) drops '~' or adds '+' is
# --- caught even though we cannot link the real function.
def _unreserved_set_from_source(url_encode_body: str) -> set:
    chars = set()
    for lo, hi in re.findall(r"c >= '(.)' && c <= '(.)'", url_encode_body):
        for code in range(ord(lo), ord(hi) + 1):
            chars.add(chr(code))
    for ch in re.findall(r"c == '(.)'", url_encode_body):
        chars.add(ch)
    return chars


def test_url_encode_unreserved_set_is_exactly_rfc3986(url_encode: str):
    import string

    expected = set(string.ascii_letters + string.digits + "-_.~")
    assert _unreserved_set_from_source(url_encode) == expected


@pytest.mark.parametrize("reserved", [" ", "/", ":", "+", "%", "?", "&", "=", "#", "@"])
def test_url_encode_treats_reserved_chars_as_encodable(url_encode: str, reserved: str):
    # None of these are in the passthrough set -> they hit the percent branch.
    assert reserved not in _unreserved_set_from_source(url_encode)


# =========================================================================== #
# ResetNvsFlash -- erase then re-init, with error logging.
# =========================================================================== #
def test_reset_nvs_erases_then_inits(reset_nvs: str):
    assert "nvs_flash_erase();" in reset_nvs
    assert "nvs_flash_init();" in reset_nvs
    assert reset_nvs.index("nvs_flash_erase();") < reset_nvs.index("nvs_flash_init();")


def test_reset_nvs_logs_errors_on_each_step(reset_nvs: str):
    assert "Failed to erase NVS flash" in reset_nvs
    assert "Failed to initialize NVS flash" in reset_nvs
    assert reset_nvs.count("if (ret != ESP_OK)") == 2


# =========================================================================== #
# ResetToFactory -- otadata guard + erase + reboot.
# =========================================================================== #
def test_reset_to_factory_finds_otadata_partition(reset_to_factory: str):
    assert (
        "esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL)"
        in reset_to_factory
    )


def test_reset_to_factory_guards_null_partition_before_erase(reset_to_factory: str):
    assert "if (partition == NULL)" in reset_to_factory
    guard_region = reset_to_factory[
        reset_to_factory.index("if (partition == NULL)") :
    ]
    # On missing partition: log + early return, never erase a NULL handle.
    early_return_block = guard_region[: guard_region.index("}") + 1]
    assert "return;" in early_return_block
    assert "Failed to find otadata partition" in early_return_block
    # The guard precedes the erase call.
    assert reset_to_factory.index("if (partition == NULL)") < reset_to_factory.index(
        "esp_partition_erase_range"
    )


def test_reset_to_factory_erases_full_partition_range(reset_to_factory: str):
    assert "esp_partition_erase_range(partition, 0, partition->size);" in reset_to_factory


def test_reset_to_factory_reboots_after_erase(reset_to_factory: str):
    assert "RestartInSeconds(3);" in reset_to_factory
    assert reset_to_factory.index("esp_partition_erase_range") < reset_to_factory.index(
        "RestartInSeconds(3);"
    )


# =========================================================================== #
# RestartInSeconds -- countdown then esp_restart.
# =========================================================================== #
def test_restart_counts_down_then_restarts(restart_in_seconds: str):
    assert "for (int i = seconds; i > 0; i--)" in restart_in_seconds
    assert "vTaskDelay(1000 / portTICK_PERIOD_MS);" in restart_in_seconds
    assert "esp_restart();" in restart_in_seconds
    # The reboot is the last action, after the loop.
    assert restart_in_seconds.index("for (int i = seconds") < restart_in_seconds.index(
        "esp_restart();"
    )


# =========================================================================== #
# Constructor -- GPIO configured as pulled-up inputs (active-low buttons).
# =========================================================================== #
def test_constructor_configures_both_pins_as_pulled_up_inputs(source: str):
    ctor = function_body(source, "SystemReset::SystemReset(gpio_num_t reset_nvs_pin")
    assert "io_conf.mode = GPIO_MODE_INPUT;" in ctor
    assert "io_conf.pull_up_en = GPIO_PULLUP_ENABLE;" in ctor
    assert "io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;" in ctor
    # Both pins go in the bit mask.
    assert "(1ULL << reset_nvs_pin_) | (1ULL << reset_factory_pin_)" in ctor
    assert "io_conf.intr_type = GPIO_INTR_DISABLE;" in ctor
    assert "gpio_config(&io_conf);" in ctor


# =========================================================================== #
# Secret-safety invariants (US-005 cardinal rule).
# =========================================================================== #
def test_device_secret_is_never_logged(source: str):
    # device_secret may be read and set as a header, but must never appear in any
    # ESP_LOG* statement. Scan every log line for the variable token.
    for match in re.finditer(r"ESP_LOG[A-Z]\([^;]*\);", source, re.DOTALL):
        log_stmt = match.group(0)
        assert "device_secret" not in log_stmt, f"secret leaked into log: {log_stmt}"


def test_device_secret_only_referenced_in_safe_sinks(source: str):
    # Every use of the `device_secret` local must be: the declaration, the
    # emptiness guard, or the X-Device-Token header. No print / no concat into a
    # URL / no log.
    body = function_body(source, "bool SystemReset::ReleaseCloudOwnership")
    safe_lines = [
        'const std::string device_secret = backend_settings.GetString("device_secret");',
        "if (api_url.empty() || device_secret.empty() || device_id.empty())",
        'http->SetHeader("X-Device-Token", device_secret);',
    ]
    for line in body.splitlines():
        if "device_secret" in line:
            stripped = line.strip()
            assert any(
                safe.strip() in stripped for safe in safe_lines
            ), f"device_secret used in an unexpected sink: {stripped!r}"


def test_token_header_name_present_but_value_not_inlined(release_ownership: str):
    # Sanity: the header is keyed by the device_secret *variable*, never a
    # literal secret baked into source.
    assert 'http->SetHeader("X-Device-Token", device_secret)' in release_ownership
    # No obviously-literal token assignment in this function.
    assert "X-Device-Token\", \"" not in release_ownership
