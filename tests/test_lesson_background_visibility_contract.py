"""Regression locks for visible lesson poster backgrounds.

Lesson frames can fetch and decode posters successfully while the LCD still shows
the idle/chat UI if the poster object sits behind opaque containers. The lesson
background must be a visible screen-level layer, with the main UI surfaces made
transparent when a poster is active.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/display/lcd_display.cc"


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


def test_lesson_background_layer_is_not_hidden_behind_opaque_chat_surfaces():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void LcdDisplay::SetLessonBackground")

    assert "LV_OPA_TRANSP" in body
    assert "container_" in body
    assert "content_" in body
    assert "lv_obj_move_foreground(top_bar_)" in body
    assert "lv_obj_move_foreground(status_bar_)" in body


def test_clearing_lesson_background_restores_theme_backgrounds():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void LcdDisplay::SetLessonBackground")

    clear_branch = body[body.index("if (image == nullptr)") : body.index("lesson_background_cached_ = std::move(image)")]
    assert "current_theme_" in clear_branch
    assert "lvgl_theme->background_color()" in clear_branch
    assert "lvgl_theme->chat_background_color()" in clear_branch


def test_lesson_background_is_persistent_not_preview_timer_based():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void LcdDisplay::SetLessonBackground")

    assert "preview_timer_" not in body
    assert "PREVIEW_IMAGE_DURATION_MS" not in body
    assert "lv_image_set_scale(lesson_background_" in body


def test_lesson_stop_clears_persistent_background_on_firmware():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    stop_branch = body[body.index('strcmp(type, "lesson_stop") == 0') : body.index('strcmp(type, "lesson_step") != 0')]
    assert "SetLessonBackground(nullptr)" in stop_branch
    assert 'display->SetEmotion("neutral")' in stop_branch


def test_lesson_step_uses_background_layer_not_transient_preview_image():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert "SetLessonBackground" in step_branch
    assert "SetPreviewImage" not in step_branch
