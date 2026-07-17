import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_firmware_version_policy_host_contract():
    subprocess.run(
        ["bash", str(ROOT / "scripts/run_host_native_firmware_version_policy_test.sh")],
        cwd=ROOT,
        check=True,
    )
