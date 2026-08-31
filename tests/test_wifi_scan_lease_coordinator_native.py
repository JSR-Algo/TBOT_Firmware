import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_wifi_scan_lease_coordinator_host_model():
    header = (
        ROOT
        / "components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"
    ).read_text()
    assert "WifiScanLeaseProofFactory" not in header
    assert "DrainDecision" not in header
    assert "DrainProof" not in header
    assert "ArmDrainBarrier" not in header
    assert "CompleteDrain" not in header
    assert "DefaultEventLoopScanDrainExecutor" not in header
    assert "drain_id" not in header
    assert "friend class WifiScanRecoveryExecutor;" in header
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_wifi_scan_lease_coordinator_test.sh")],
        cwd=ROOT,
        check=True,
    )
