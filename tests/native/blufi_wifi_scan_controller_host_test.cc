#include "../../main/boards/common/blufi_wifi_scan_controller.h"
#include "../../components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

using Controller = BlufiWifiScanController;

class WifiScanRecoveryExecutor {
public:
    static WifiScanLeaseCoordinator::RecoveryProof Prove(
            const WifiScanLeaseCoordinator::RecoveryDecision& recovery) {
        return WifiScanLeaseCoordinator::RecoveryProof{
            recovery.recovery_id(), recovery.coordinator_identity_, true, true};
    }
};

namespace {

Controller::Request Request(uint32_t generation, uint64_t session,
                            uint64_t connection, bool send = true) {
    return Controller::Request{
        generation,
        session,
        connection,
        true,
        send,
    };
}

void ClaimStart(Controller& controller, uint64_t request_id) {
    assert(controller.ClaimStart(request_id).claimed);
}

void EarlyPhysicalCallbackIsConsumedAfterBothCommits() {
    Controller logical;
    WifiScanLeaseCoordinator physical;
    const auto request = logical.RequestScan(Request(1, 11, 101));
    const auto lease = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(request.start_now);
    assert(lease.acquired);
    assert(logical.ClaimStart(request.request_id).claimed);
    assert(physical.ObserveScanDone(lease.lease).deferred_until_commit);
    const auto physical_commit =
        physical.CommitSubmission(lease.lease, true);
    assert(physical_commit.consume_latched);
    assert(logical.CommitStart(request.request_id, true).accepted);
    const auto completion = logical.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    assert(logical.FinishCompletion(completion.request_id).start_pending == false);
    assert(physical.FinishCompletion(lease.lease));
}

void LeaseBusyCoalescesOneUnclaimedCurrentRequest() {
    Controller logical;
    const auto first = logical.RequestScan(Request(1, 11, 101, false));
    const auto repeated = logical.RequestScan(Request(1, 11, 101, true));
    assert(first.start_now);
    assert(repeated.queued);
    assert(repeated.request_id == first.request_id);
    const auto owner = logical.ClaimStart(first.request_id);
    assert(owner.claimed);
    const auto committed = logical.CommitStart(first.request_id, true);
    assert(committed.accepted);
    const auto completion = logical.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    assert(completion.send_list);
    assert(!logical.FinishCompletion(completion.request_id).start_pending);
}

void SynchronousFailureRecoveryDoesNotRetryFailedOwner() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    const auto failure = controller.CommitStart(owner.request_id, false, true);
    assert(failure.send_failure);
    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(!recovered.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void CleanupFailureCanEnterRecoveryAfterCallbackClaim() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    const auto completion = controller.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    assert(controller.RetainFailedCompletion(completion.request_id));
    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
}

void ClaimedStartMustRevalidateExactLifecycleBeforeSubmission() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.IsClaimCurrent(owner.request_id, 1, 11, 101));
    controller.InvalidateSession(2, 22, 202);
    assert(!controller.IsClaimCurrent(owner.request_id, 2, 22, 202));
}

void GlobalIncarnationSynchronizesBeforeLogicalClaim() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    assert(controller.SynchronizeDriverIncarnation(owner.request_id, 7));
    assert(controller.driver_incarnation() == 7);
    ClaimStart(controller, owner.request_id);
    assert(!controller.SynchronizeDriverIncarnation(owner.request_id, 8));
}

void StationDrainBackpressuresOneBlufiRequestUntilRelease() {
    Controller logical;
    WifiScanLeaseCoordinator physical;
    const auto station = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation);
    assert(station.acquired);
    assert(physical.CommitSubmission(station.lease, true).accepted);
    assert(physical.BeginDrain(station.lease));

    const auto request = logical.RequestScan(Request(1, 11, 101));
    assert(request.start_now);
    assert(!physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
    const auto repeated = logical.RequestScan(Request(1, 11, 101));
    assert(repeated.queued);
    assert(repeated.request_id == request.request_id);

    assert(physical.ObserveScanDone(station.lease).consume_now);
    assert(physical.FinishCompletion(station.lease));
    const auto blufi = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(blufi.acquired);
    assert(logical.SynchronizeDriverIncarnation(
        request.request_id, blufi.lease.driver_incarnation));
    ClaimStart(logical, request.request_id);
}

void DisconnectReconnectReplacesLeaseBusyReservation() {
    Controller logical;
    const auto stale = logical.RequestScan(Request(1, 11, 101));
    assert(stale.start_now);
    logical.InvalidateSession(2, 22, 202);
    const auto current = logical.RequestScan(Request(2, 22, 202));
    assert(current.start_now);
    assert(current.request_id != stale.request_id);
    assert(!logical.ClaimStart(stale.request_id).claimed);
    ClaimStart(logical, current.request_id);
}

