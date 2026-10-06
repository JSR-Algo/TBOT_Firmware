"""Renderer v6 decodes on the lesson worker (prepare) and the lesson_cinematic task (tick).

On the LCDWiki robot (BE08 R14) a v6 prepare used 47,496 bytes of stack; on 32 KB it
overflowed into the PSRAM heap and corrupted the scene controller. v6 builds must give
both tasks headroom; builds without v6 keep their existing 32 KB stacks.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MEASURED_V6_PREPARE_STACK_BYTES = 47_496
REQUIRED_BYTES = 48 * 1024


def _v6_branch_value(text: str, name: str) -> tuple[int, int]:
    match = re.search(
        rf"#if CONFIG_TBOT_LESSON_RENDERER_V6\n([^#]*\b{name}\b[^#]*)#else\n([^#]*\b{name}\b[^#]*)#endif", text)
    assert match, f"{name}: no CONFIG_TBOT_LESSON_RENDERER_V6 stack branch"
    values = []
    for branch in match.groups():
        found = re.search(rf"{name}\s*=\s*([0-9]+)(?:\s*\*\s*([0-9]+))?", branch)
        assert found, f"{name} missing in a branch"
        values.append(int(found.group(1)) * int(found.group(2) or 1))
    return values[0], values[1]


def test_lesson_worker_stack_covers_v6_prepare():
    app = (ROOT / "main/application.cc").read_text()
    v6, default = _v6_branch_value(app, "kLessonMessageWorkerStackDepth")
    assert v6 >= REQUIRED_BYTES > MEASURED_V6_PREPARE_STACK_BYTES
    assert default == 32768


def test_cinematic_tick_stack_covers_v6_decode():
    renderer = (ROOT / "main/lesson_cinematic_renderer.cc").read_text()
    v6, default = _v6_branch_value(renderer, "kProductionRendererStackBytes")
    assert v6 >= REQUIRED_BYTES
    assert default == 32 * 1024
    assert 'xTaskCreateWithCaps(ProductionRendererTask, "lesson_cinematic", kProductionRendererStackBytes' in renderer
