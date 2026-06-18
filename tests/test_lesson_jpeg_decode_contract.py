"""Regression locks for lesson poster JPEG decoding.

Cloudinary lesson posters are served as JPEG. LVGL is built with PNG decoding in
this firmware config, so the lesson renderer must decode JPEG bytes to RGB565
before wrapping them in LvglAllocatedImage. Passing raw JPEG bytes to LVGL leaves
the robot in the caption-only fallback path.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/lesson_handler.cc"


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
    body = function_body(source, "std::unique_ptr<LvglImage> FetchLessonImage")

    assert '#include "jpeg_to_image.h"' in source
    assert "bool IsJpegImage" in source
    assert "IsJpegImage(data, content_length)" in body
    assert "jpeg_to_image(" in body
    assert "LV_COLOR_FORMAT_RGB565" in body
    assert "std::make_unique<LvglAllocatedImage>(decoded_data, decoded_len" in body
    assert "heap_caps_free(data);" in body