void RetryReservationSurvivesSchedulerFailuresAndStartsExactlyOnce() {
    Controller logical;
    const auto requested = logical.RequestScan(Request(1, 11, 101));
    assert(requested.start_now);
    for (int failure = 0; failure < 3; ++failure) {
        const auto retry = logical.UnclaimedRequestIfCurrent(
            requested.request_id, 1, 11, 101);
        assert(retry.has_value());
        assert(retry->ble_connection_epoch == 101);
    }
    const auto eventual = logical.UnclaimedRequestIfCurrent(
        requested.request_id, 1, 11, 101);
    assert(eventual.has_value());
    ClaimStart(logical, requested.request_id);
    assert(!logical.UnclaimedRequestIfCurrent(
        requested.request_id, 1, 11, 101).has_value());
    assert(!logical.ClaimStart(requested.request_id).claimed);
}

void StaleRetryReservationIsDiscardedAtLifecycleBoundary() {
    Controller logical;
    const auto stale = logical.RequestScan(Request(1, 11, 101));
    logical.InvalidateSession(2, 22, 202);
    assert(!logical.UnclaimedRequestIfCurrent(
        stale.request_id, 1, 11, 101).has_value());
}

class Barrier {
public:
    explicit Barrier(unsigned participants) : participants_(participants) {}

    void ArriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const unsigned generation = generation_;
        if (++arrived_ == participants_) {
            arrived_ = 0;
            ++generation_;
            condition_.notify_all();
            return;
        }
        condition_.wait(lock, [this, generation]() {
            return generation_ != generation;
        });
    }

private:
    const unsigned participants_;
    std::mutex mutex_;
    std::condition_variable condition_;
    unsigned arrived_ = 0;
    unsigned generation_ = 0;
};

class Signal {
public:
    void Notify() {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
        condition_.notify_one();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return ready_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool ready_ = false;
};

void DelayedOldCompletionCannotSatisfyNewRequest() {
    Controller controller;
    assert(controller.CanUnregisterHandler());
    assert(!controller.CallbackOwed());
    const auto first = controller.RequestScan(Request(1, 11, 101));
    assert(first.start_now);
    assert(!controller.CanUnregisterHandler());
    assert(!controller.CallbackOwed());
    ClaimStart(controller, first.request_id);
    assert(controller.CommitStart(first.request_id, true).accepted);
    assert(controller.CallbackOwed());
    assert(!controller.CanUnregisterHandler());

    const auto owned = controller.RecoveryTicketIfOwned(
        first.request_id, controller.driver_incarnation());
    assert(owned.has_value());
    assert(owned->request_id == first.request_id);

    controller.InvalidateSession(2, 22, 202);
    assert(controller.CallbackOwed());
    assert(!controller.CanUnregisterHandler());
    const auto second = controller.RequestScan(Request(2, 22, 202));
    assert(second.queued);

    const auto old = controller.BeginCompletion(2, 22, 202);
    assert(old.owned_callback);
    assert(!controller.CallbackOwed());
    assert(!controller.CanUnregisterHandler());
    assert(old.discard_results);
    assert(!old.save_results);
    assert(!old.send_list);

    const auto drained = controller.FinishCompletion(old.request_id);
    assert(drained.start_pending);
    assert(drained.request_id != old.request_id);
    assert(drained.pending.setup_generation == 2);
    assert(drained.pending.ble_connection_epoch == 202);
    assert(!controller.CallbackOwed());
    assert(!controller.CanUnregisterHandler());
}

void DisconnectReconnectRefreshesLifecycleWhileOldCallbackDrains() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, old.request_id);
    assert(controller.CommitStart(old.request_id, true).accepted);

    controller.InvalidateSession(1, 12, 101);
    assert(controller.phase() == Controller::Phase::kDraining);
    assert(controller.CallbackOwed());

    assert(controller.UpdateSession(1, 12, 101, 1, 13, 101));
    assert(controller.UpdateSession(1, 13, 101, 1, 14, 102));
    assert(!controller.UpdateSession(1, 13, 101, 1, 14, 102));
    const auto current = controller.RequestScan(Request(1, 14, 102));
    assert(current.queued);
    assert(!current.rejected_stale);
    assert(controller.CallbackOwed());

    const auto delayed_old = controller.BeginCompletion(1, 14, 102);
    assert(delayed_old.owned_callback);
    assert(delayed_old.discard_results);
    assert(!delayed_old.send_list);

    const auto drained = controller.FinishCompletion(delayed_old.request_id);
    assert(drained.start_pending);
    assert(drained.pending.ble_session_state == 14);
    assert(drained.pending.ble_connection_epoch == 102);
    assert(!controller.CallbackOwed());
}

