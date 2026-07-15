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


def test_lesson_background_uses_cover_scale_not_width_only_fit():
    source = SOURCE.read_text(encoding="utf-8")
    bodies = []
    offset = 0
    while True:
        try:
            start = source.index("void LcdDisplay::SetLessonBackground", offset)
        except ValueError:
            break
        bodies.append(function_body(source[start:], "void LcdDisplay::SetLessonBackground"))
        offset = start + 1

    assert "LessonImageCoverScale" in source
    assert len(bodies) >= 2
    for body in bodies:
        assert "LessonImageCoverScale(" in body
        assert "256 * width_ / img_dsc->header.w" not in body

def test_lesson_object_and_robot_overlay_use_safe_composition_layout():
    source = SOURCE.read_text(encoding="utf-8")

    assert "kLessonObjectMaxWidthPercent = 46" in source
    assert "kLessonObjectMaxHeightPercent = 50" in source
    assert "kLessonObjectYOffsetDivisor = 12" in source
    assert "kLessonRobotMaxWidthPercent = 32" in source
    assert "kLessonRobotMaxHeightPercent = 34" in source
    assert "kLessonRobotBottomInsetDivisor = 6" in source

    object_bodies = []
    overlay_bodies = []
    offset = 0
    while True:
        try:
            start = source.index("void LcdDisplay::SetLessonObject", offset)
        except ValueError:
            break
        object_bodies.append(function_body(source[start:], "void LcdDisplay::SetLessonObject"))
        offset = start + 1

    offset = 0
    while True:
        try:
            start = source.index("void LcdDisplay::SetLessonRobotOverlay", offset)
        except ValueError:
            break
        overlay_bodies.append(function_body(source[start:], "void LcdDisplay::SetLessonRobotOverlay"))
        offset = start + 1

    assert len(object_bodies) >= 2
    assert len(overlay_bodies) >= 2
    for body in object_bodies:
        assert "LessonImageFitScale(" in body
        assert "kLessonObjectMaxWidthPercent" in body
        assert "kLessonObjectMaxHeightPercent" in body
        assert "LV_ALIGN_CENTER, 0, -height_ / kLessonObjectYOffsetDivisor" in body
        assert "width_ * 3 / 5" not in body
        assert "height_ / 12)" not in body
    for body in overlay_bodies:
        assert "LessonImageFitScale(" in body
        assert "kLessonRobotMaxWidthPercent" in body
        assert "kLessonRobotMaxHeightPercent" in body
        assert "LV_ALIGN_BOTTOM_LEFT, width_ / 24, -height_ / kLessonRobotBottomInsetDivisor" in body
        assert "width_ / 3" not in body
        assert "-height_ / 12" not in body

def test_lesson_stop_clears_layers_and_uses_terminal_reason_cue():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    stop_branch = body[body.index('strcmp(type, "lesson_stop") == 0') : body.index('strcmp(type, "lesson_step") != 0')]
    assert "SetLessonBackground(nullptr)" in stop_branch
    assert 'Str(body, "reason")' in stop_branch
    assert '"FAILED"' in stop_branch
    assert '"CANCELLED"' in stop_branch
    assert '"Hoàn thành bài học"' in stop_branch
    assert '"Bài học đã dừng"' in stop_branch
    assert "Lang::Strings::ERROR" in stop_branch
    assert "stop_emotion" in stop_branch
    assert "Lang::Sounds::OGG_SUCCESS" in stop_branch
    assert "Lang::Sounds::OGG_POPUP" in stop_branch
    assert "Lang::Sounds::OGG_EXCLAMATION" in stop_branch
    assert "display->ClearChatMessages()" in stop_branch
    assert 'display->SetChatMessage("assistant", stop_message)' in stop_branch
    assert stop_branch.index("display->ClearChatMessages()") < stop_branch.index(
        'display->SetChatMessage("assistant", stop_message)'
    )


