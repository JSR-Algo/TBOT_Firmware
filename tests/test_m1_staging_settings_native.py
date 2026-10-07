import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_staging_routing_preserves_original_profile_across_reopen(tmp_path):
    compiler = os.environ.get("CXX", "clang++")
    includes = ["-I", str(ROOT / "tests/native_stubs/m1_settings"), "-I", str(ROOT / "main")]
    flags = ["-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *includes]
    objects = []
    for enabled, name in [(0, "ProductionSettings"), (1, "StagingSettings")]:
        output = tmp_path / (name + ".o")
        subprocess.run([compiler, *flags, f"-DCONFIG_TBOT_M1_STAGING={enabled}", f"-DSettings={name}",
                        "-c", str(ROOT / "main/settings.cc"), "-o", str(output)], check=True)
        objects.append(str(output))
    binary = tmp_path / "settings-test"
    subprocess.run([compiler, *flags, str(ROOT / "tests/native/m1_settings_test.cc"), *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    assert subprocess.run([str(binary), "failed-write"], capture_output=True).returncode != 0
