from __future__ import annotations

import json
import re
import struct
import unicodedata
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_MANAGER = ROOT / "main/app_manager.cc"
MENU_HEADER = ROOT / "main/ui/menu_ui.h"
MENU_CONFIG = ROOT / "sdcard/tbot_ui/menu_config.json"
VI_LOCALE = ROOT / "main/assets/locales/vi-VN/language.json"
PRODUCTION_FONT = ROOT / "managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf"
ASSET_BUILDER = ROOT / "scripts/build_default_assets.py"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def production_font_codepoints() -> set[int]:
    data = PRODUCTION_FONT.read_bytes()
    table_count = struct.unpack_from(">H", data, 4)[0]
    tables = {
        data[offset : offset + 4]: struct.unpack_from(">II", data, offset + 8)
        for offset in range(12, 12 + table_count * 16, 16)
    }
    cmap_offset, _ = tables[b"cmap"]
    cmap_count = struct.unpack_from(">H", data, cmap_offset + 2)[0]
    codepoints: set[int] = set()

    for record in range(cmap_count):
        platform, encoding, subtable_relative = struct.unpack_from(
            ">HHI", data, cmap_offset + 4 + record * 8
        )
        if platform not in (0, 3) or (platform == 3 and encoding not in (1, 10)):
            continue
        subtable = cmap_offset + subtable_relative
        format_number = struct.unpack_from(">H", data, subtable)[0]
        if format_number == 4:
            segment_count = struct.unpack_from(">H", data, subtable + 6)[0] // 2
            ends = struct.unpack_from(f">{segment_count}H", data, subtable + 14)
            starts_offset = subtable + 16 + segment_count * 2
            starts = struct.unpack_from(f">{segment_count}H", data, starts_offset)
            for start, end in zip(starts, ends):
                if end != 0xFFFF:
                    codepoints.update(range(start, end + 1))
        elif format_number == 12:
            group_count = struct.unpack_from(">I", data, subtable + 12)[0]
            for group in range(group_count):
                start, end, _ = struct.unpack_from(
                    ">III", data, subtable + 16 + group * 12
                )
                codepoints.update(range(start, end + 1))

    return codepoints


def production_font_advance_px(text: str, pixel_size: int = 20) -> float:
    data = PRODUCTION_FONT.read_bytes()
    table_count = struct.unpack_from(">H", data, 4)[0]
    tables = {
        data[offset : offset + 4]: struct.unpack_from(">II", data, offset + 8)
        for offset in range(12, 12 + table_count * 16, 16)
    }
    head_offset, _ = tables[b"head"]
    hhea_offset, _ = tables[b"hhea"]
    hmtx_offset, _ = tables[b"hmtx"]
    cmap_offset, _ = tables[b"cmap"]
    units_per_em = struct.unpack_from(">H", data, head_offset + 18)[0]
    metric_count = struct.unpack_from(">H", data, hhea_offset + 34)[0]

    cmap_count = struct.unpack_from(">H", data, cmap_offset + 2)[0]
    format_four = None
    for record in range(cmap_count):
        platform, encoding, relative = struct.unpack_from(
            ">HHI", data, cmap_offset + 4 + record * 8
        )
        candidate = cmap_offset + relative
        if platform == 3 and encoding == 1 and struct.unpack_from(">H", data, candidate)[0] == 4:
            format_four = candidate
            break
    assert format_four is not None

    segment_count = struct.unpack_from(">H", data, format_four + 6)[0] // 2
    ends_offset = format_four + 14
    starts_offset = ends_offset + segment_count * 2 + 2
    deltas_offset = starts_offset + segment_count * 2
    ranges_offset = deltas_offset + segment_count * 2

    def glyph_id(codepoint: int) -> int:
        for index in range(segment_count):
            end = struct.unpack_from(">H", data, ends_offset + index * 2)[0]
            start = struct.unpack_from(">H", data, starts_offset + index * 2)[0]
            if start <= codepoint <= end:
                delta = struct.unpack_from(">h", data, deltas_offset + index * 2)[0]
                range_value_offset = ranges_offset + index * 2
                range_value = struct.unpack_from(">H", data, range_value_offset)[0]
                if range_value == 0:
                    return (codepoint + delta) & 0xFFFF
                glyph = struct.unpack_from(
                    ">H",
                    data,
                    range_value_offset + range_value + (codepoint - start) * 2,
                )[0]
                return (glyph + delta) & 0xFFFF if glyph else 0
        return 0

    advances = []
    last_advance = struct.unpack_from(">H", data, hmtx_offset + (metric_count - 1) * 4)[0]
    for character in text:
        glyph = glyph_id(ord(character))
        advance = (
            struct.unpack_from(">H", data, hmtx_offset + glyph * 4)[0]
            if glyph < metric_count
            else last_advance
        )
        advances.append(advance)
    return sum(advances) * pixel_size / units_per_em


