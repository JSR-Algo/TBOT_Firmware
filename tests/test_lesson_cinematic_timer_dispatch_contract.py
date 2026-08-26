from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main" / "lesson_cinematic_renderer.cc").read_text()


def _function_body(name: str) -> str:
    start = SOURCE.index(name)
    opening = SOURCE.index("{", start)
    depth = 0
    for index in range(opening, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[opening + 1 : index]
    raise AssertionError(f"unterminated function: {name}")


def test_esp_timer_callback_only_dispatches_to_renderer_worker() -> None:
    body = _function_body("void ProductionRendererTimerCallback")

    assert "xTaskNotifyGive" in body
    assert "TickActiveLesson" not in body
    assert ".decode" not in body
    assert ".present" not in body


def test_renderer_worker_owns_all_production_tick_routes() -> None:
    body = _function_body("void ProductionRendererTask")

    assert "ulTaskNotifyTake" in body
    assert "TickActiveLessonLayeredCinematicRenderer" in body
    assert "TickActiveLessonFlattenedCinematicRenderer" in body
    assert "TickActiveLessonCinematicRenderer" in body


def test_v3_rom_jpeg_workspace_stays_in_internal_ram_for_steady_decode() -> None:
    capacity_probe = _function_body("bool InitializeProductionLessonCinematicRenderer")
    production_open = _function_body("bool ProductionOpen")

    expected_caps = "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT"
    assert expected_caps in capacity_probe
    assert expected_caps in production_open
