"""Execute the complete production lesson worker with bounded host queue adapters."""
import os
import json
from pathlib import Path
import shlex
import subprocess

import pytest

from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


def test_cinematic_error_source_guard_is_accessible_on_target(tmp_path):
    commands = ROOT / "build/compile_commands.json"
    if not commands.exists():
        pytest.skip("Configured target compilation database required")
    entry = next(item for item in json.loads(commands.read_text())
                 if item["file"].endswith("/lesson_handler.cc"))
    args = shlex.split(entry["command"])
    if not Path(args[0]).exists():
        pytest.skip("Configured target toolchain required")
    source = tmp_path / "cinematic_source_guard.cc"
    source.write_text(
        '#include "application.h"\n'
        'bool CinematicSourceCurrent(Application& app, const ChatRequestContext& context) {\n'
        '    return app.IsChatLessonRequestCurrent(context);\n'
        '}\n'
    )
    args[args.index("-o") + 1] = str(tmp_path / "cinematic_source_guard.o")
    args[args.index("-c") + 1] = str(source)
    subprocess.run(args, cwd=entry["directory"], check=True, timeout=60)


@pytest.fixture(scope="module")
def worker_binary(tmp_path_factory):
    source_path = Path(os.environ.get("TBOT_WORKER_SOURCE", ROOT / "main/application.cc"))
    worker = method(source_path.read_text(), "void Application::LessonMessageTask")
    fixture = (ROOT / "tests/native/lesson_cinematic_error_worker_test.cc").read_text()
    build = tmp_path_factory.mktemp("cinematic-error-worker")
    source = build / "worker.cc"
    source.write_text(fixture.replace("// PRODUCTION_WORKER", worker))
    binary = build / "worker"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "main"), str(source), "-o", str(binary),
    ], check=True, timeout=60)
    return binary


@pytest.mark.parametrize("scenario", [
    "idle", "after_timeout", "terminal", "owned", "stale_epoch", "after_data",
    "exception_retry", "unknown_exception_retry",
])
def test_cinematic_error_worker_polling(worker_binary, scenario):
    subprocess.run([str(worker_binary), scenario], check=True, timeout=30)
