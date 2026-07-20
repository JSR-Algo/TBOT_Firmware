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


def test_new_ota_app_marks_valid_before_network_version_check():
    source = read("main/application.cc")
    activation_body = function_body(source, "void Application::ActivationTask")

    assert "ota_->MarkCurrentVersionValid();" in activation_body
    assert activation_body.index("ota_->MarkCurrentVersionValid();") < activation_body.index("CheckNewVersion();")
    # Unclaimed devices must skip OTA HTTPS while BLE advertising stays up —
    # otherwise heap is exhausted and the UI freezes on "Loading setup...".
    assert "if (!IsDeviceClaimed())" in activation_body
    assert activation_body.index("if (!IsDeviceClaimed())") < activation_body.index("CheckNewVersion();")
    assert "skip OTA/bootstrap HTTPS" in activation_body

    # Also short-circuit BEFORE xTaskCreate: with BLE+Wi-Fi the largest free
    # internal block is often ~7KB, so an 8KB activation task never starts and
    # the robot freezes on Activating / "Loading setup...".
    source_full = source
    assert "skip activation worker" in source_full
    assert "Failed to create activation task" in source_full


def test_ota_download_rejects_images_larger_than_update_partition_before_writing():
    source = read("main/ota.cc")
    upgrade_body = function_body(source, "bool Ota::Upgrade")

    assert "content_length > update_partition->size" in upgrade_body
    assert "Firmware image too large" in upgrade_body
    assert upgrade_body.index("content_length > update_partition->size") < upgrade_body.index("esp_ota_begin")


def test_ota_download_logs_new_image_version_and_final_byte_count():
    source = read("main/ota.cc")
    upgrade_body = function_body(source, "bool Ota::Upgrade")

    assert "New firmware image version" in upgrade_body
    assert "new_app_info.version" in upgrade_body
    assert "Firmware download complete" in upgrade_body
    assert "total_read" in upgrade_body
    assert "content_length" in upgrade_body


def test_ota_download_rejects_short_reads_before_esp_ota_end():
    source = read("main/ota.cc")
    upgrade_body = function_body(source, "bool Ota::Upgrade")

    assert "total_read != content_length" in upgrade_body
    assert "Firmware download size mismatch" in upgrade_body
    assert upgrade_body.index("total_read != content_length") < upgrade_body.index("esp_ota_end(update_handle)")

def test_ota_check_version_does_not_retry_stale_ephemeral_nvs_url():
    source = read("main/ota.cc")
    url_builder_body = function_body(source, "std::vector<std::string> BuildCheckVersionUrls")

    assert "IsEphemeralEndpoint(configured_url)" in url_builder_body
    assert '"http://"' not in url_builder_body
    assert "configured_url.substr" not in url_builder_body
    assert "add_unique(configured_url)" in url_builder_body
    assert "CONFIG_OTA_URL" in url_builder_body


def test_ota_check_version_tries_canonical_before_stale_distinct_configured_url():
    source = read("main/ota.cc")
    url_builder_body = function_body(source, "std::vector<std::string> BuildCheckVersionUrls")

    stale_start = url_builder_body.index("if (IsStaleConfiguredEndpoint")
    canonical_push = url_builder_body.index("add_unique(canonical_url)", stale_start)
    configured_push = url_builder_body.index("add_unique(configured_url)", canonical_push)
    assert canonical_push < configured_push
    assert "configured_url != canonical_url" in source
    assert "IsEphemeralEndpoint(configured_url)" in url_builder_body


def test_ota_endpoint_precedence_keeps_public_override_but_recovers_stale_local_urls():
    source = read("main/ota.cc")
    url_builder_body = function_body(source, "std::vector<std::string> BuildCheckVersionUrls")

    assert "IsStaleConfiguredEndpoint(configured_url, canonical_url)" in url_builder_body
    assert "add_unique(configured_url);" in url_builder_body
    assert "add_unique(canonical_url);" in url_builder_body
    assert "configured_url != canonical_url" in source
    assert "IsPrivateOrLocalHost" in source
    assert 'host == "localhost"' in source
    assert 'host.ends_with(".local")' in source
    assert "first_octet == 10" in source
    assert "first_octet == 172" in source
    assert "first_octet == 192 && second_octet == 168" in source


def test_ota_check_version_has_bounded_http_lifetime_and_closes_every_opened_response():
    source = read("main/ota.cc")
    setup_body = function_body(source, "std::unique_ptr<Http> Ota::SetupHttp")
    check_body = function_body(source, "esp_err_t Ota::CheckVersion")

    assert "static constexpr int kHttpTimeoutMs = 8000" in read("main/ota.h")
    assert "http->SetTimeout(timeout_ms);" in setup_body
    assert "ScopedHttpClose close_http(*http);" in check_body
    assert check_body.index("ScopedHttpClose close_http(*http);") < check_body.index(
        "http->GetStatusCode()"
    )
    assert "close_http.Close();" in check_body
    assert "http->Close();" not in check_body


