import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_wifi_config_intent_state_native():
    subprocess.run(
        ["bash", str(ROOT / "scripts/run_host_native_wifi_config_intent_state_test.sh")],
        cwd=ROOT,
        check=True,
    )