def displayed_vietnamese_strings() -> list[str]:
    app_manager = read(APP_MANAGER)
    menu_header = read(MENU_HEADER)
    menu_config = json.loads(read(MENU_CONFIG))
    vi_locale = json.loads(read(VI_LOCALE))

    defaults = re.findall(r'std::string\s+\w+\s*=\s*"([^"]*)"', menu_header)
    loaded_keys = (
        "title",
        "subtitle",
        "chatbox",
        "game",
        "music",
        "music_empty",
        "left_right_hint",
        "both_hint",
        "hold_hint",
        "slave_ok",
        "slave_wait",
        "sd_ok",
        "sd_fail",
    )
    configured = [menu_config["vi"][key] for key in loaded_keys]
    app_hints = re.findall(r'MakeHintLabel\(g_overlay,\s*"([^"]+)"', app_manager)
    return defaults + configured + app_hints + [vi_locale["strings"]["OPEN_TBOT_APP"]]


def test_production_menu_strings_are_valid_utf8_nfc_json():
    for path in (APP_MANAGER, MENU_HEADER, MENU_CONFIG, VI_LOCALE):
        text = read(path)
        assert unicodedata.is_normalized("NFC", text), path

    assert json.loads(read(MENU_CONFIG))["vi"]
    assert json.loads(read(VI_LOCALE))["strings"]


def test_production_font_covers_every_displayed_vietnamese_character():
    builder = read(ASSET_BUILDER)
    app_manager = read(APP_MANAGER)
    assert "replace('basic', 'common') + '.bin'" in builder
    assert "lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN)" in app_manager

    supported = production_font_codepoints()
    missing: dict[str, list[str]] = {}

    for text in displayed_vietnamese_strings():
        unsupported = sorted(
            {
                f"{character} (U+{ord(character):04X})"
                for character in text
                if ord(character) > 0x7F and ord(character) not in supported
            }
        )
        if unsupported:
            missing[text] = unsupported

    assert not missing, missing


def test_music_and_speed_hints_use_bounded_wrapped_two_line_layout():
    source = read(APP_MANAGER)

    helper = re.search(
        r"lv_obj_t\* MakeHintLabel\(.*?\n\}",
        source,
        re.DOTALL,
    )
    assert helper, "shared bounded hint label helper is required"
    body = helper.group(0)
    assert "lv_obj_set_width(hint, 440);" in body
    assert "lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);" in body
    assert "lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);" in body

    hint_calls = re.findall(r'MakeHintLabel\(g_overlay,\s*"([^"]+)",', source)
    assert len(hint_calls) == 2
    for escaped_text in hint_calls:
        assert escaped_text.count(r"\n") == 1, escaped_text
        assert all(
            production_font_advance_px(line) <= 440
            for line in escaped_text.split(r"\n")
        ), escaped_text

    music_state = re.search(
        r"g_music_state_label = MakeLabel.*?"
        r"lv_obj_align\(g_music_state_label, LV_ALIGN_BOTTOM_MID, 0, (-?\d+)\);",
        source,
        re.DOTALL,
    )
    assert music_state
    assert int(music_state.group(1)) <= -60


def test_sd_menu_strings_and_fallback_defaults_share_the_same_display_contract():
    source = read(ROOT / "main/ui/menu_ui.cc")
    config = json.loads(read(MENU_CONFIG))["vi"]
    defaults = dict(
        re.findall(
            r'std::string\s+(\w+)\s*=\s*"([^"]*)"',
            read(MENU_HEADER),
        )
    )

    for key, fallback in defaults.items():
        assert f'GetStr(vi, "{key}", &s.{key});' in source
        assert key in config
        assert config[key] == fallback