void DelayedLifecycleRefreshCannotRegressNewGeneration() {
    Controller controller;
    assert(controller.UpdateSession(1, 10, 100, 1, 11, 100));
    controller.InvalidateSession(2, 20, 100);

    assert(!controller.UpdateSession(1, 11, 100, 1, 12, 101));
    const auto current = controller.RequestScan(Request(2, 20, 100));
    assert(current.start_now);
    assert(!current.rejected_stale);
}

void DelayedBoundaryInvalidationCannotRegressNewGeneration() {
    Controller controller;
    assert(controller.UpdateSession(1, 10, 100, 1, 11, 100));
    controller.InvalidateSession(1, 11, 100, 2, 20, 100);

    controller.InvalidateSession(1, 11, 100, 1, 12, 100);
    const auto current = controller.RequestScan(Request(2, 20, 100));
    assert(current.start_now);
    assert(!current.rejected_stale);
}

void DelayedStaleRequestCannotOverwriteCurrentPending() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, old.request_id);
    assert(controller.CommitStart(old.request_id, true).accepted);

    controller.InvalidateSession(2, 22, 202);
    const auto current = controller.RequestScan(Request(2, 22, 202));
    assert(current.queued);
    const auto delayed = controller.RequestScan(Request(1, 11, 101));
    assert(delayed.rejected_stale);
    assert(!delayed.start_now);
    assert(!delayed.queued);

    const auto completion = controller.BeginCompletion(2, 22, 202);
    assert(completion.owned_callback);
    const auto promoted = controller.FinishCompletion(completion.request_id);
    assert(promoted.start_pending);
    assert(promoted.pending.setup_generation == 2);
    assert(promoted.pending.ble_session_state == 22);
    assert(promoted.pending.ble_connection_epoch == 202);
}

void IdleControllerRejectsDelayedStaleRequestAfterInvalidation() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    assert(old.start_now);
    controller.InvalidateSession(2, 22, 202);
    assert(controller.phase() == Controller::Phase::kIdle);

    const auto delayed = controller.RequestScan(Request(1, 11, 101));
    assert(delayed.rejected_stale);
    assert(controller.phase() == Controller::Phase::kIdle);

    const auto current = controller.RequestScan(Request(2, 22, 202));
    assert(current.start_now);
    assert(!current.rejected_stale);
}

void CurrentRepeatedRequestStillCoalescesAndReplacesPending() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    const auto first = controller.RequestScan(Request(1, 11, 101, true));
    const auto replacement = controller.RequestScan(Request(1, 11, 101, false));
    assert(first.queued);
    assert(replacement.queued);
    assert(!replacement.rejected_stale);

    const auto completion = controller.BeginCompletion(1, 11, 101);
    const auto promoted = controller.FinishCompletion(completion.request_id);
    assert(!promoted.start_pending);
    assert(completion.send_list);
}

void ThreadedDelayedStaleRequestCannotReplaceCurrentPending() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, old.request_id);
    assert(controller.CommitStart(old.request_id, true).accepted);
    controller.InvalidateSession(2, 22, 202);

    Barrier start(3);
    Signal current_queued;
    Controller::RequestDecision delayed;
    std::thread current([&]() {
        start.ArriveAndWait();
        assert(controller.RequestScan(Request(2, 22, 202)).queued);
        current_queued.Notify();
    });
    std::thread stale([&]() {
        start.ArriveAndWait();
        current_queued.Wait();
        delayed = controller.RequestScan(Request(1, 11, 101));
    });
    start.ArriveAndWait();
    current.join();
    stale.join();
    assert(delayed.rejected_stale);

    const auto completion = controller.BeginCompletion(2, 22, 202);
    const auto promoted = controller.FinishCompletion(completion.request_id);
    assert(promoted.start_pending);
    assert(promoted.pending.setup_generation == 2);
}

void InvalidateBeforeClaimRetiresReservation() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    assert(old.start_now);
    assert(!controller.BeginCompletion(1, 11, 101).owned_callback);
    assert(!controller.CommitStart(old.request_id, true).accepted);
    assert(controller.phase() == Controller::Phase::kStarting);

    const auto retired = controller.InvalidateSession(2, 22, 202);
    assert(!retired.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
    assert(!controller.ClaimStart(old.request_id).claimed);
    assert(!controller.CommitStart(old.request_id, true).accepted);
    assert(!controller.BeginCompletion(2, 22, 202).owned_callback);

    const auto current = controller.RequestScan(Request(2, 22, 202));
    assert(current.start_now);
    assert(current.request_id != old.request_id);
    ClaimStart(controller, current.request_id);
}

