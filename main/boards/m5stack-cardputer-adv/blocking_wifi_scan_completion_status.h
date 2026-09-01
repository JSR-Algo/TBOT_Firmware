#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include <wifi_scan_lease_coordinator.h>

class BlockingWifiScanCompletionStatus {
public:
    void Observe(const WifiScanLeaseCoordinator::Lease& lease,
                 uint32_t status) {
        std::lock_guard<std::mutex> lock(mutex_);
        observed_ = Observation{lease, status};
    }

    bool IsObserved(const WifiScanLeaseCoordinator::Lease& lease) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return MatchesLocked(lease);
    }

    bool Succeeded(const WifiScanLeaseCoordinator::Lease& lease) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return MatchesLocked(lease) && observed_->status == 0;
    }

private:
    struct Observation {
        WifiScanLeaseCoordinator::Lease lease;
        uint32_t status = 1;
    };

    bool MatchesLocked(const WifiScanLeaseCoordinator::Lease& lease) const {
        return observed_.has_value() &&
            observed_->lease.owner == lease.owner &&
            observed_->lease.lease_id == lease.lease_id &&
            observed_->lease.driver_incarnation == lease.driver_incarnation;
    }

    mutable std::mutex mutex_;
    std::optional<Observation> observed_;
};
