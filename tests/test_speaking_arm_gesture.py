from pathlib import Path
import shutil
import subprocess
import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("fixture", ["speaking_arm_gesture", "speaking_arm_dispatch", "speaking_arm_transport"])
def test_speaking_arm_gesture_native(tmp_path, fixture):
    assert (ROOT / "main/speaking_arm_gesture.h").exists(), "Production gesture controller missing"
    compiler = shutil.which("clang++") or shutil.which("c++")
    assert compiler, "C++ compiler required"
    executable = tmp_path / "speaking-arm-gesture-test"
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(ROOT / f"tests/native/{fixture}_test.cc"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


@pytest.mark.parametrize("method", ["RunCourseModeHilStopAndRest", "RunCourseModeHilSafeMotion"])
def test_hil_explicit_motion_invalidates_speaking_ownership_before_uart(method):
    source = (ROOT / "main/application.cc").read_text()
    body = source.split(f"bool Application::{method}(", 1)[1].split("\n}", 1)[0]
    assert "speaking_arm_dispatch_.Cancel();" in body
    assert body.index("speaking_arm_dispatch_.Cancel();") < body.index("robot_uart_.")
