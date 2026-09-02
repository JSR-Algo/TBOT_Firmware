#pragma once
#include "wifi_scan_lease_coordinator.h"
class WifiScanRecoveryExecutor {
public:
    WifiScanLeaseCoordinator::RecoveryProof Execute(
        const WifiScanLeaseCoordinator::RecoveryDecision& recovery);
};
