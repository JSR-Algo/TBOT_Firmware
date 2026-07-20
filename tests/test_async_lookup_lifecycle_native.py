import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_async_lookup_lifecycle_host_contract():
    subprocess.run(
        ["bash", str(ROOT / "scripts/run_host_native_async_lookup_lifecycle_test.sh")],
        cwd=ROOT,
        check=True,
    )