def test_lesson_lifecycle_cues_clear_stale_chat_copy():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    start_branch = body[body.index('if (strcmp(type, "lesson_start") == 0)') : body.index('if (strcmp(type, "lesson_pause") == 0)')]
    pause_branch = body[body.index('if (strcmp(type, "lesson_pause") == 0)') : body.index('if (strcmp(type, "lesson_resume") == 0)')]
    resume_branch = body[body.index('if (strcmp(type, "lesson_resume") == 0)') : body.index('if (strcmp(type, "lesson_stop") == 0)')]

    assert "start_display->ClearChatMessages()" in start_branch
    assert "display->ClearChatMessages()" in pause_branch
    assert "display->ClearChatMessages()" in resume_branch
    assert "SetEmotion(" not in start_branch
    assert "SetEmotion(" not in pause_branch
    assert "SetEmotion(" not in resume_branch


def test_lesson_lifecycle_aborts_active_speech_on_pause_or_terminal():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    failure_helper = body[body.index("auto end_lesson_after_failure") : body.index("const bool is_prepare")]
    pause_branch = body[body.index('if (strcmp(type, "lesson_pause") == 0)') : body.index('if (strcmp(type, "lesson_resume") == 0)')]
    stop_branch = body[body.index('if (strcmp(type, "lesson_stop") == 0)') : body.index('if (strcmp(type, "lesson_error") == 0)')]

    assert "abort_speaking_if_needed" in body
    assert "app.GetDeviceState() == kDeviceStateSpeaking" in body
    assert "app.AbortSpeaking(kAbortReasonNone);" in body
    assert "abort_speaking_if_needed();" in failure_helper
    assert "abort_speaking_if_needed();" in pause_branch
    assert "abort_speaking_if_needed();" in stop_branch


def test_lesson_step_uses_background_layer_not_transient_preview_image():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert "SetLessonBackground" in step_branch
    assert "SetPreviewImage" not in step_branch

def test_lesson_step_clears_stale_chat_before_caption_draw():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert "display->ClearChatMessages();" in step_branch
    assert step_branch.index("display->ClearChatMessages();") < step_branch.index("display->SetLessonCaption(cap.c_str());")

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

def test_lesson_robot_overlay_layer_exists_and_stacks_above_object():
    source = SOURCE.read_text(encoding="utf-8")
    header = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
    base_header = (ROOT / "main/display/lvgl_display/lvgl_display.h").read_text(encoding="utf-8")

    assert "lesson_robot_overlay_" in header
    assert "lesson_robot_overlay_cached_" in header
    assert "virtual void SetLessonRobotOverlay(std::unique_ptr<LvglImage> image)" in base_header
    assert "virtual void SetLessonRobotOverlay(std::unique_ptr<LvglImage> image) override;" in header

    bodies = []
    offset = 0
    while True:
        try:
            start = source.index("void LcdDisplay::SetLessonRobotOverlay", offset)
        except ValueError:
            break
        bodies.append(function_body(source[start:], "void LcdDisplay::SetLessonRobotOverlay"))
        offset = start + 1

    assert len(bodies) >= 2, "both LCD UI variants must implement the robot overlay lesson layer"
    for body in bodies:
        assert "lv_image_set_src(lesson_robot_overlay_" in body
        assert "lv_obj_remove_flag(lesson_robot_overlay_, LV_OBJ_FLAG_HIDDEN)" in body
        assert "lv_obj_move_foreground(lesson_robot_overlay_)" in body
        assert "lv_obj_move_foreground(lesson_object_)" not in body
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

def test_lesson_step_fetches_and_draws_robot_overlay_image_layer():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert 'const char* overlay_src = Str(Obj(ro, "asset"), "src")' in step_branch
    assert "FetchLessonImage(overlay_src)" in step_branch
    assert "SetLessonRobotOverlay" in step_branch
    assert "overlay_drew" in step_branch
    assert "SetPreviewImage" not in step_branch

