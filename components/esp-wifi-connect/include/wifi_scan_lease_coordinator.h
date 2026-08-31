#pragma once

#include <cstdint>
#include <mutex>

// Serializes ownership of the process-global ESP-IDF Wi-Fi scan callback.
class WifiScanLeaseCoordinator {
public:
    enum class Owner : uint8_t {
        kStation,
        kConfigAp,
        kBlufi,
        kBlockingUi,
    };

    enum class Phase : uint8_t {
        kFree,
        kStarting,
        kRunning,
        kCompleting,
        kDraining,
        kRecovering,
    };

    struct Lease {
        Owner owner = Owner::kStation;
        uint64_t lease_id = 0;
        uint32_t driver_incarnation = 0;
    };

    struct AcquireDecision {
        bool acquired = false;
        Lease lease;
    };

    struct CallbackDecision {
        bool consume_now = false;
        bool deferred_until_commit = false;
    };

    struct CommitDecision {
        bool accepted = false;
        bool consume_latched = false;
        bool released = false;
        bool callback_won_error = false;
    };

    AcquireDecision TryAcquire(Owner owner) {
        std::lock_guard<std::mutex> lock(mutex_);
        AcquireDecision result;
        if (phase_ != Phase::kFree) {
            return result;
        }

        current_ = Lease{owner, NextLeaseId(), driver_incarnation_};
        phase_ = Phase::kStarting;
        callback_latched_ = false;
        result.acquired = true;
        result.lease = current_;
        return result;
    }

    CallbackDecision ObserveScanDone(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        CallbackDecision result;
        if (!Matches(lease)) {
            return result;
        }

        if (phase_ == Phase::kStarting) {
            if (!callback_latched_) {
                callback_latched_ = true;
                result.deferred_until_commit = true;
            }
            return result;
        }

        if (phase_ == Phase::kRunning || phase_ == Phase::kDraining) {
            phase_ = Phase::kCompleting;
            result.consume_now = true;
        }
        return result;
    }

    CommitDecision CommitSubmission(const Lease& lease, bool accepted) {
        std::lock_guard<std::mutex> lock(mutex_);
        CommitDecision result;
        if (!Matches(lease) || phase_ != Phase::kStarting) {
            return result;
        }

        if (callback_latched_) {
            phase_ = Phase::kCompleting;
            result.accepted = accepted;
            result.consume_latched = true;
            result.callback_won_error = !accepted;
            return result;
        }

        if (accepted) {
            phase_ = Phase::kRunning;
            result.accepted = true;
            return result;
        }

        ReleaseLocked();
        result.released = true;
        return result;
    }

    bool FinishCompletion(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || phase_ != Phase::kCompleting) {
            return false;
        }
        ReleaseLocked();
        return true;
    }

    bool BeginDrain(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || phase_ != Phase::kRunning) {
            return false;
        }
        phase_ = Phase::kDraining;
        return true;
    }

    bool CompleteDrain(const Lease& lease, bool barrier_drained) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || phase_ != Phase::kDraining ||
            !barrier_drained) {
            return false;
        }
        ReleaseLocked();
        return true;
    }

    bool BeginRecovery(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) ||
            (phase_ != Phase::kRunning && phase_ != Phase::kDraining)) {
            return false;
        }
        phase_ = Phase::kRecovering;
        return true;
    }

    bool CompleteRecovery(const Lease& lease, bool driver_ready,
                          bool barrier_drained) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || phase_ != Phase::kRecovering || !driver_ready ||
            !barrier_drained) {
            return false;
        }

        AdvanceDriverIncarnationLocked();
        ReleaseLocked();
        return true;
    }

private:
    bool Matches(const Lease& lease) const {
        return phase_ != Phase::kFree && lease.owner == current_.owner &&
               lease.lease_id != 0 && lease.lease_id == current_.lease_id &&
               lease.driver_incarnation != 0 &&
               lease.driver_incarnation == current_.driver_incarnation;
    }

    uint64_t NextLeaseId() {
        ++last_lease_id_;
        if (last_lease_id_ == 0) {
            ++last_lease_id_;
        }
        return last_lease_id_;
    }

    void AdvanceDriverIncarnationLocked() {
        ++driver_incarnation_;
        if (driver_incarnation_ == 0) {
            ++driver_incarnation_;
        }
    }

    void ReleaseLocked() {
        current_ = Lease{};
        callback_latched_ = false;
        phase_ = Phase::kFree;
    }

    std::mutex mutex_;
    Phase phase_ = Phase::kFree;
    Lease current_;
    uint64_t last_lease_id_ = 0;
    uint32_t driver_incarnation_ = 1;
    bool callback_latched_ = false;
};