void SameTupleInvalidateBeforeClaimClearsPendingAndRetiresOwner() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    const auto queued = controller.RequestScan(Request(1, 11, 101, false));
    assert(queued.queued);

    const auto invalidated = controller.InvalidateSession(1, 11, 101);
    assert(!invalidated.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
    assert(!controller.ClaimStart(old.request_id).claimed);

    const auto after_boundary =
        controller.RequestScan(Request(1, 11, 101, false));
    assert(after_boundary.start_now);
    assert(after_boundary.request_id != old.request_id);
}

void SameTupleInvalidateRunningClearsPreBoundaryPending() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    assert(controller.RequestScan(Request(1, 11, 101, false)).queued);

    controller.InvalidateSession(1, 11, 101);
    const auto completion = controller.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    assert(completion.discard_results);
    const auto finished = controller.FinishCompletion(completion.request_id);
    assert(!finished.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void SameTupleRequestAfterRunningInvalidationCanQueue() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    controller.InvalidateSession(1, 11, 101);
    const auto after_boundary =
        controller.RequestScan(Request(1, 11, 101, false));
    assert(after_boundary.queued);
    assert(!after_boundary.rejected_stale);

    const auto completion = controller.BeginCompletion(1, 11, 101);
    const auto finished = controller.FinishCompletion(completion.request_id);
    assert(finished.start_pending);
    assert(!finished.pending.send_list);
}

void SameTupleInvalidateDuringRecoveryClearsPreBoundaryPending() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    assert(controller.RequestScan(Request(1, 11, 101, false)).queued);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    controller.InvalidateSession(1, 11, 101);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(!recovered.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void InvalidateAfterClaimDrainsAcceptedSubmission() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, old.request_id);
    assert(!controller.ClaimStart(old.request_id).claimed);

    const auto invalidated = controller.InvalidateSession(2, 22, 202);
    assert(!invalidated.start_pending);
    const auto committed = controller.CommitStart(old.request_id, true);
    assert(committed.accepted);
    assert(committed.draining);

    const auto completion = controller.BeginCompletion(2, 22, 202);
    assert(completion.owned_callback);
    assert(completion.discard_results);
    assert(!completion.save_results);
    assert(!completion.send_list);
    controller.FinishCompletion(completion.request_id);
}

void SynchronousStartFailurePromotesPendingRequest() {
    Controller controller;
    const auto first = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, first.request_id);
    controller.InvalidateSession(2, 22, 202);
    const auto queued = controller.RequestScan(Request(2, 22, 202));
    assert(queued.queued);

    const auto failure = controller.CommitStart(first.request_id, false);
    assert(!failure.accepted);
    assert(!failure.send_failure);
    assert(failure.start_pending);
    assert(failure.pending_request_id != first.request_id);
    assert(failure.pending.setup_generation == 2);

    ClaimStart(controller, failure.pending_request_id);
    const auto current_failure =
        controller.CommitStart(failure.pending_request_id, false);
    assert(current_failure.send_failure);
    assert(!current_failure.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void ConcurrentRequestsCoalesceToLatestPendingRequest() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    Barrier barrier(3);
    std::thread first([&]() {
        barrier.ArriveAndWait();
        assert(controller.RequestScan(Request(1, 11, 101, true)).queued);
    });
    std::thread second([&]() {
        barrier.ArriveAndWait();
        assert(controller.RequestScan(Request(1, 11, 101, false)).queued);
    });
    barrier.ArriveAndWait();
    first.join();
    second.join();

    const auto completion = controller.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    const auto next = controller.FinishCompletion(completion.request_id);
    assert(!next.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
    assert(!controller.FinishCompletion(completion.request_id).start_pending);
}

void RunningAndDrainingCompletionsHaveDistinctDelivery() {
    Controller controller;
    const auto running = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, running.request_id);
    assert(controller.CommitStart(running.request_id, true).accepted);
    const auto current = controller.BeginCompletion(1, 11, 101);
    assert(current.owned_callback);
    assert(!current.discard_results);
    assert(current.save_results);
    assert(current.send_list);
    assert(!controller.FinishCompletion(current.request_id).start_pending);

    const auto draining = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, draining.request_id);
    assert(controller.CommitStart(draining.request_id, true).accepted);
    controller.InvalidateSession(2, 22, 202);
    const auto stale = controller.BeginCompletion(2, 22, 202);
    assert(stale.owned_callback);
    assert(stale.discard_results);
    assert(!stale.save_results);
    assert(!stale.send_list);
    assert(!controller.FinishCompletion(stale.request_id).start_pending);
}

