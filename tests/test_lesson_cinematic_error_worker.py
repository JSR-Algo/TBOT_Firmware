"""Execute the complete production lesson worker with bounded host queue adapters."""
import os
from pathlib import Path
import subprocess

import pytest

from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


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
