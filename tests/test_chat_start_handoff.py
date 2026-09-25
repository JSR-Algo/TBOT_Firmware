from pathlib import Path
import os
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_start_admission_values(tmp_path, sanitize):
    binary = tmp_path / "start"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I", str(ROOT / "main"), str(ROOT / "tests/native/chat_start_handoff_test.cc"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
