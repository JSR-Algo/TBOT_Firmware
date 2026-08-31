#pragma once

#include <cstdint>
#include <limits>
#include <mutex>

class WifiScanRecoveryExecutor;

// Serializes ownership of the process-global ESP-IDF Wi-Fi scan callback.
class WifiScanLeaseCoordinator {
public:
    explicit WifiScanLeaseCoordinator(uint64_t last_lease_id = 0,
                                      uint32_t driver_incarnation = 1,
                                      uint64_t last_recovery_id = 0)
        : last_lease_id_(last_lease_id),
          last_recovery_id_(last_recovery_id),
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

    class RecoveryDecision {
    public:
        bool begun() const { return begun_; }
        uint64_t recovery_id() const { return recovery_id_; }

    private:
        RecoveryDecision() = default;
        RecoveryDecision(bool begun, uint64_t recovery_id,
                         const WifiScanLeaseCoordinator* coordinator_identity)
            : begun_(begun), recovery_id_(recovery_id),
              coordinator_identity_(coordinator_identity) {}

        bool begun_ = false;
        uint64_t recovery_id_ = 0;
        const WifiScanLeaseCoordinator* coordinator_identity_ = nullptr;

        friend class WifiScanLeaseCoordinator;
        friend class WifiScanRecoveryExecutor;
        friend class RecoveryProof;
    };

    class RecoveryProof {
    public:
        bool Proves(const RecoveryDecision& recovery) const {
            return recovery.begun_ && recovery.recovery_id_ != 0 &&
                   recovery_id_ == recovery.recovery_id_ &&
                   coordinator_identity_ == recovery.coordinator_identity_ &&
                   driver_ready_ && barrier_drained_;
        }

    private:
        RecoveryProof() = default;
        RecoveryProof(uint64_t recovery_id,
                      const WifiScanLeaseCoordinator* coordinator_identity,
                      bool driver_ready, bool barrier_drained)
            : recovery_id_(recovery_id),
              coordinator_identity_(coordinator_identity),
              driver_ready_(driver_ready),
              barrier_drained_(barrier_drained) {}

        bool Proves(uint64_t recovery_id,
                    const WifiScanLeaseCoordinator* coordinator_identity) const {
            return recovery_id != 0 && recovery_id_ == recovery_id &&
                   coordinator_identity_ == coordinator_identity &&
                   driver_ready_ && barrier_drained_;
        }

        uint64_t recovery_id_ = 0;
        const WifiScanLeaseCoordinator* coordinator_identity_ = nullptr;
        bool driver_ready_ = false;
        bool barrier_drained_ = false;

        friend class WifiScanLeaseCoordinator;
        friend class WifiScanRecoveryExecutor;
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

    bool RetainFailedCompletion(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || submission_pending_ ||
            phase_ != Phase::kCompleting) {
            return false;
        }
        phase_ = Phase::kDraining;
        return true;
    }

    RecoveryDecision BeginRecovery(const Lease& lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        RecoveryDecision result;
        if (!Matches(lease) || submission_pending_ ||
            (phase_ != Phase::kRunning && phase_ != Phase::kDraining) ||
            last_recovery_id_ == std::numeric_limits<uint64_t>::max()) {
            return result;
        }

        ++last_recovery_id_;
        active_recovery_id_ = last_recovery_id_;
        phase_ = Phase::kRecovering;
        result = RecoveryDecision{true, active_recovery_id_, this};
        return result;
    }

    bool CompleteRecovery(const Lease& lease, const RecoveryProof& proof) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(lease) || phase_ != Phase::kRecovering ||
            !proof.Proves(active_recovery_id_, this)) {
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
        active_recovery_id_ = 0;
        phase_ = Phase::kFree;
    }

    std::mutex mutex_;
    Phase phase_ = Phase::kFree;
    Lease current_;
    uint64_t last_lease_id_ = 0;
    uint64_t last_recovery_id_ = 0;
    uint64_t active_recovery_id_ = 0;
    uint32_t driver_incarnation_ = 1;
    bool callback_latched_ = false;
    bool submission_pending_ = false;
};
