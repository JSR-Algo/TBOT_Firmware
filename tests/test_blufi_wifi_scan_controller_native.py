import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_blufi_wifi_scan_controller_host_model():
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_blufi_wifi_scan_controller_test.sh")],
        cwd=ROOT,
        check=True,
    )