def test_lesson_step_rejects_missing_required_scene_or_poster_before_ack():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    guard_idx = step_branch.index('if (scene == nullptr || bg == nullptr || to == nullptr || ro == nullptr ||')
    error_idx = step_branch.index('MakeErrorBody("LESSON_FRAME_INVALID"', guard_idx)
    emit_error_idx = step_branch.index('emit(root, "lesson_error", eb)', error_idx)
    fetch_idx = step_branch.index("FetchLessonImage(poster_src)", emit_error_idx)
    ack_idx = step_branch.index("emit_ack(root, sequence")
    fatal_guard = step_branch[guard_idx:error_idx]

    assert 'Blank(poster_src)' in step_branch
    assert 'Blank(object_src)' not in fatal_guard
    assert 'Blank(overlay_src)' not in fatal_guard
    assert 'const bool has_object_src = !Blank(object_src)' in step_branch
    assert 'const bool has_overlay_src = !Blank(overlay_src)' in step_branch
    assert 'if (lvgl_display != nullptr && has_object_src && object_clear_ok)' in step_branch
    assert 'if (lvgl_display != nullptr && has_overlay_src && overlay_clear_ok)' in step_branch
    assert guard_idx < error_idx < emit_error_idx < fetch_idx < ack_idx

def test_lesson_step_renders_layers_back_to_front_before_ack():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    background_idx = step_branch.index("SetLessonBackground(std::move(*holder))")
    object_idx = step_branch.index("SetLessonObject(std::move(*holder))")
    overlay_idx = step_branch.index("SetLessonRobotOverlay(std::move(*holder))")
    caption_idx = step_branch.index("display->SetLessonCaption(cap.c_str())")
    ack_idx = step_branch.index("emit_ack(root, sequence")
    success_branch = step_branch[step_branch.index('display->SetStatus("Đang học...")') : ack_idx]
    no_content_branch = step_branch[step_branch.index("if (!has_visible_content)") : step_branch.index("return;", step_branch.index("if (!has_visible_content)"))]

    assert "display->SetEmotion(" not in success_branch
    assert "display->SetEmotion(" not in no_content_branch
    assert background_idx < object_idx < overlay_idx < caption_idx < ack_idx

def test_lesson_step_uses_dedicated_caption_overlay_not_chat_bubble():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")
    display_header = (ROOT / "main/display/display.h").read_text(encoding="utf-8")
    lcd_header = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
    lcd_source = SOURCE.read_text(encoding="utf-8")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert "virtual void SetLessonCaption(const char* content)" in display_header
    assert "virtual void SetLessonCaption(const char* content) override;" in lcd_header
    assert "void LcdDisplay::SetLessonCaption(const char* content)" in lcd_source
    assert "display->SetLessonCaption(cap.c_str())" in step_branch
    assert 'display->SetChatMessage("assistant", cap.c_str())' not in step_branch

def test_lesson_caption_overlay_wraps_for_small_tft_prompts():
    source = SOURCE.read_text(encoding="utf-8")
    setup_body = function_body(source, "void LcdDisplay::SetupUI")
    caption_setup = setup_body[setup_body.index("lesson_caption_bar_ = lv_obj_create") : setup_body.index("lv_obj_add_flag(lesson_caption_bar_", setup_body.index("lesson_caption_bar_ = lv_obj_create"))]
    caption_body = function_body(source, "void LcdDisplay::SetLessonCaption")

    assert "kLessonCaptionWidthPercent = 92" in source
    assert "kLessonCaptionLabelWidthPercent = 86" in source
    assert "kLessonCaptionMaxHeightPercent = 24" in source
    assert "kLessonCaptionBottomInsetDivisor = 60" in source
    assert "lv_obj_set_width(lesson_caption_bar_, width_ * kLessonCaptionWidthPercent / 100)" in caption_setup
    assert "lv_obj_set_height(lesson_caption_bar_, height_ * kLessonCaptionMaxHeightPercent / 100)" in caption_setup
    assert "lv_obj_set_width(lesson_caption_label_, width_ * kLessonCaptionLabelWidthPercent / 100)" in caption_setup
    assert "lv_label_set_long_mode(lesson_caption_label_, LV_LABEL_LONG_WRAP)" in caption_setup
    assert "LV_LABEL_LONG_DOT" not in caption_setup
    assert "lv_obj_align(lesson_caption_bar_, LV_ALIGN_BOTTOM_MID, 0, -height_ / kLessonCaptionBottomInsetDivisor)" in caption_setup
    assert "lv_obj_align(lesson_caption_bar_, LV_ALIGN_BOTTOM_MID, 0, -height_ / kLessonCaptionBottomInsetDivisor)" in caption_body
    assert "LV_SIZE_CONTENT" not in caption_setup

