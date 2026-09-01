#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

// Serializes ownership of asynchronous Wi-Fi scans across BluFi lifecycle changes.
class BlufiWifiScanController {
public:
    explicit BlufiWifiScanController(uint32_t initial_driver_incarnation = 1)
        : driver_incarnation_(initial_driver_incarnation == 0
                                  ? 1
                                  : initial_driver_incarnation) {}

    enum class Phase : uint8_t {
        kIdle,
        kStarting,
        kRunning,
        kCompleting,
        kDraining,
    };

    struct Request {
        uint32_t setup_generation = 0;
        uint64_t ble_session_state = 0;
        uint64_t ble_connection_epoch = 0;
        bool save_results = true;
        bool send_list = false;
    };

    struct RequestDecision {
        uint64_t request_id = 0;
        bool start_now = false;
        bool queued = false;
        bool rejected_stale = false;
    };

    struct StartClaimDecision {
        bool claimed = false;
    };

    struct StartDecision {
        bool accepted = false;
        bool send_failure = false;
        bool draining = false;
        bool start_pending = false;
        uint64_t pending_request_id = 0;
        Request owner;
        Request pending;
    };

    struct CompletionDecision {
        uint64_t request_id = 0;
        bool owned_callback = false;
        bool discard_results = true;
        bool save_results = false;
        bool send_list = false;
        Request owner;
    };

    struct FinishDecision {
        bool completion_released = false;
        bool start_pending = false;
        uint64_t request_id = 0;
        Request pending;
    };

    struct RecoveryTicket {
        uint64_t request_id = 0;
        uint32_t driver_incarnation = 0;
        uint64_t lifecycle_revision = 0;
        bool valid = false;
    };

    RequestDecision RequestScan(const Request& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        RequestDecision result;
        if (!lifecycle_initialized_) {
            RecordLifecycle(request.setup_generation, request.ble_session_state,
                            request.ble_connection_epoch);
        }
        if (!MatchesCurrent(request)) {
            result.rejected_stale = true;
            return result;
        }
        if (phase_ == Phase::kIdle) {
            owner_ = request;
            owner_request_id_ = NextRequestId();
            phase_ = Phase::kStarting;
            submission_claimed_ = false;
            callback_claimed_ = false;
            invalidated_ = false;
            result.request_id = owner_request_id_;
            result.start_now = true;
            return result;
        }

        if (phase_ == Phase::kStarting && !submission_claimed_ &&
            !invalidated_ && MatchesCurrent(owner_)) {
            // A busy physical lease has not submitted anything yet. Fold all
            // current-session callers into that reservation so one eventual
            // scan satisfies the latest requested delivery semantics.
            owner_.save_results = owner_.save_results || request.save_results;
            owner_.send_list = owner_.send_list || request.send_list;
            result.request_id = owner_request_id_;
            result.queued = true;
            return result;
        }

        if (phase_ == Phase::kRunning && !callback_claimed_ && !invalidated_ &&
            MatchesCurrent(request)) {
            owner_.save_results = owner_.save_results || request.save_results;
            owner_.send_list = owner_.send_list || request.send_list;
            result.request_id = owner_request_id_;
            result.queued = true;
            return result;
        }

        // Multiple logical callers need only one subsequent physical scan. The
        // latest lifecycle snapshot supersedes older pending work.
        pending_ = request;
        result.request_id = owner_request_id_;
        result.queued = true;
        return result;
    }

    StartClaimDecision ClaimStart(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        StartClaimDecision result;
        if (request_id == 0 || request_id != owner_request_id_ ||
            phase_ != Phase::kStarting || invalidated_ ||
            submission_claimed_) {
            return result;
        }

        submission_claimed_ = true;
        result.claimed = true;
        return result;
    }

    FinishDecision ReleaseStartClaimForRetry(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        FinishDecision result;
        if (request_id == 0 || request_id != owner_request_id_ ||
            phase_ != Phase::kStarting || !submission_claimed_) {
            return result;
        }
        if (invalidated_ || !MatchesCurrent(owner_)) {
            ResetOwner();
            PromotePending(result);
            return result;
        }
        submission_claimed_ = false;
        return result;
    }

