"""Execute production LCD/LVGL methods with instrumented host LVGL objects.

The shim checks ownership and callback order; it does not establish TFT output.
"""
from pathlib import Path
import subprocess

import pytest

from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def display_binary(tmp_path_factory):
    lcd = (ROOT / "main/display/lcd_display.cc").read_text()
    base = (ROOT / "main/display/lvgl_display/lvgl_display.cc").read_text()
    methods = [method(base, name) for name in (
        "LvglDisplay::LvglDisplay()", "void LvglDisplay::SetStatus",
        "void LvglDisplay::ShowNotification(const char*", "void LvglDisplay::UpdateStatusBar",
    )]
    methods += [method(lcd, name) for name in (
        "bool LcdDisplay::PresentLessonFramebuffer",
        "void LcdDisplay::SetLessonMode", "void LcdDisplay::SetEmotion",
        "void LcdDisplay::SetLessonCaption", "void LcdDisplay::SetHideSubtitle",
    )]
    for name in ("void LcdDisplay::SetChatMessage", "void LcdDisplay::SetPreviewImage", "void LcdDisplay::ClearChatMessages"):
        first = lcd.index(name)
        methods.append(method(lcd[first + len(name):], name))
    # Compile the alternate layout's clear body on a derived host adapter too.
    methods.append(method(lcd, "void LcdDisplay::ClearChatMessages").replace(
        "void LcdDisplay::ClearChatMessages", "void WechatLcdDisplay::ClearChatMessages", 1))
    fixture = (ROOT / "tests/native/lesson_display_ownership_test.cc").read_text()
    build = tmp_path_factory.mktemp("lesson-display")
    source = build / "display.cc"
    source.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(methods)))
    binary = build / "display"
    subprocess.run([
        "c++", "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "main"), str(source), "-o", str(binary),
    ], check=True, timeout=60)
    return binary


@pytest.mark.parametrize("scenario", [
    "entry", "chat", "status", "notification", "notification_timer", "network",
    "preview", "preview_expiry", "subtitle", "emotion_race", "glyph_race",
    "idempotent", "lesson_caption", "battery_policy", "chat_restored",
    "present_claim", "present_invalid", "present_allocation_failure",
    "clear_caption_fallback", "status_race", "notification_race", "chat_race", "preview_race",
    "hide_caption_fallback", "hide_caption_dedicated", "subtitle_restore",
    "wechat_clear",
])
def test_display_excludes_chat_during_lesson(display_binary, scenario):
    subprocess.run([str(display_binary), scenario], check=True, timeout=30)