def test_lesson_step_always_updates_caption_to_clear_stale_prompt():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    schedule_branch = step_branch[step_branch.index("Schedule([display") : step_branch.index("});", step_branch.index("Schedule([display"))]
    assert "display->SetLessonCaption(cap.c_str())" in schedule_branch
    assert "if (!cap.empty())" not in schedule_branch

def test_lesson_mode_hides_face_without_emoji_box_in_wechat_layout():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void LcdDisplay::SetLessonMode")
    active_branch = body[body.index("if (active)") : body.index("} else {")]
    missing_surface_guard = body[body.index("if (emoji_box_ == nullptr") : body.index("if (active)")]

    assert "emoji_image_ == nullptr" in missing_surface_guard
    assert "emoji_label_ == nullptr" in missing_surface_guard
    assert "return;" in missing_surface_guard
    assert "emoji_image_ != nullptr" in active_branch
    assert "emoji_label_ != nullptr" in active_branch
    assert "lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN)" in active_branch
    assert "lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN)" in active_branch

def test_lesson_listening_thinking_face_reappears_over_lesson_scene():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void LcdDisplay::SetEmotion")

    assert 'strcmp(emotion, "thinking") == 0' in body
    thinking_branch = body[body.index('strcmp(emotion, "thinking") == 0') :]

    assert "emoji_box_ != nullptr" in thinking_branch
    assert "lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN)" in thinking_branch
    assert "lv_obj_move_foreground(emoji_box_)" in thinking_branch
    assert "lesson_caption_bar_ != nullptr" in thinking_branch
    assert "lv_obj_move_foreground(lesson_caption_bar_)" in thinking_branch


def test_failed_emotion_gif_load_falls_back_to_neutral_face():
    source = SOURCE.read_text(encoding="utf-8")
    body = function_body(source, "void LcdDisplay::SetEmotion")

    gif_branch = body[body.index("if (image->IsGif())") : body.index("#if CONFIG_USE_WECHAT_MESSAGE_STYLE")]
    failed_load = gif_branch[gif_branch.index("} else {") : gif_branch.index("} else {", gif_branch.index("} else {") + 1)]

    assert 'GetEmojiImage("neutral")' in failed_load
    assert "fallback->image_dsc()" in failed_load
    assert "lv_image_set_src(emoji_image_, fallback->image_dsc())" in failed_load
    assert "lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN)" in failed_load
    assert "lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN)" in failed_load


def test_lesson_step_ack_degraded_reflects_all_three_image_layers():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    poster_idx = step_branch.index("bool poster_drew = background_plan.reused")
    object_idx = step_branch.index("bool object_drew = object_plan.reused")
    overlay_idx = step_branch.index("bool overlay_drew = overlay_plan.reused")
    render_degraded_idx = step_branch.index("const bool render_degraded =")
    degraded_idx = step_branch.index("const bool degraded =")
    ack_idx = step_branch.index("emit_ack(root, sequence, rendered, degraded, nullptr, true, render_elapsed_ms, telemetry)")

    assert poster_idx < object_idx < overlay_idx < render_degraded_idx < degraded_idx < ack_idx
    render_degraded_expr = step_branch[
        render_degraded_idx : step_branch.index(";", render_degraded_idx)
    ]
    assert "poster_drew" in render_degraded_expr
    assert "object_drew" in render_degraded_expr
    assert "overlay_drew" in render_degraded_expr
    degraded_expr = step_branch[degraded_idx : step_branch.index(";", degraded_idx)]
    assert "motion_degraded" in degraded_expr
    assert "render_degraded" in degraded_expr
    assert "false" not in step_branch[ack_idx : step_branch.index(";", ack_idx)]

