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
