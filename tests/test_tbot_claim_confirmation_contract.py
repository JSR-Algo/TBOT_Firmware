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


def test_pending_claim_parser_reads_robot_visible_claim_envelope():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    body = function_body(source, "bool ParsePendingTbotClaimFromDeviceConfigJson")

    assert 'cJSON_GetObjectItem(root, "claim")' in body
    assert 'cJSON_GetObjectItem(claim, "claim_id")' in body
    assert 'cJSON_GetObjectItem(claim, "status")' in body
    assert 'cJSON_GetObjectItem(claim, "message")' in body
    assert 'cJSON_GetObjectItem(claim, "expires_at")' in body
    assert 'WAITING_PHYSICAL_CONFIRM' in body
    assert 'cJSON_IsNull(claim)' in body
    assert 'return true;' in body
    assert "return false;" in body


def test_claim_confirm_body_and_url_match_backend_contract():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    body_builder = function_body(source, "std::string BuildTbotClaimConfirmBody")
    url_builder = function_body(source, "std::string BuildTbotClaimConfirmUrl")

    assert 'cJSON_AddStringToObject(root, "claim_id", claim_id.c_str())' in body_builder
    assert 'cJSON_AddStringToObject(root, "device_id", device_id.c_str())' in body_builder
    assert 'cJSON_AddStringToObject(root, "confirm_method", confirm_method.c_str())' in body_builder
    assert 'button_press' in read("main/provisioning/claim_confirmation_reporter.h")
    assert '/claim/confirm' in url_builder
    assert 'tbot-backend-8wmh.onrender.com/tbot/v1/' not in source
    assert 'trycloudflare.com/tbot/v1/' not in source

def test_device_config_polling_uses_backend_api_url_and_robot_identity():
    source = read("main/provisioning/claim_confirmation_reporter.cc")

    poll_url = function_body(source, "std::string BuildTbotDeviceConfigUrl")
    poll_body = function_body(source, "bool FetchPendingTbotClaimFromDeviceConfig")
    identity_body = function_body(source, "static std::string GetTbotClaimDeviceId")

    assert '/device/config?device_id=' in poll_url
    assert 'UrlEncodeQueryParam(device_id)' in poll_url
    assert '+ device_id' not in poll_url
    assert 'GetTbotClaimDeviceId()' in poll_body
    assert 'Settings websocket_settings("websocket", false);' in identity_body
    assert 'websocket_settings.GetString("claim_device_id")' in identity_body
    assert 'Board::GetInstance().GetUuid()' in identity_body
    assert 'http->Open("GET", url)' in poll_body
    assert 'ParsePendingTbotClaimFromDeviceConfigJson' in poll_body
    assert 'tbot-backend-8wmh.onrender.com/tbot/v1/' not in source
    assert 'trycloudflare.com/tbot/v1/' not in source

def test_device_config_query_encoder_percent_encodes_reserved_characters():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    encoder = function_body(source, "std::string UrlEncodeQueryParam")

    assert '0123456789ABCDEF' in encoder
    assert "encoded.push_back('%')" in encoder
    assert "c == '-' || c == '_' || c == '.' || c == '~'" in encoder


def test_claim_confirm_reporter_uses_device_identity_and_bootstrap_token():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    report_body = function_body(source, "ClaimConfirmationResult ClaimConfirmationReporter::Confirm")

    assert 'GetTbotClaimDeviceId()' in report_body
    assert 'BuildTbotClaimConfirmBody(claim.claim_id, device_id' in report_body
    assert 'http->SetHeader("Authorization", "Bearer " + bootstrap_token)' in report_body
    assert 'http->SetHeader("Content-Type", "application/json")' in report_body
    assert 'http->Open("POST", url)' in report_body


def test_claim_confirm_failure_logs_backend_error_code_without_raw_body():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    helper_body = function_body(source, "static std::string ExtractTbotClaimErrorSummary")
    report_body = function_body(source, "ClaimConfirmationResult ClaimConfirmationReporter::Confirm")

    assert 'cJSON_GetObjectItem(root, "code")' in helper_body
    assert 'cJSON_GetObjectItem(root, "message")' in helper_body
    assert 'device_secret' in helper_body
    assert 'Claim confirmation failed (HTTP %d) resp_len=%u%s' in report_body
    assert 'ExtractTbotClaimErrorSummary(response_body)' in report_body
    assert 'ESP_LOGW(TAG, "Claim confirmation failed (HTTP %d) body=%s"' not in source