def test_lesson_step_ack_rendered_requires_display_surface():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    display_idx = step_branch.index("Display* display = base_display;")
    rendered_idx = step_branch.index("const bool rendered = display != nullptr && dynamic_cast<NoDisplay*>(display) == nullptr;")
    ack_idx = step_branch.index("emit_ack(root, sequence, rendered, degraded, nullptr, true, render_elapsed_ms, telemetry)")

    assert display_idx < rendered_idx < ack_idx
    assert "emit_ack(root, sequence, /*rendered*/ true, degraded)" not in step_branch

def test_lesson_step_ack_reports_measured_render_elapsed_after_layer_work():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') :]
    start_idx = step_branch.index("const int64_t render_start_us = esp_timer_get_time();")
    poster_idx = step_branch.index("FetchLessonImage(poster_src)")
    object_idx = step_branch.index("FetchLessonImage(object_src)")
    overlay_idx = step_branch.index("FetchLessonImage(overlay_src)")
    schedule_idx = step_branch.index("Schedule([display, lvgl_display")
    elapsed_idx = step_branch.index("const int64_t render_elapsed_ms =")
    ack_idx = step_branch.index("emit_ack(root, sequence, rendered, degraded, nullptr, true, render_elapsed_ms, telemetry)")
    log_idx = step_branch.index("renderElapsedMs=%ld")

    assert start_idx < poster_idx < object_idx < overlay_idx < schedule_idx < elapsed_idx < ack_idx < log_idx
    raw_elapsed_idx = step_branch.index("const int64_t render_elapsed_us = esp_timer_get_time() - render_start_us;")
    assert schedule_idx < raw_elapsed_idx < elapsed_idx
    assert "render_elapsed_us < 0 ? 0 : render_elapsed_us / 1000" in step_branch
    assert "kLessonRenderElapsedMaxMs" in step_branch[elapsed_idx : step_branch.index(";", elapsed_idx)]
    assert 'cJSON_AddNumberToObject(b, "renderElapsedMs", static_cast<double>(render_elapsed_ms));' in body
    assert "static_cast<long>(render_elapsed_ms)" in step_branch[ack_idx:]
    assert "renderElapsedMs" not in step_branch[:start_idx]

def test_lesson_stop_and_caption_only_steps_clear_foreground_object():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    stop_branch = body[body.index('strcmp(type, "lesson_stop") == 0') : body.index('strcmp(type, "lesson_step") != 0')]
    assert "SetLessonObject(nullptr)" in stop_branch

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert "clear_object" in step_branch
    assert "SetLessonObject(nullptr)" in step_branch

def test_lesson_stop_and_caption_only_steps_clear_robot_overlay():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    stop_branch = body[body.index('strcmp(type, "lesson_stop") == 0') : body.index('strcmp(type, "lesson_step") != 0')]
    assert "SetLessonRobotOverlay(nullptr)" in stop_branch

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    assert "clear_overlay" in step_branch
    assert "SetLessonRobotOverlay(nullptr)" in step_branch

def test_lesson_error_clears_layers_and_shows_friendly_failure_without_ack_loop():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    failure_display = body[body.index("auto show_lesson_failure_display") : body.index("const bool is_prepare")]
    error_branch = body[body.index('strcmp(type, "lesson_error") == 0') : body.index('strcmp(type, "lesson_step") != 0')]
    assert "SetLessonBackground(nullptr)" in failure_display
    assert "SetLessonObject(nullptr)" in failure_display
    assert "SetLessonRobotOverlay(nullptr)" in failure_display
    assert 'display->SetEmotion("sad")' in failure_display
    assert 'display->SetChatMessage("assistant", "Bài học chưa tải được.")' in failure_display
    assert "Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);" in failure_display
    assert "end_lesson_after_failure();" in error_branch
    assert "show_lesson_failure_display();" in failure_display
    assert "emit_ack" not in error_branch

