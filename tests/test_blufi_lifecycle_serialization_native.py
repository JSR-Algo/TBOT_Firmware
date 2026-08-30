import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_blufi_lifecycle_transactions_serialize_across_drain_gaps():
    subprocess.run(
        ["bash", str(ROOT / "scripts/run_host_native_blufi_lifecycle_serialization_model_test.sh")],
        cwd=ROOT,
        check=True,
    )
