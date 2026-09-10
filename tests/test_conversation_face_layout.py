from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / 'main/display/lcd_display.cc'


def test_conversation_face_fills_available_area_and_preserves_bars():
    source = SOURCE.read_text()
    layout = source.split('void LcdDisplay::UpdateConversationFaceLayout() {', 1)[1].split('void LcdDisplay::SetEmotion', 1)[0]
    assert 'height_ - header - footer' not in layout
    assert 'lv_obj_set_size(emoji_image_, width_, height_)' in layout
    assert 'LV_IMAGE_ALIGN_COVER' in layout
    assert 'LV_ALIGN_CENTER, 0, 0' in layout
    for bar in ('top_bar_', 'status_bar_', 'bottom_bar_'):
        assert f'lv_obj_move_foreground({bar})' in layout
    emotion = source.split('void LcdDisplay::SetEmotion', 1)[1].split('void LcdDisplay::', 1)[0]
    assert 'UpdateConversationFaceLayout();' in emotion


def test_transparent_white_overlays_survive_theme_changes():
    source = SOURCE.read_text()
    styling = source.split('void LcdDisplay::StyleConversationOverlays() {', 1)[1].split('void LcdDisplay::', 1)[0]
    assert 'LV_OPA_TRANSP' in styling
    assert 'lv_color_white()' in styling
    for label in ('status_label_', 'notification_label_', 'chat_message_label_', 'network_label_', 'battery_label_', 'mute_label_'):
        assert label in styling
    theme = source.split('void LcdDisplay::SetTheme(', 1)[1].split('void LcdDisplay::', 1)[0]
    assert 'StyleConversationOverlays();' in theme
