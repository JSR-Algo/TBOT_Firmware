from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_conversation_playout_drain_native(tmp_path):
    assert (ROOT / "main/audio/conversation_playout_drain.h").exists(), "Production drain state machine missing"
    compiler = shutil.which("clang++") or shutil.which("c++")
    assert compiler, "C++ compiler required"
    executable = tmp_path / "conversation-playout-drain-test"
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(ROOT / "tests/native/conversation_playout_drain_test.cc"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