void CallbackRacingCommitStartHasConsistentOwnership() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        Controller controller;
        const auto request = controller.RequestScan(Request(1, 11, 101));
        ClaimStart(controller, request.request_id);
        Barrier barrier(3);
        Controller::StartDecision commit;
        Controller::CompletionDecision completion;
        std::thread starter([&]() {
            barrier.ArriveAndWait();
            commit = controller.CommitStart(request.request_id, true);
        });
        std::thread callback([&]() {
            barrier.ArriveAndWait();
            completion = controller.BeginCompletion(1, 11, 101);
        });
        barrier.ArriveAndWait();
        starter.join();
        callback.join();

        assert(commit.accepted);
        assert(!commit.draining);
        if (!completion.owned_callback) {
            completion = controller.BeginCompletion(1, 11, 101);
        }
        assert(completion.owned_callback);
        assert(!completion.discard_results);
        assert(!controller.BeginCompletion(1, 11, 101).owned_callback);
        assert(!controller.FinishCompletion(completion.request_id).start_pending);

        const auto late_commit =
            controller.CommitStart(request.request_id, true);
        assert(!late_commit.accepted);
        assert(controller.phase() == Controller::Phase::kIdle);
    }
}

void ForeignCompletionWhileStartingCannotOwnFailedSubmission() {
    Controller controller;
    const auto request = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, request.request_id);

    const auto foreign = controller.BeginCompletion(1, 11, 101);
    assert(!foreign.owned_callback);

    const auto failed = controller.CommitStart(request.request_id, false);
    assert(!failed.accepted);
    assert(failed.send_failure);
    assert(failed.owner.setup_generation == 1);
    assert(failed.owner.ble_session_state == 11);
    assert(failed.owner.ble_connection_epoch == 101);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void LostCallbackRecoveryAdvancesDriverBeforePendingStart() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    controller.InvalidateSession(2, 22, 202);
    assert(controller.RequestScan(Request(2, 22, 202)).queued);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    assert(!controller.BeginRecovery(owner.request_id).valid);

    const auto failed = controller.CompleteRecovery(ticket, false);
    assert(!failed.start_pending);
    assert(controller.phase() == Controller::Phase::kDraining);

    const auto retry = controller.BeginRecovery(owner.request_id);
    assert(retry.valid);
    assert(retry.driver_incarnation == ticket.driver_incarnation);
    const auto recovered = controller.CompleteRecovery(retry, true);
    assert(recovered.start_pending);
    assert(recovered.pending.setup_generation == 2);

    const auto next_ticket =
        controller.BeginRecovery(recovered.request_id);
    assert(!next_ticket.valid);  // A Starting request has no owed callback yet.
    assert(controller.driver_incarnation() == ticket.driver_incarnation + 1);
}

void CurrentOwnerRetriesAfterSuccessfulRecovery() {
    Controller controller;
    const auto owner_request = Request(1, 11, 101, false);
    const auto owner = controller.RequestScan(owner_request);
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    assert(ticket.lifecycle_revision != 0);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(recovered.start_pending);
    assert(recovered.request_id != owner.request_id);
    assert(recovered.pending.setup_generation == owner_request.setup_generation);
    assert(recovered.pending.ble_session_state == owner_request.ble_session_state);
    assert(recovered.pending.ble_connection_epoch ==
           owner_request.ble_connection_epoch);
    assert(recovered.pending.save_results == owner_request.save_results);
    assert(recovered.pending.send_list == owner_request.send_list);
    assert(controller.phase() == Controller::Phase::kStarting);
    assert(controller.driver_incarnation() == ticket.driver_incarnation + 1);
}

void StaleOwnerDoesNotRetryAfterSuccessfulRecovery() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    controller.InvalidateSession(2, 22, 202);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(!recovered.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void InvalidationDuringRecoveryCannotRetryOldOwner() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    controller.InvalidateSession(2, 22, 202);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(!recovered.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void InvalidationRevisionRetiresRecoveringOwnerEvenForSameTuple() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    controller.InvalidateSession(1, 11, 101);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(!recovered.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void PendingFromNewLifecycleWinsWhenRecoveryTicketRevisionIsOld() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    controller.InvalidateSession(2, 22, 202);
    assert(controller.RequestScan(Request(2, 22, 202, false)).queued);

    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(recovered.start_pending);
    assert(recovered.pending.setup_generation == 2);
    assert(recovered.pending.ble_session_state == 22);
    assert(recovered.pending.ble_connection_epoch == 202);
    assert(!recovered.pending.send_list);
}

void ThreadedInvalidateDuringRecoveryUsesLatestLifecycle() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);

    Barrier start(3);
    Signal invalidated;
    Controller::FinishDecision recovered;
    std::thread invalidator([&]() {
        start.ArriveAndWait();
        controller.InvalidateSession(2, 22, 202);
        assert(controller.RequestScan(Request(2, 22, 202)).queued);
        invalidated.Notify();
    });
    std::thread recovery([&]() {
        start.ArriveAndWait();
        invalidated.Wait();
        recovered = controller.CompleteRecovery(ticket, true);
    });
    start.ArriveAndWait();
    invalidator.join();
    recovery.join();

    assert(recovered.start_pending);
    assert(recovered.pending.setup_generation == 2);
    assert(recovered.pending.ble_session_state == 22);
    assert(recovered.pending.ble_connection_epoch == 202);
}

