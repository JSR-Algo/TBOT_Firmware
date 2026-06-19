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

def test_lesson_object_foreground_layer_exists_and_stacks_above_background():
    source = SOURCE.read_text(encoding="utf-8")
    header = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
    base_header = (ROOT / "main/display/lvgl_display/lvgl_display.h").read_text(encoding="utf-8")

    assert "lesson_object_" in header
    assert "lesson_object_cached_" in header
    assert "virtual void SetLessonObject(std::unique_ptr<LvglImage> image)" in base_header
    assert "virtual void SetLessonObject(std::unique_ptr<LvglImage> image) override;" in header

    bodies = []
    offset = 0
    while True:
        try:
            start = source.index("void LcdDisplay::SetLessonObject", offset)
        except ValueError:
            break
        bodies.append(function_body(source[start:], "void LcdDisplay::SetLessonObject"))
        offset = start + 1

    assert len(bodies) >= 2, "both LCD UI variants must implement the foreground lesson object"
    for body in bodies:
        assert "lv_image_set_src(lesson_object_" in body
        assert "lv_obj_remove_flag(lesson_object_, LV_OBJ_FLAG_HIDDEN)" in body
        assert "lv_obj_move_foreground(lesson_object_)" in body
        assert "lv_obj_move_foreground(top_bar_)" in body
        assert "lv_obj_move_foreground(status_bar_)" in body

def test_lesson_step_fetches_and_draws_teaching_object_foreground_layer():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert 'const char* object_src = Str(Obj(to, "asset"), "src")' in step_branch
    assert "FetchLessonImage(object_src)" in step_branch
    assert "SetLessonObject" in step_branch
    assert "object_drew" in step_branch
    assert "SetPreviewImage" not in step_branch

def test_lesson_stop_and_caption_only_steps_clear_foreground_object():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    stop_branch = body[body.index('strcmp(type, "lesson_stop") == 0') : body.index('strcmp(type, "lesson_step") != 0')]
    assert "SetLessonObject(nullptr)" in stop_branch

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert "clear_object" in step_branch
    assert "SetLessonObject(nullptr)" in step_branch