def test_claim_confirm_response_persists_device_secret_and_websocket_url():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    header = read("main/provisioning/claim_confirmation_reporter.h")

    persist_body = function_body(source, "static bool ProcessTbotClaimConfirmationResponse")
    wrapper_body = function_body(source, "bool PersistTbotClaimConfirmationResponse")
    confirm_body = function_body(source, "ClaimConfirmationResult ClaimConfirmationReporter::Confirm")

    assert 'cJSON_GetObjectItem(root, "device_id")' in persist_body
    assert 'cJSON_GetObjectItem(root, "device_secret")' in persist_body
    assert 'cJSON_GetObjectItem(root, "ws_url")' in persist_body
    assert 'is_nonempty_string(device_id)' in persist_body
    assert 'is_nonempty_string(device_secret)' in persist_body
    assert 'is_nonempty_string(ws_url)' in persist_body
    assert 'Settings websocket_settings("websocket", true);' in persist_body
    assert 'websocket_settings.SetString("url", ws_url->valuestring);' in persist_body
    assert 'Settings backend_settings("backend", true);' in persist_body
    assert 'backend_settings.SetString("device_id", device_id->valuestring);' in persist_body
    assert 'backend_settings.SetString("device_secret", device_secret->valuestring);' in persist_body
    assert 'SetString("token", device_secret->valuestring)' not in persist_body
    assert 'ProcessTbotClaimConfirmationResponse(' in confirm_body
    assert 'response_body, deferred_success_response == nullptr' in confirm_body
    assert 'bool PersistTbotClaimConfirmationResponse(const std::string& json);' in header
    assert 'ProcessTbotClaimConfirmationResponse(json, true)' in wrapper_body


def test_claim_confirm_response_rejects_empty_credentials_before_any_settings_write():
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    persist_body = function_body(source, "static bool ProcessTbotClaimConfirmationResponse")

    validation_end = persist_body.index('Settings websocket_settings("websocket", true);')
    validation = persist_body[:validation_end]
    assert "valuestring != nullptr" in validation
    assert "valuestring[0] != '\\0'" in validation
    assert validation.count("is_nonempty_string(") == 4
    assert "require_api_url && !is_nonempty_string(api_url)" in validation
    assert 'Settings backend_settings("backend", true);' not in validation
    assert 'Settings claim_state("tbot_claim", true);' not in validation


def test_claim_confirm_result_distinguishes_retryable_and_terminal_failures():
    header = read("main/provisioning/claim_confirmation_reporter.h")
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    confirm_body = function_body(source, "ClaimConfirmationResult ClaimConfirmationReporter::Confirm")

    assert "enum class ClaimConfirmationResult" in header
    for result in ("Confirmed", "RetryableFailure", "AmbiguousSuccess", "TerminalFailure"):
        assert result in header
    assert "static ClaimConfirmationResult Confirm" in header

    # Client allocation/open failures and malformed 2xx persistence do not prove
    # rejection of the one-time token, so the same claim attempt remains valid.
    assert confirm_body.count("ClaimConfirmationResult::RetryableFailure") >= 3
    persist_failure = confirm_body[confirm_body.index("ProcessTbotClaimConfirmationResponse") :]
    assert "ClaimConfirmationResult::AmbiguousSuccess" in persist_failure

    # Explicit timeout/throttle and all server failures are retryable. Remaining
    # 4xx responses cover auth, expiry, mismatch, and already-confirmed classes.
    assert "status_code == 408" in confirm_body
    assert "status_code == 425" in confirm_body
    assert "status_code == 429" in confirm_body
    assert "status_code >= 500" in confirm_body
    assert "ClaimConfirmationResult::TerminalFailure" in confirm_body
    assert "ClaimConfirmationResult::Confirmed" in confirm_body


def test_claim_confirmation_reporter_is_in_firmware_build_sources():
    cmake = read("main/CMakeLists.txt")

    assert 'provisioning/claim_confirmation_reporter.cc' in cmake
    assert '${CMAKE_CURRENT_SOURCE_DIR}/provisioning' in cmake


def test_expires_at_is_parsed_to_epoch_and_compared_to_a_clock():
    # C4: expires_at must be turned into a comparable epoch and checked against
    # "now" so a stale pending claim self-expires (no infinite WAITING_CONFIRM).
    header = read("main/provisioning/claim_confirmation_reporter.h")
    source = read("main/provisioning/claim_confirmation_reporter.cc")
    parse_body = function_body(source, "bool ParseIso8601UtcToEpoch")
    expired_body = function_body(source, "bool IsPendingTbotClaimExpired")

    assert "bool ParseIso8601UtcToEpoch(" in header
    assert "bool IsPendingTbotClaimExpired(" in header
    # Real parse of the ISO-8601 fields, then a UTC epoch computation.
    assert 'sscanf(iso_utc.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d"' in parse_body
    assert "out_epoch_seconds" in parse_body
    # Expiry compares the parsed deadline to the supplied clock value.
    assert "ParseIso8601UtcToEpoch(claim.expires_at" in expired_body
    assert "now_epoch_seconds >= expires_epoch" in expired_body