    bool SynchronizeDriverIncarnation(uint64_t request_id,
                                      uint32_t driver_incarnation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || request_id != owner_request_id_ ||
            phase_ != Phase::kStarting || submission_claimed_ ||
            driver_incarnation == 0) {
            return false;
        }
        driver_incarnation_ = driver_incarnation;
        return true;
    }

    std::optional<Request> UnclaimedRequestIfCurrent(
            uint64_t request_id, uint32_t generation, uint64_t session,
            uint64_t connection) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || request_id != owner_request_id_ ||
            phase_ != Phase::kStarting || submission_claimed_ || invalidated_ ||
            !Matches(owner_, generation, session, connection) ||
            !MatchesCurrent(owner_)) {
            return std::nullopt;
        }
        return owner_;
    }

    bool IsClaimCurrent(uint64_t request_id, uint32_t generation,
                        uint64_t session, uint64_t connection) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_id != 0 && request_id == owner_request_id_ &&
               phase_ == Phase::kStarting && submission_claimed_ &&
               !invalidated_ &&
               Matches(owner_, generation, session, connection) &&
               MatchesCurrent(owner_);
    }

    StartDecision CommitStart(uint64_t request_id, bool accepted,
                              bool retain_for_recovery = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        StartDecision result;
        if (request_id == 0 || request_id != owner_request_id_ ||
            !submission_claimed_) {
            return result;
        }

        if (phase_ != Phase::kStarting) {
            return result;
        }

        if (!accepted) {
            result.owner = owner_;
            result.send_failure = !invalidated_ && owner_.send_list;
            if (retain_for_recovery) {
                invalidated_ = true;
                retry_owner_after_recovery_ = false;
                phase_ = Phase::kDraining;
                return result;
            }
            ResetOwner();
            PromotePending(result);
            return result;
        }

        result.accepted = true;
        phase_ = invalidated_ ? Phase::kDraining : Phase::kRunning;
        result.draining = phase_ == Phase::kDraining;
        return result;
    }

    CompletionDecision BeginCompletion(uint32_t current_generation,
                                       uint64_t current_session,
                                       uint64_t current_connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        CompletionDecision result;
        if (callback_claimed_ || recovering_ ||
            (phase_ != Phase::kRunning && phase_ != Phase::kDraining)) {
            return result;
        }

        // ESP scan events are global. A callback observed before CommitStart(true)
        // may belong to WifiStation or another component, so Starting never owns it.
        callback_claimed_ = true;
        result.request_id = owner_request_id_;
        result.owned_callback = true;
        result.owner = owner_;

        const bool current =
            phase_ == Phase::kRunning && !invalidated_ &&
            owner_.setup_generation == current_generation &&
            owner_.ble_session_state == current_session &&
            owner_.ble_connection_epoch == current_connection;
        result.discard_results = !current;
        result.save_results = current && owner_.save_results;
        result.send_list = current && owner_.send_list;
        return result;
    }

    bool PrepareCompletion(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || request_id != owner_request_id_ ||
            !callback_claimed_ || recovering_ || completion_prepared_ ||
            (phase_ != Phase::kRunning && phase_ != Phase::kDraining)) {
            return false;
        }
        completion_prepared_ = true;
        phase_ = Phase::kCompleting;
        return true;
    }

    bool CommitPreparedCompletion(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || request_id != owner_request_id_ ||
            !callback_claimed_ || !completion_prepared_ ||
            completion_committed_ || phase_ != Phase::kCompleting) {
            return false;
        }
        completion_committed_ = true;
        return true;
    }

    FinishDecision ReleaseCommittedCompletion(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        FinishDecision result;
        if (request_id == 0 || request_id != owner_request_id_ ||
            !callback_claimed_ || !completion_prepared_ ||
            !completion_committed_ || phase_ != Phase::kCompleting) {
            return result;
        }

        result.completion_released = true;
        ResetOwner();
        PromotePending(result);
        return result;
    }

    FinishDecision FinishCompletion(uint64_t request_id) {
        if (!PrepareCompletion(request_id) ||
            !CommitPreparedCompletion(request_id)) {
            return FinishDecision{};
        }
        return ReleaseCommittedCompletion(request_id);
    }

    bool RetainFailedCompletion(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || request_id != owner_request_id_ ||
            !callback_claimed_ || recovering_) {
            return false;
        }
        callback_claimed_ = false;
        invalidated_ = true;
        retry_owner_after_recovery_ = false;
        phase_ = Phase::kDraining;
        completion_prepared_ = false;
        completion_committed_ = false;
        return true;
    }

    bool RetainCommittedCompletionForRecovery(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || request_id != owner_request_id_ ||
            !callback_claimed_ || !completion_prepared_ ||
            !completion_committed_ || recovering_ ||
            phase_ != Phase::kCompleting) {
            return false;
        }
        callback_claimed_ = false;
        completion_prepared_ = false;
        completion_committed_ = false;
        invalidated_ = true;
        retry_owner_after_recovery_ = false;
        phase_ = Phase::kDraining;
        return true;
    }

    FinishDecision InvalidateSession(uint32_t current_generation,
                                     uint64_t current_session,
                                     uint64_t current_connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        return InvalidateLocked(current_generation, current_session,
                                current_connection);
    }

    FinishDecision InvalidateSession(
            uint32_t expected_generation, uint64_t expected_session,
            uint64_t expected_connection, uint32_t current_generation,
            uint64_t current_session, uint64_t current_connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        FinishDecision result;
        if (lifecycle_initialized_ &&
            (expected_generation != current_generation_ ||
             expected_session != current_session_ ||
             expected_connection != current_connection_)) {
            return result;
        }
        return InvalidateLocked(current_generation, current_session,
                                current_connection);
    }

    bool UpdateSession(uint32_t expected_generation, uint64_t expected_session,
                       uint64_t expected_connection,
                       uint32_t current_generation, uint64_t current_session,
                       uint64_t current_connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_initialized_ &&
            (expected_generation != current_generation_ ||
             expected_session != current_session_ ||
             expected_connection != current_connection_)) {
            return false;
        }
        RecordLifecycle(current_generation, current_session,
                        current_connection);
        return true;
    }

    RecoveryTicket BeginRecovery(uint64_t expected_request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        RecoveryTicket result;
        if ((phase_ != Phase::kRunning && phase_ != Phase::kDraining) ||
            recovering_ || callback_claimed_ || expected_request_id == 0 ||
            expected_request_id != owner_request_id_) {
            return result;
        }

        recovering_ = true;
        invalidated_ = true;
        phase_ = Phase::kDraining;
        result.request_id = owner_request_id_;
        result.driver_incarnation = driver_incarnation_;
        result.lifecycle_revision = lifecycle_revision_;
        result.valid = true;
        return result;
    }

    FinishDecision CompleteRecovery(const RecoveryTicket& ticket, bool success) {
        std::lock_guard<std::mutex> lock(mutex_);
        FinishDecision result;
        if (!ticket.valid || !recovering_ ||
            ticket.request_id != owner_request_id_ ||
            ticket.driver_incarnation != driver_incarnation_) {
            return result;
        }

        recovering_ = false;
        if (!success) {
            return result;
        }

        ++driver_incarnation_;
        if (driver_incarnation_ == 0) {
            ++driver_incarnation_;
        }

        const Request recovered_owner = owner_;
        const bool retry_recovered_owner = retry_owner_after_recovery_;
        const bool lifecycle_unchanged =
            ticket.lifecycle_revision == lifecycle_revision_;
        std::optional<Request> valid_pending;
        if (pending_.has_value() && MatchesCurrent(*pending_)) {
            valid_pending = pending_;
        }
        pending_.reset();
        ResetOwner();
        if (valid_pending.has_value()) {
            Reserve(*valid_pending, result);
        } else if (retry_recovered_owner && lifecycle_unchanged &&
                   MatchesCurrent(recovered_owner)) {
            Reserve(recovered_owner, result);
        }
        return result;
    }

    Phase phase() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return phase_;
    }

    uint32_t driver_incarnation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return driver_incarnation_;
    }

    bool CallbackOwed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !callback_claimed_ &&
               (phase_ == Phase::kRunning || phase_ == Phase::kDraining);
    }

    bool CanUnregisterHandler() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return phase_ == Phase::kIdle && !recovering_;
    }

    std::optional<RecoveryTicket> RecoveryTicketIfOwned(
            uint64_t request_id, uint32_t driver_incarnation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || request_id != owner_request_id_ ||
            driver_incarnation != driver_incarnation_ || callback_claimed_ ||
            (phase_ != Phase::kRunning && phase_ != Phase::kDraining)) {
            return std::nullopt;
        }
        return RecoveryTicket{
            owner_request_id_, driver_incarnation_, lifecycle_revision_, true};
    }