void RepeatedCurrentRequestMergesIntoOwnerAcrossRecovery() {
    Controller controller;
    const auto owner_request = Request(1, 11, 101, true);
    const auto pending_request = Request(1, 11, 101, false);
    const auto owner = controller.RequestScan(owner_request);
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    assert(controller.RequestScan(pending_request).queued);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(recovered.start_pending);
    assert(recovered.pending.send_list == owner_request.send_list);
    assert(recovered.pending.save_results == pending_request.save_results);
}

void RejectedStalePendingLeavesCurrentOwnerRecovery() {
    Controller controller;
    const auto owner_request = Request(1, 11, 101, true);
    const auto stale_pending = Request(2, 22, 202, false);
    const auto owner = controller.RequestScan(owner_request);
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    const auto rejected = controller.RequestScan(stale_pending);
    assert(rejected.rejected_stale);
    assert(!rejected.queued);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(recovered.start_pending);
    assert(recovered.pending.setup_generation == owner_request.setup_generation);
    assert(recovered.pending.send_list == owner_request.send_list);
}

void RecoveryIncarnationWrapSkipsZero() {
    Controller controller(std::numeric_limits<uint32_t>::max());
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    assert(ticket.driver_incarnation == std::numeric_limits<uint32_t>::max());
    controller.CompleteRecovery(ticket, true);
    assert(controller.driver_incarnation() == 1);
}

void WrongRequestRecoveryIsRejected() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    assert(!controller.BeginRecovery(owner.request_id + 1).valid);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    auto wrong_incarnation = ticket;
    ++wrong_incarnation.driver_incarnation;
    assert(!controller.CompleteRecovery(wrong_incarnation, true).start_pending);
    auto wrong = ticket;
    ++wrong.request_id;
    assert(!controller.CompleteRecovery(wrong, true).start_pending);
    assert(controller.phase() == Controller::Phase::kDraining);
    assert(!controller.BeginRecovery(owner.request_id).valid);
    const auto recovered = controller.CompleteRecovery(ticket, true);
    assert(recovered.start_pending);
    assert(recovered.request_id != owner.request_id);
    assert(controller.phase() == Controller::Phase::kStarting);
}

void CallbackBeforeInvalidatePreservesCurrentOwnerDelivery() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    Signal callback_claimed;
    Controller::CompletionDecision completion;
    std::thread callback([&]() {
        completion = controller.BeginCompletion(1, 11, 101);
        callback_claimed.Notify();
    });
    std::thread invalidator([&]() {
        callback_claimed.Wait();
        controller.InvalidateSession(2, 22, 202);
    });
    callback.join();
    invalidator.join();

    assert(completion.owned_callback);
    assert(!completion.discard_results);
    assert(completion.save_results);
    assert(completion.send_list);
    controller.FinishCompletion(completion.request_id);
}

void InvalidateBeforeCallbackDiscardsCurrentOwnerDelivery() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    Signal invalidated;
    Controller::CompletionDecision completion;
    std::thread invalidator([&]() {
        controller.InvalidateSession(2, 22, 202);
        invalidated.Notify();
    });
    std::thread callback([&]() {
        invalidated.Wait();
        completion = controller.BeginCompletion(1, 11, 101);
    });
    invalidator.join();
    callback.join();

    assert(completion.owned_callback);
    assert(completion.discard_results);
    assert(!completion.save_results);
    assert(!completion.send_list);
    controller.FinishCompletion(completion.request_id);
}

void SimultaneousCallbackAndInvalidateHaveConsistentLinearization() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        Controller controller;
        const auto owner = controller.RequestScan(Request(1, 11, 101));
        ClaimStart(controller, owner.request_id);
        assert(controller.CommitStart(owner.request_id, true).accepted);

        Barrier barrier(3);
        Controller::CompletionDecision completion;
        std::thread callback([&]() {
            barrier.ArriveAndWait();
            completion = controller.BeginCompletion(1, 11, 101);
        });
        std::thread invalidator([&]() {
            barrier.ArriveAndWait();
            controller.InvalidateSession(2, 22, 202);
        });
        barrier.ArriveAndWait();
        callback.join();
        invalidator.join();

        assert(completion.owned_callback);
        const bool delivered = !completion.discard_results;
        assert(completion.save_results == delivered);
        assert(completion.send_list == delivered);
        controller.FinishCompletion(completion.request_id);
    }
}

