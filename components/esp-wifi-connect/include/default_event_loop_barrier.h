#pragma once

#include <chrono>

#include "wifi_scan_lease_coordinator.h"

bool DrainDefaultEventLoop(std::chrono::milliseconds timeout);

class DefaultEventLoopScanDrainExecutor {
public:
    static WifiScanLeaseCoordinator::DrainProof Execute(
        WifiScanLeaseCoordinator& coordinator,
        const WifiScanLeaseCoordinator::Lease& lease,
        const WifiScanLeaseCoordinator::DrainDecision& drain);
};
