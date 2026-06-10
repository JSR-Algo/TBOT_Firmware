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