void CallbackBetweenDriverReturnAndCommitsCannotStrandEitherOwner() {
    for (const bool driver_accepted : {true, false}) {
        Controller logical;
        WifiScanLeaseCoordinator physical;
        std::mutex submission_mutex;
        const auto request = logical.RequestScan(Request(1, 11, 101));
        const auto lease = physical.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kBlufi);
        assert(lease.acquired);
        ClaimStart(logical, request.request_id);

        Barrier driver_returned(2);
        Signal callback_observed;
        WifiScanLeaseCoordinator::CallbackDecision callback;
        std::thread event([&]() {
            driver_returned.ArriveAndWait();
            std::lock_guard<std::mutex> lock(submission_mutex);
            callback = physical.ObserveScanDone(lease.lease);
            callback_observed.Notify();
        });
        driver_returned.ArriveAndWait();
        callback_observed.Wait();

        WifiScanLeaseCoordinator::CommitDecision physical_commit;
        Controller::StartDecision logical_commit;
        {
            std::lock_guard<std::mutex> lock(submission_mutex);
            physical_commit = physical.CommitSubmission(
                lease.lease, driver_accepted);
            logical_commit = logical.CommitStart(
                request.request_id,
                driver_accepted || physical_commit.callback_won_error,
                physical_commit.drain_required);
        }
        event.join();
        assert(callback.deferred_until_commit);
        assert(physical_commit.consume_latched);
        assert(logical_commit.accepted);
        assert(!logical_commit.send_failure);
        const auto completion = logical.BeginCompletion(1, 11, 101);
        assert(completion.owned_callback);
        logical.FinishCompletion(completion.request_id);
        assert(physical.FinishCompletion(lease.lease));
    }
}

void CallbackCannotEnterBetweenPhysicalAndLogicalCommit() {
    Controller logical;
    WifiScanLeaseCoordinator physical;
    std::mutex submission_mutex;
    const auto request = logical.RequestScan(Request(1, 11, 101));
    const auto lease = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    ClaimStart(logical, request.request_id);

    Signal physical_committed;
    Signal callback_finished;
    WifiScanLeaseCoordinator::CallbackDecision callback;
    std::thread event([&]() {
        physical_committed.Wait();
        std::lock_guard<std::mutex> lock(submission_mutex);
        callback = physical.ObserveScanDone(lease.lease);
        callback_finished.Notify();
    });
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        const auto physical_commit =
            physical.CommitSubmission(lease.lease, true);
        assert(physical_commit.accepted);
        physical_committed.Notify();
        const auto logical_commit =
            logical.CommitStart(request.request_id, true);
        assert(logical_commit.accepted);
    }
    callback_finished.Wait();
    event.join();
    assert(callback.consume_now);
    const auto completion = logical.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    logical.FinishCompletion(completion.request_id);
    assert(physical.FinishCompletion(lease.lease));
}

void CallbackLatchedBeforePreSubmissionFailureCannotStrandLease() {
    Controller logical;
    WifiScanLeaseCoordinator physical;
    std::mutex submission_mutex;
    const auto request = logical.RequestScan(Request(1, 11, 101));
    const auto lease = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    ClaimStart(logical, request.request_id);

    assert(physical.ObserveScanDone(lease.lease).deferred_until_commit);
    WifiScanLeaseCoordinator::CommitDecision physical_commit;
    Controller::StartDecision logical_commit;
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        assert(!physical.AbandonUnsubmitted(lease.lease));
        physical_commit = physical.CommitSubmission(lease.lease, false);
        logical_commit = logical.CommitStart(
            request.request_id, physical_commit.callback_won_error,
            physical_commit.drain_required);
    }
    assert(physical_commit.consume_latched);
    assert(physical_commit.callback_won_error);
    assert(logical_commit.accepted);
    const auto completion = logical.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    logical.FinishCompletion(completion.request_id);
    assert(physical.FinishCompletion(lease.lease));
    assert(physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation).acquired);
}

void WatchdogArmFailureEntersExactRecoveryAndRetriesNotification() {
    Controller logical;
    WifiScanLeaseCoordinator physical;
    const auto request = logical.RequestScan(Request(1, 11, 101));
    const auto lease = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(logical.SynchronizeDriverIncarnation(
        request.request_id, lease.lease.driver_incarnation));
    ClaimStart(logical, request.request_id);
    assert(physical.CommitSubmission(lease.lease, true).accepted);
    assert(logical.CommitStart(request.request_id, true).accepted);

    // Both watchdog create and start failures take this same exact-debt path.
    assert(physical.BeginDrain(lease.lease));
    int manager_notifications = 1;  // first notification fails
    const auto ticket = logical.BeginRecovery(request.request_id);
    assert(ticket.valid);
    const auto recovery = physical.BeginRecovery(lease.lease);
    assert(recovery.begun());
    ++manager_notifications;  // fallback timer retries successfully
    const auto proof = WifiScanRecoveryExecutor::Prove(recovery);
    assert(physical.CompleteRecovery(lease.lease, proof));
    const auto recovered = logical.CompleteRecovery(ticket, true);
    assert(recovered.start_pending);
    assert(manager_notifications == 2);
}

