import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_blufi_advertising_callback_ledger_choreographies():
    subprocess.run(
        ["bash", str(ROOT / "scripts/run_host_native_blufi_advertising_ledger_test.sh")],
        cwd=ROOT,
        check=True,
    )
