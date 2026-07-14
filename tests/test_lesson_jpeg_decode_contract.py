"""Regression locks for lesson poster JPEG decoding.

Cloudinary lesson posters are served as JPEG. LVGL is built with PNG decoding in
this firmware config, so the lesson renderer must decode JPEG bytes to RGB565
before wrapping them in LvglAllocatedImage. Passing raw JPEG bytes to LVGL leaves
the robot in the caption-only fallback path.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/lesson_handler.cc"
DECODER_SOURCE = ROOT / "main/display/lvgl_display/jpg/jpeg_to_image.c"
DECODER_HEADER = ROOT / "main/display/lvgl_display/jpg/jpeg_to_image.h"
ENCODER_SOURCE = ROOT / "main/display/lvgl_display/jpg/image_to_jpeg.cpp"
COMPONENT_MANIFEST = ROOT / "main/idf_component.yml"
SDKCONFIG = ROOT / "sdkconfig"


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
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_lesson_image_fetch_decodes_jpeg_to_rgb565_before_lvgl_wrap():
    source = SOURCE.read_text(encoding="utf-8")
    decode_body = function_body(source, "std::unique_ptr<LvglImage> DecodeLessonImageBytes")
    fetch_body = function_body(source, "std::unique_ptr<LvglImage> FetchLessonImage")
    local_body = function_body(source, "std::unique_ptr<LvglImage> FetchLessonLocalImage")

    assert '#include "jpeg_to_image.h"' in source
    assert "bool IsJpegImage" in source
    assert "IsJpegImage(data, content_length)" in decode_body
    assert "jpeg_to_image_with_caps(" in decode_body
    assert "LV_COLOR_FORMAT_RGB565" in decode_body
    assert "std::make_unique<LvglAllocatedImage>(decoded_data, decoded_len" in decode_body
    assert "heap_caps_free(data);" in decode_body
    assert "DecodeLessonImageBytes(data, content_length, \"lesson image fetch\")" in fetch_body
    assert "DecodeLessonImageBytes(data, content_length, \"lesson image file\")" in local_body


def test_lesson_jpeg_decoder_uses_rom_tjpg_with_psram_work_and_output_buffers():
    source = SOURCE.read_text(encoding="utf-8")
    decoder = DECODER_SOURCE.read_text(encoding="utf-8")
    header = DECODER_HEADER.read_text(encoding="utf-8")
    decode_body = function_body(source, "std::unique_ptr<LvglImage> DecodeLessonImageBytes")
    caps_body = function_body(decoder, "esp_err_t jpeg_to_image_with_caps")
    software_body = function_body(decoder, "static esp_err_t decode_with_rom_jpeg")
    default_body = function_body(decoder, "esp_err_t jpeg_to_image(")

    assert "jpeg_to_image_with_caps(" in header
    assert "LessonAllocationCaps(decoded_len)" not in decode_body
    assert "LessonAllocationCaps(content_length)" in decode_body
    assert '#include "jpeg_decoder.h"' in decoder
    assert '#include "esp_jpeg_dec.h"' not in decoder
    assert "decode_with_rom_jpeg" in caps_body
    assert "output_caps" in caps_body
    assert "heap_caps_aligned_calloc(16, 1, decoded_size, output_caps)" in software_body
    assert "heap_caps_malloc(JPEG_ROM_WORK_BUFFER_SIZE, output_caps)" in software_body
    assert ".working_buffer = work_buf" in software_body
    assert "esp_jpeg_get_image_info" in software_body
    assert "esp_jpeg_decode" in software_body
    assert "src_len > UINT32_MAX" in software_body
    assert "out_info.output_len > UINT32_MAX" in software_body
    assert "heap_caps_free(out_buf)" in software_body
    assert "heap_caps_free(work_buf)" in software_body
    assert "decode_with_hardware_jpeg" in default_body
    assert "decode_with_hardware_jpeg" not in caps_body


def test_rom_decoder_component_is_direct_and_new_jpeg_remains_encoder_only():
    decoder = DECODER_SOURCE.read_text(encoding="utf-8")
    encoder = ENCODER_SOURCE.read_text(encoding="utf-8")
    manifest = COMPONENT_MANIFEST.read_text(encoding="utf-8")
    sdkconfig = SDKCONFIG.read_text(encoding="utf-8")

    assert "espressif/esp_jpeg:" in manifest
    assert "CONFIG_JD_USE_ROM=y" in sdkconfig
    assert "jpeg_dec_open" not in decoder
    assert "jpeg_dec_process" not in decoder
    assert '#include "esp_jpeg_enc.h"' in encoder
    assert "jpeg_enc_open" in encoder
    assert "espressif/esp_new_jpeg:" in manifest
