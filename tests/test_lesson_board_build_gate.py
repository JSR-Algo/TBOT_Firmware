from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MAIN_CMAKE = ROOT / "main" / "CMakeLists.txt"

LESSON_RUNTIME_SOURCES = {
    "lesson_asset_cache_evict.cc",
    "lesson_asset_download_staging.cc",
    "lesson_asset_http_transfer.cc",
    "lesson_asset_pack_activation.cc",
    "lesson_asset_sample_url_policy.cc",
    "lesson_asset_storage_coordinator.cc",
    "lesson_asset_sync_path_policy.cc",
    "lesson_asset_sync_attestation.cc",
    "lesson_handler.cc",
    "lesson_tvideo_template.cc",
    "lesson_heap_probe.cc",
    "lesson_renderer_memory_heap_hook.cc",
    "lesson_renderer_memory_probe.cc",
    "lesson_motion_presets.cc",
    "lesson_layer_state.cc",
}


def test_lesson_runtime_sources_are_lcdwiki_only():
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    base_sources = cmake.split("# Add board common files", 1)[0]

    for source in LESSON_RUNTIME_SOURCES:
        assert f'"{source}"' not in base_sources, (
            f"{source} must not compile into every board"
        )

    gate_marker = cmake.index("# The TBOT lesson renderer")
    gate_start = cmake.index("if(CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P)", gate_marker)
    gate_end = cmake.index("endif()", gate_start)
    lcdwiki_gate = cmake[gate_start:gate_end]
    for source in LESSON_RUNTIME_SOURCES:
        assert f'"{source}"' in lcdwiki_gate, (
            f"{source} must remain enabled for lcdwiki-es3c35p"
        )


def test_lesson_runtime_entry_points_use_the_same_board_gate():
    guarded_files = (
        "application.cc",
        "mcp_server.cc",
        "protocols/websocket_protocol.cc",
        "display/lcd_display.cc",
    )

    for relative_path in guarded_files:
        source = (ROOT / "main" / relative_path).read_text(encoding="utf-8")
        assert "#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P" in source, (
            f"{relative_path} must compile TBOT lesson entry points only for lcdwiki-es3c35p"
        )


def test_custom_lodepng_allocators_are_enabled_only_with_lesson_handler():
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    allocator_override = re.compile(
        r"if\(CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P\)\s+"
        r"idf_component_get_property\(TBOT_LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB\)\s+"
        r"target_compile_definitions\(\$\{TBOT_LVGL_COMPONENT_LIB\} PRIVATE "
        r"LODEPNG_NO_COMPILE_ALLOCATORS=1\)\s+"
        r"endif\(\)"
    )

    assert allocator_override.search(cmake), (
        "LVGL must keep its built-in LodePNG allocators on boards that do not "
        "compile lesson_handler.cc"
    )


def test_lcd_display_constructs_lesson_probe_only_on_lcdwiki():
    header = (ROOT / "main" / "display" / "lcd_display.h").read_text(
        encoding="utf-8"
    )
    guarded_probe = re.compile(
        r"#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P\s+"
        r"LessonRendererMemoryProbe lesson_renderer_memory_probe_;\s+"
        r"#endif"
    )

    assert guarded_probe.search(header), (
        "non-lesson boards must not construct a probe whose implementation is "
        "part of the LCDWiki-only lesson runtime"
    )
