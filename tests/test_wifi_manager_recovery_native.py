import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_wifi_manager_recovery_production_harness():
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_wifi_manager_recovery_test.sh")],
        cwd=ROOT,
        check=True,
    )


def test_wifi_radio_recovery_restorer_native():
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_wifi_radio_recovery_restorer_test.sh")],
        cwd=ROOT,
        check=True,
    )


def test_wifi_scan_recovery_gate_native():
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_wifi_scan_recovery_gate_test.sh")],
        cwd=ROOT,
        check=True,
    )