def test_lesson_step_caption_prefers_child_question_then_authored_prompt_over_asset_label():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")

    step_branch = body[body.index('const cJSON* scene = Obj(body, "scene")') : body.index("ESP_LOGI(TAG, \"lesson_step rendered")]
    prompt_idx = step_branch.index('const char* prompt = Str(body, "prompt")')
    fallback_idx = step_branch.index('const cJSON* card = Obj(to, "primitiveFallbackCard")')
    assert prompt_idx < fallback_idx
    assert 'const char* story_ask = Str(Obj(body, "storyBeat"), "ask")' in step_branch
    assert "std::string prompt_caption;" in step_branch
    assert "if (!passive) prompt_caption = CaptionCandidate(story_ask);" in step_branch
    assert "if (prompt_caption.empty()) prompt_caption = CaptionCandidate(prompt);" in step_branch
    assert "if (!passive && prompt_caption.empty()) prompt_caption = CaptionCandidate(story_ask);" in step_branch
    assert "const bool has_caption_prompt = !prompt_caption.empty()" in step_branch
    assert "if (has_caption_prompt) caption = prompt_caption;" in step_branch
    assert "if (!has_caption_prompt)" in step_branch

def test_lesson_image_fetch_supports_sd_pack_paths_before_network_fetch():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    fetch_body = function_body(source, "std::unique_ptr<LvglImage> FetchLessonImage")
    local_body = function_body(source, "std::unique_ptr<LvglImage> FetchLessonLocalImage")
    path_body = function_body(source, "std::string LessonLocalPath")

    trim_idx = fetch_body.index("TrimAsciiWhitespace(fetch_url);")
    local_idx = fetch_body.index('strncmp(canonical_url, "sd://", 5) == 0')
    network_idx = fetch_body.index("Board::GetInstance().GetNetwork()")
    assert trim_idx < local_idx
    assert local_idx < network_idx
    assert 'return FetchLessonLocalImage(canonical_url);' in fetch_body

    assert 'return ValidateLessonPackPath(std::string("/sdcard/") + tail);' in path_body
    assert 'return ValidateLessonPackPath(std::string("/") + tail);' in path_body
    assert "TrimAsciiWhitespace(local_url);" in path_body
    assert 'strncmp(canonical_url, "file://", 7) == 0' in path_body

    assert 'std::string path = LessonLocalPath(url);' in local_body
    assert 'std::string fs_path = LessonPackFileSystemPath(path);' in local_body
    assert 'FILE* fp = fopen(fs_path.c_str(), "rb")' in local_body
    assert 'fread(data, 1, content_length, fp)' in local_body
    assert 'DecodeLessonImageBytes(data, content_length, "lesson image file")' in local_body

def test_lesson_host_sd_root_override_preserves_canonical_wire_validation():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    resolver_body = function_body(source, "std::string LessonPackFileSystemPath")
    local_ready_body = function_body(source, "bool LessonLocalFileReady")

    assert '#ifdef TBOT_HOST_NATIVE_COVERAGE' in resolver_body
    assert 'std::getenv("TBOT_HOST_LESSON_ASSET_ROOT")' in resolver_body
    assert 'canonical_path.rfind(kLessonAssetPackRoot, 0) == 0' in resolver_body
    assert 'canonical_path.substr(strlen(kLessonAssetPackRoot))' in resolver_body

    path_idx = local_ready_body.index('std::string path = LessonLocalPath(url);')
    resolve_idx = local_ready_body.index('std::string fs_path = LessonPackFileSystemPath(path);')
    open_idx = local_ready_body.index('FILE* fp = fopen(fs_path.c_str(), "rb")')
    assert path_idx < resolve_idx < open_idx

def test_lesson_sd_pack_path_normalizes_esp_sdcard_uri_without_double_prefix():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    path_body = function_body(source, "std::string LessonLocalPath")

    sd_branch = path_body[path_body.index('if (strncmp(canonical_url, "sd://", 5) == 0)') : path_body.index('if (strncmp(canonical_url, "file://", 7) == 0')]
    esp_uri_guard = 'if (strncmp(tail, "sdcard/", 7) == 0) return ValidateLessonPackPath(std::string("/") + tail);'
    generic_sd_prefix = 'return ValidateLessonPackPath(std::string("/sdcard/") + tail);'

    assert esp_uri_guard in sd_branch
    assert generic_sd_prefix in sd_branch
    assert sd_branch.index(esp_uri_guard) < sd_branch.index(generic_sd_prefix)

