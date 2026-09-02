import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_wifi_scan_recovery_executor_native():
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_wifi_scan_recovery_executor_test.sh")],
        cwd=ROOT,
        check=True,
    )