def test_ota_check_version_uses_one_deadline_across_fallback_connect_headers_and_body():
    source = read("main/ota.cc")
    check_body = function_body(source, "esp_err_t Ota::CheckVersion")

    assert "check_deadline_us" in check_body
    assert "esp_timer_get_time()" in check_body
    assert "RemainingCheckTimeoutMs(check_deadline_us)" in check_body
    assert "SetupHttp(RemainingCheckTimeoutMs(check_deadline_us))" in check_body
    assert check_body.count(
        "http->SetTimeout(RemainingCheckTimeoutMs(check_deadline_us));"
    ) >= 2


def test_ota_check_falls_back_after_any_invalid_attempt_and_persists_only_valid_recovery():
    source = read("main/ota.cc")
    check_body = function_body(source, "esp_err_t Ota::CheckVersion")

    loop_idx = check_body.index("for (const auto& url : urls)")
    persist_idx = check_body.index("PersistRecoveredOtaUrl(successful_url)")
    parse_idx = check_body.index("cJSON_Parse(response_body.c_str())", loop_idx)
    valid_idx = check_body.index("IsValidCheckVersionResponse(candidate_root", parse_idx)
    success_idx = check_body.index("successful_url = url", valid_idx)

    assert check_body.index("ScopedHttpClose close_http(*http)", loop_idx) < parse_idx
    assert check_body.index("status_code != 200", loop_idx) < parse_idx
    assert check_body.count("continue;") >= 3
    assert parse_idx < valid_idx < success_idx < persist_idx
    assert "cJSON_Delete(candidate_root);" in check_body[valid_idx:success_idx]
    assert "if (root == nullptr)" in check_body
    assert persist_idx < check_body.index("has_activation_code_ = false")


def test_ota_semantically_validates_firmware_before_selecting_or_persisting_endpoint():
    source = read("main/ota.cc")
    check_body = function_body(source, "esp_err_t Ota::CheckVersion")
    validator = function_body(source, "bool IsValidCheckVersionResponse")

    assert '#include "firmware_version_policy.h"' in source
    assert "EvaluateFirmwareResponse" in validator
    assert "current_version" in validator
    assert "should_download" in validator
    assert "selected_should_download" in check_body
    assert check_body.index("IsValidCheckVersionResponse") < check_body.index(
        "successful_url = url"
    )
    assert check_body.index("successful_url = url") < check_body.index(
        "PersistRecoveredOtaUrl(successful_url)"
    )
    assert "std::stoi" not in source
    assert "ParseVersion" not in source


def test_ota_check_logs_do_not_expose_endpoint_or_device_identity():
    source = read("main/ota.cc")
    setup_body = function_body(source, "std::unique_ptr<Http> Ota::SetupHttp")
    check_body = function_body(source, "esp_err_t Ota::CheckVersion")
    recovered_body = function_body(source, "void PersistRecoveredOtaUrl")

    log_lines = "\n".join(
        line for body in (setup_body, check_body, recovered_body)
        for line in body.splitlines()
        if "ESP_LOG" in line
    )
    assert "url.c_str()" not in log_lines
    assert "configured_url.c_str()" not in log_lines
    assert "serial_number_.c_str()" not in log_lines

def test_ota_claim_reset_clears_local_ownership_once_per_nonce():
    source = read("main/ota.cc")
    check_body = function_body(source, "esp_err_t Ota::CheckVersion")

    assert 'cJSON_GetObjectItem(root, "claim_reset")' in check_body
    assert 'cJSON_GetObjectItem(claim_reset, "local_claim")' in check_body
    assert 'cJSON_GetObjectItem(claim_reset, "nonce")' in check_body
    assert 'Settings reset_state("tbot_reset", true);' in check_body
    assert 'reset_state.GetString("claim_reset_nonce") != reset_nonce' in check_body

    assert 'Settings claim_state("tbot_claim", true);' in check_body
    assert 'claim_state.SetInt("confirmed", 0);' in check_body
    assert 'claim_state.SetInt("factory_test", 0);' in check_body

    assert 'Settings backend_settings("backend", true);' in check_body
    assert 'backend_settings.SetString("device_id", "");' in check_body
    assert 'backend_settings.SetString("device_secret", "");' in check_body
    assert 'backend_settings.SetInt("release_pending", 0);' in check_body

    assert 'Settings websocket_settings("websocket", true);' in check_body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in check_body
    assert 'websocket_settings.SetString("token", "");' in check_body
    assert 'websocket_settings.SetString("url", "");' in check_body
    assert 'websocket_settings.SetString("claim_device_id", "");' in check_body

    assert 'reset_state.SetString("claim_reset_nonce", reset_nonce);' in check_body
    assert 'esp_restart();' in check_body

def test_ota_claim_reset_does_not_log_reset_nonce_or_tokens():
    source = read("main/ota.cc")
    check_body = function_body(source, "esp_err_t Ota::CheckVersion")
    reset_region = check_body[
        check_body.index('cJSON_GetObjectItem(root, "claim_reset")'):
        check_body.index('has_server_time_ = false')
    ]

    for line in reset_region.splitlines():
        if "ESP_LOG" not in line:
            continue
        assert "reset_nonce" not in line
        assert "bootstrap_token" not in line
        assert "device_secret" not in line
        assert "token" not in line.lower()
