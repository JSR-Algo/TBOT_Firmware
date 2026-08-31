#pragma once

#include <cstdint>
#include <limits>
#include <mutex>

// Serializes ownership of the process-global ESP-IDF Wi-Fi scan callback.
class WifiScanLeaseCoordinator {
public:
    explicit WifiScanLeaseCoordinator(uint64_t last_lease_id = 0,
                                      uint32_t driver_incarnation = 1)
        : last_lease_id_(last_lease_id),
          driver_incarnation_(driver_incarnation) {}

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
        bool drain_required = false;
    };

    struct DrainDecision {
        bool armed = false;
        uint64_t drain_id = 0;
    };

    AcquireDecision TryAcquire(Owner owner) {
        std::lock_guard<std::mutex> lock(mutex_);
        AcquireDecision result;
        if (phase_ != Phase::kFree) {
            return result;
        }

        uint64_t lease_id = 0;
        if (driver_incarnation_ == 0 || !NextLeaseId(lease_id)) {
            return result;
        }

        current_ = Lease{owner, lease_id, driver_incarnation_};
        phase_ = Phase::kStarting;
        callback_latched_ = false;
        submission_pending_ = true;
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

        if (submission_pending_ &&
            (phase_ == Phase::kStarting || phase_ == Phase::kDraining)) {
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
        if (!Matches(lease) || !submission_pending_) {
            return result;
        }
        submission_pending_ = false;

        if (callback_latched_) {
            phase_ = Phase::kCompleting;
            result.accepted = accepted;
            result.consume_latched = true;
            result.callback_won_error = !accepted;
            return result;
        }

        result.accepted = accepted;
        if (phase_ == Phase::kDraining) {
            result.drain_required = true;
            return result;
        }

        if (accepted) {
            phase_ = Phase::kRunning;
            return result;
        }

        phase_ = Phase::kDraining;
        result.drain_required = true;
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
        if (!Matches(lease) ||
            (phase_ != Phase::kStarting && phase_ != Phase::kRunning)) {
            return false;
        }
        phase_ = Phase::kDraining;
        return true;
    }

    DrainDecision ArmDrainBarrier(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        DrainDecision result;
        if (!Matches(lease) || phase_ != Phase::kDraining ||
            submission_pending_ ||
            last_drain_id_ == std::numeric_limits<uint64_t>::max()) {
            return result;
        }

        ++last_drain_id_;
        armed_drain_id_ = last_drain_id_;
        result.armed = true;
        result.drain_id = armed_drain_id_;
        return result;
    }

    bool CompleteDrain(const Lease& lease, const DrainDecision& drain,
                       bool barrier_drained) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || phase_ != Phase::kDraining ||
            submission_pending_ || !drain.armed || drain.drain_id == 0 ||
            drain.drain_id != armed_drain_id_ || !barrier_drained) {
            return false;
        }
        ReleaseLocked();
        return true;
    }

    bool BeginRecovery(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || submission_pending_ ||
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

        if (!AdvanceDriverIncarnationLocked()) {
            return false;
        }
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

    bool NextLeaseId(uint64_t& lease_id) {
        if (last_lease_id_ == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        ++last_lease_id_;
        lease_id = last_lease_id_;
        return true;
    }

    bool AdvanceDriverIncarnationLocked() {
        if (driver_incarnation_ == std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        ++driver_incarnation_;
        return true;
    }

    void ReleaseLocked() {
        current_ = Lease{};
        callback_latched_ = false;
        submission_pending_ = false;
        armed_drain_id_ = 0;
        phase_ = Phase::kFree;
    }

    std::mutex mutex_;
    Phase phase_ = Phase::kFree;
    Lease current_;
    uint64_t last_lease_id_ = 0;
    uint64_t last_drain_id_ = 0;
    uint64_t armed_drain_id_ = 0;
    uint32_t driver_incarnation_ = 1;
    bool callback_latched_ = false;
    bool submission_pending_ = false;
};
