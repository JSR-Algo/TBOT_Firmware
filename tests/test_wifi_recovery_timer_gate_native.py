import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_wifi_recovery_timer_gate_native():
    subprocess.run(
        ["bash", str(ROOT / "scripts/run_host_native_wifi_recovery_timer_gate_test.sh")],
        cwd=ROOT,
        check=True,
    )