private:
    FinishDecision InvalidateLocked(uint32_t current_generation,
                                    uint64_t current_session,
                                    uint64_t current_connection) {
        FinishDecision result;
        // Invalidation is a lifecycle boundary even when external counters have
        // not changed. Pending work from before the boundary is never reusable.
        pending_.reset();
        RecordLifecycle(current_generation, current_session,
                        current_connection);
        if (phase_ == Phase::kStarting && !submission_claimed_) {
            ResetOwner();
            return result;
        }
        if (phase_ != Phase::kIdle) {
            invalidated_ = true;
            if (phase_ == Phase::kRunning) {
                phase_ = Phase::kDraining;
            }
        }
        return result;
    }
    static bool Matches(const Request& request, uint32_t generation,
                        uint64_t session, uint64_t connection) {
        return request.setup_generation == generation &&
               request.ble_session_state == session &&
               request.ble_connection_epoch == connection;
    }

    bool MatchesCurrent(const Request& request) const {
        return lifecycle_initialized_ &&
               Matches(request, current_generation_, current_session_,
                       current_connection_);
    }

    void RecordLifecycle(uint32_t generation, uint64_t session,
                         uint64_t connection) {
        current_generation_ = generation;
        current_session_ = session;
        current_connection_ = connection;
        lifecycle_initialized_ = true;
        ++lifecycle_revision_;
        if (lifecycle_revision_ == 0) {
            ++lifecycle_revision_;
        }
    }

    uint64_t NextRequestId() {
        ++last_request_id_;
        if (last_request_id_ == 0) {
            ++last_request_id_;
        }
        return last_request_id_;
    }

    void ResetOwner() {
        phase_ = Phase::kIdle;
        owner_ = Request{};
        owner_request_id_ = 0;
        submission_claimed_ = false;
        callback_claimed_ = false;
        completion_prepared_ = false;
        completion_committed_ = false;
        invalidated_ = false;
        retry_owner_after_recovery_ = true;
    }

    void PromotePending(FinishDecision& result) {
        if (!pending_.has_value()) {
            return;
        }
        const Request pending = *pending_;
        pending_.reset();
        if (!MatchesCurrent(pending)) {
            return;
        }
        Reserve(pending, result);
    }

    void Reserve(const Request& request, FinishDecision& result) {
        owner_ = request;
        owner_request_id_ = NextRequestId();
        phase_ = Phase::kStarting;
        submission_claimed_ = false;
        result.start_pending = true;
        result.request_id = owner_request_id_;
        result.pending = owner_;
    }

    void PromotePending(StartDecision& result) {
        FinishDecision promoted;
        PromotePending(promoted);
        result.start_pending = promoted.start_pending;
        result.pending_request_id = promoted.request_id;
        result.pending = promoted.pending;
    }

    mutable std::mutex mutex_;
    Phase phase_ = Phase::kIdle;
    Request owner_;
    std::optional<Request> pending_;
    uint64_t last_request_id_ = 0;
    uint64_t owner_request_id_ = 0;
    uint32_t driver_incarnation_ = 1;
    uint32_t current_generation_ = 0;
    uint64_t current_session_ = 0;
    uint64_t current_connection_ = 0;
    uint64_t lifecycle_revision_ = 0;
    bool lifecycle_initialized_ = false;
    bool submission_claimed_ = false;
    bool callback_claimed_ = false;
    bool completion_prepared_ = false;
    bool completion_committed_ = false;
    bool invalidated_ = false;
    bool recovering_ = false;
    bool retry_owner_after_recovery_ = true;
};