def test_lesson_local_paths_are_confined_to_lesson_asset_pack_root():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    path_body = function_body(source, "std::string LessonLocalPath")
    validator_body = function_body(source, "std::string ValidateLessonPackPath")

    assert 'constexpr const char* kLessonAssetPackRoot = "/sdcard/tbot/lesson-assets/";' in source
    assert 'path.rfind(kLessonAssetPackRoot, 0) != 0' in validator_body
    assert 'path.find("..") != std::string::npos' in validator_body
    assert 'path.find("\\\\") != std::string::npos' in validator_body
    assert 'return ValidateLessonPackPath(std::string(canonical_url + 7));' in path_body

def test_lesson_image_fetch_has_single_image_memory_cap_for_http_and_sd():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    assert "constexpr size_t kMaxLessonImageBytes = 512 * 1024;" in source
    assert "static_cast<size_t>(file_size) > kMaxLessonImageBytes" in source
    assert "cap >= kMaxLessonImageBytes" in source
    assert "new_cap > kMaxLessonImageBytes" in source

def test_lesson_image_fetch_rejects_known_content_length_over_memory_cap_before_alloc():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    fetch_body = function_body(source, "std::unique_ptr<LvglImage> FetchLessonImage")
    known_length_branch = fetch_body[
        fetch_body.index("if (content_length > 0)") : fetch_body.index("} else {")
    ]

    guard = "if (content_length > kMaxLessonImageBytes)"
    alloc = "heap_caps_malloc(content_length, LessonAllocationCaps(content_length))"
    assert guard in known_length_branch
    assert known_length_branch.index(guard) < known_length_branch.index(alloc)


def test_lesson_png_decode_is_no_cache_psram_owned_and_frees_compressed_input():
    handler = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    png = function_body(handler, "std::unique_ptr<LvglImage> DecodeLessonPngBytes")

    assert "args.no_cache = true" in png
    assert "lv_image_decoder_close(&decoder)" in png
    assert "heap_caps_free(compressed)" in png
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in png
    assert "mutable_decoded->data = nullptr" in png
    assert "mutable_decoded->unaligned_data = nullptr" in png
    assert "LvglAllocatedImage" in png
    assert "LessonPngAllocatorScope allocator_scope" in png
    assert "handlers_->buf_malloc_cb = LessonPngBufferMalloc" in handler
    assert "g_lesson_png_allocator_active = true" in handler
    assert "g_lesson_png_allocator_active = false" in handler
    assert "lv_lock()" in handler and "lv_unlock()" in handler
    assert "LODEPNG_NO_COMPILE_ALLOCATORS" in (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
    assert "heap_caps_realloc(pointer, size, LessonAllocationCaps(size))" in handler


def test_lesson_duplicate_ack_replays_cached_full_body_before_reconstructing_fields():
    source = (ROOT / "main/lesson_handler.cc").read_text(encoding="utf-8")
    body = function_body(source, "void Application::HandleLessonMessage")
    duplicate = body[body.index("if (sequence <= g_session.last_in_sequence)") : body.index("if (is_step && g_session.paused)")]

    full_body = duplicate.index("g_session.ack_history.rbegin()")
    exact_emit = duplicate.index('emit(root, "lesson_ack", replay_body)')
    reconstructed = duplicate.index("const bool re_rendered")
    assert full_body < exact_emit < reconstructed


def test_schedule_and_wait_cancels_only_pending_callbacks_and_waits_for_running_work():
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    body = function_body(source, "bool Application::ScheduleAndWait")

    assert "enum Status { kPending, kRunning, kDone, kCancelled }" in body
    assert "compare_exchange_strong(expected, WaitState::kRunning)" in body
    assert "compare_exchange_strong(expected, WaitState::kCancelled)" in body
    running = body.index("if (expected == WaitState::kRunning)")
    wait_done = body.index("xSemaphoreTake(state->done, portMAX_DELAY)", running)
    result = body.index("state->result.load()", wait_done)
    assert running < wait_done < result
