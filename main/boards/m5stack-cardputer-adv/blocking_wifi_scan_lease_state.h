#pragma once

#include <mutex>
#include <optional>

#include <wifi_scan_lease_coordinator.h>

// Host-testable ownership state for the process-lifetime blocking UI scanner.
class BlockingWifiScanLeaseState {
public:
    enum class CallbackAction {
        kIgnore,
        kWakeWaiter,
        kCompleteWithoutWaiter,
    };

    bool Begin(const WifiScanLeaseCoordinator::Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lease_.has_value()) {
            return false;
        }
        lease_ = lease;
        waiter_attached_ = true;
        callback_claimed_ = false;
        recovery_debt_ = false;
        return true;
    }

    CallbackAction OnCallback(
            const WifiScanLeaseCoordinator::Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesLocked(lease) || callback_claimed_) {
            return CallbackAction::kIgnore;
        }
        callback_claimed_ = true;
        return waiter_attached_ ? CallbackAction::kWakeWaiter
                                : CallbackAction::kCompleteWithoutWaiter;
    }

    bool CallbackClaimed(
            const WifiScanLeaseCoordinator::Lease& lease) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return MatchesLocked(lease) && callback_claimed_;
    }

    bool AbandonUnsubmitted(
            const WifiScanLeaseCoordinator::Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesLocked(lease) || callback_claimed_ || recovery_debt_) {
            return false;
        }
        ResetLocked();
        return true;
    }

    bool DetachWaiterForRecovery(
            const WifiScanLeaseCoordinator::Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesLocked(lease) || callback_claimed_) {
            return false;
        }
        waiter_attached_ = false;
        recovery_debt_ = true;
        return true;
    }

    bool RetainCompletionForRecovery(
            const WifiScanLeaseCoordinator::Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesLocked(lease) || !callback_claimed_) {
            return false;
        }
        waiter_attached_ = false;
        recovery_debt_ = true;
        return true;
    }

    bool FinishNormally(const WifiScanLeaseCoordinator::Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesLocked(lease) || !callback_claimed_) {
            return false;
        }
        ResetLocked();
        return true;
    }

    bool HasDebt(const WifiScanLeaseCoordinator::Lease& lease) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return MatchesLocked(lease) && recovery_debt_;
    }

    bool FinishRecovery(const WifiScanLeaseCoordinator::Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MatchesLocked(lease) || !recovery_debt_) {
            return false;
        }
        ResetLocked();
        return true;
    }

    bool Owns(const WifiScanLeaseCoordinator::Lease& lease) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return MatchesLocked(lease);
    }

    std::optional<WifiScanLeaseCoordinator::Lease> LeaseSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lease_;
    }

private:
    bool MatchesLocked(const WifiScanLeaseCoordinator::Lease& lease) const {
        return lease_.has_value() && lease_->owner == lease.owner &&
               lease_->lease_id == lease.lease_id &&
               lease_->driver_incarnation == lease.driver_incarnation;
    }

    void ResetLocked() {
        lease_.reset();
        waiter_attached_ = false;
        callback_claimed_ = false;
        recovery_debt_ = false;
    }

    mutable std::mutex mutex_;
    std::optional<WifiScanLeaseCoordinator::Lease> lease_;
    bool waiter_attached_ = false;
    bool callback_claimed_ = false;
    bool recovery_debt_ = false;
};