void CallbackWinningWatchdogRecoveryNeedsNoReset() {
    Controller logical;
    WifiScanLeaseCoordinator physical;
    const auto request = logical.RequestScan(Request(1, 11, 101));
    const auto lease = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    ClaimStart(logical, request.request_id);
    assert(physical.CommitSubmission(lease.lease, true).accepted);
    assert(logical.CommitStart(request.request_id, true).accepted);
    assert(physical.BeginDrain(lease.lease));
    assert(physical.ObserveScanDone(lease.lease).consume_now);
    const auto completion = logical.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    assert(!physical.BeginRecovery(lease.lease).begun());
    logical.FinishCompletion(completion.request_id);
    assert(physical.FinishCompletion(lease.lease));
}

}  // namespace

int main() {
    EarlyPhysicalCallbackIsConsumedAfterBothCommits();
    LeaseBusyCoalescesOneUnclaimedCurrentRequest();
    SynchronousFailureRecoveryDoesNotRetryFailedOwner();
    CleanupFailureCanEnterRecoveryAfterCallbackClaim();
    ClaimedStartMustRevalidateExactLifecycleBeforeSubmission();
    GlobalIncarnationSynchronizesBeforeLogicalClaim();
    StationDrainBackpressuresOneBlufiRequestUntilRelease();
    DisconnectReconnectReplacesLeaseBusyReservation();
    RetryReservationSurvivesSchedulerFailuresAndStartsExactlyOnce();
    StaleRetryReservationIsDiscardedAtLifecycleBoundary();
    DelayedOldCompletionCannotSatisfyNewRequest();
    DisconnectReconnectRefreshesLifecycleWhileOldCallbackDrains();
    DelayedLifecycleRefreshCannotRegressNewGeneration();
    DelayedBoundaryInvalidationCannotRegressNewGeneration();
    DelayedStaleRequestCannotOverwriteCurrentPending();
    IdleControllerRejectsDelayedStaleRequestAfterInvalidation();
    CurrentRepeatedRequestStillCoalescesAndReplacesPending();
    ThreadedDelayedStaleRequestCannotReplaceCurrentPending();
    InvalidateBeforeClaimRetiresReservation();
    SameTupleInvalidateBeforeClaimClearsPendingAndRetiresOwner();
    SameTupleInvalidateRunningClearsPreBoundaryPending();
    SameTupleRequestAfterRunningInvalidationCanQueue();
    SameTupleInvalidateDuringRecoveryClearsPreBoundaryPending();
    InvalidateAfterClaimDrainsAcceptedSubmission();
    SynchronousStartFailurePromotesPendingRequest();
    ConcurrentRequestsCoalesceToLatestPendingRequest();
    RunningAndDrainingCompletionsHaveDistinctDelivery();
    CallbackRacingCommitStartHasConsistentOwnership();
    ForeignCompletionWhileStartingCannotOwnFailedSubmission();
    LostCallbackRecoveryAdvancesDriverBeforePendingStart();
    CurrentOwnerRetriesAfterSuccessfulRecovery();
    StaleOwnerDoesNotRetryAfterSuccessfulRecovery();
    InvalidationDuringRecoveryCannotRetryOldOwner();
    InvalidationRevisionRetiresRecoveringOwnerEvenForSameTuple();
    PendingFromNewLifecycleWinsWhenRecoveryTicketRevisionIsOld();
    ThreadedInvalidateDuringRecoveryUsesLatestLifecycle();
    RepeatedCurrentRequestMergesIntoOwnerAcrossRecovery();
    RejectedStalePendingLeavesCurrentOwnerRecovery();
    RecoveryIncarnationWrapSkipsZero();
    WrongRequestRecoveryIsRejected();
    CallbackBeforeInvalidatePreservesCurrentOwnerDelivery();
    InvalidateBeforeCallbackDiscardsCurrentOwnerDelivery();
    SimultaneousCallbackAndInvalidateHaveConsistentLinearization();
    CallbackBetweenDriverReturnAndCommitsCannotStrandEitherOwner();
    CallbackCannotEnterBetweenPhysicalAndLogicalCommit();
    CallbackLatchedBeforePreSubmissionFailureCannotStrandLease();
    WatchdogArmFailureEntersExactRecoveryAndRetriesNotification();
    CallbackWinningWatchdogRecoveryNeedsNoReset();
    std::cout << "PASS: BluFi WiFi scan controller host model\n";
    return 0;
}
