#pragma once

#include "wifi_scan_lease_coordinator.h"

#include <mutex>

// Performs the one process-wide driver reset that can prove scan callback drain.
class WifiScanRecoveryExecutor {
public:
    WifiScanRecoveryExecutor() = default;
    WifiScanRecoveryExecutor(const WifiScanRecoveryExecutor&) = delete;
    WifiScanRecoveryExecutor& operator=(const WifiScanRecoveryExecutor&) = delete;

    WifiScanLeaseCoordinator::RecoveryProof Execute(
        const WifiScanLeaseCoordinator::RecoveryDecision& recovery);

private:
    static std::mutex& ProcessMutex();
};
