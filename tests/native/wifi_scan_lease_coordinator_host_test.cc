#include "../../components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

using Coordinator = WifiScanLeaseCoordinator;

namespace {

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

Coordinator::DrainProof RunDrainBarrier(
        const Coordinator::DrainDecision& drain, bool barrier_drained) {
    return WifiScanLeaseProofFactory::RunDrainBarrier(
        drain, [barrier_drained]() { return barrier_drained; });
}

Coordinator::RecoveryProof RunRecovery(
        const Coordinator::RecoveryDecision& recovery, bool driver_ready,
        bool barrier_drained) {
    return WifiScanLeaseProofFactory::RunRecovery(
        recovery, [driver_ready, barrier_drained]() {
            return WifiScanLeaseProofFactory::RecoveryOutcome{
                driver_ready,
                barrier_drained,
            };
        });
}

static_assert(!std::is_aggregate<Coordinator::DrainProof>::value);
static_assert(!std::is_constructible<Coordinator::DrainProof, uint64_t,
                                     bool>::value);
static_assert(!std::is_aggregate<Coordinator::RecoveryProof>::value);
static_assert(!std::is_constructible<Coordinator::RecoveryProof, uint64_t,
                                     bool, bool>::value);

void ExactIdentityOwnsEveryTransition() {
    Coordinator coordinator;
    const auto station = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(station.acquired);
    assert(station.lease.lease_id != 0);
    assert(station.lease.driver_incarnation != 0);
    assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

    const Coordinator::Lease foreign_owner{
        Coordinator::Owner::kBlufi,
        station.lease.lease_id,
        station.lease.driver_incarnation,
    };
    assert(!coordinator.ObserveScanDone(foreign_owner).consume_now);
    assert(!coordinator.CommitSubmission(foreign_owner, true).accepted);
    assert(!coordinator.BeginDrain(foreign_owner));

    const Coordinator::Lease wrong_incarnation{
        station.lease.owner,
        station.lease.lease_id,
        static_cast<uint32_t>(station.lease.driver_incarnation + 1),
    };
    assert(!coordinator.ObserveScanDone(wrong_incarnation).consume_now);
    assert(!coordinator.CommitSubmission(wrong_incarnation, true).accepted);
    assert(!coordinator.BeginDrain(wrong_incarnation));

    const auto committed = coordinator.CommitSubmission(station.lease, true);
    assert(committed.accepted);
    assert(!committed.consume_latched);
    assert(!committed.released);
    assert(!committed.callback_won_error);
    assert(coordinator.ObserveScanDone(station.lease).consume_now);
    assert(coordinator.FinishCompletion(station.lease));

    const auto blufi = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(blufi.acquired);
    assert(blufi.lease.lease_id != station.lease.lease_id);
}

void SimultaneousAcquireGrantsExactlyOneLease() {
    Coordinator coordinator;
    Barrier start(3);
    Coordinator::AcquireDecision station;
    Coordinator::AcquireDecision blufi;

    std::thread station_thread([&]() {
        start.ArriveAndWait();
        station = coordinator.TryAcquire(Coordinator::Owner::kStation);
    });
    std::thread blufi_thread([&]() {
        start.ArriveAndWait();
        blufi = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    });

    start.ArriveAndWait();
    station_thread.join();
    blufi_thread.join();
    assert(station.acquired != blufi.acquired);

    const auto winner = station.acquired ? station.lease : blufi.lease;
    const auto failed = coordinator.CommitSubmission(winner, false);
    assert(failed.drain_required);
    assert(!failed.released);
    const auto drain = coordinator.ArmDrainBarrier(winner);
    assert(drain.armed);
    assert(coordinator.CompleteDrain(winner, RunDrainBarrier(drain, true)));
}

void EarlyMatchingCallbackWaitsForSuccessfulCommit() {
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(acquired.acquired);

    Barrier before_callback(2);
    Barrier after_callback(2);
    Coordinator::CallbackDecision callback;
    std::thread event_thread([&]() {
        before_callback.ArriveAndWait();
        callback = coordinator.ObserveScanDone(acquired.lease);
        after_callback.ArriveAndWait();
    });

    before_callback.ArriveAndWait();
    after_callback.ArriveAndWait();
    assert(!callback.consume_now);
    assert(callback.deferred_until_commit);
    const auto duplicate = coordinator.ObserveScanDone(acquired.lease);
    assert(!duplicate.consume_now);
    assert(!duplicate.deferred_until_commit);

    const auto committed = coordinator.CommitSubmission(acquired.lease, true);
    assert(committed.accepted);
    assert(committed.consume_latched);
    assert(!committed.callback_won_error);
    assert(!coordinator.ObserveScanDone(acquired.lease).consume_now);
    assert(coordinator.FinishCompletion(acquired.lease));
    event_thread.join();
}

void CallbackRacingSynchronousErrorWinsExactlyOnce() {
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(acquired.acquired);

    Barrier callback_latched(2);
    std::thread event_thread([&]() {
        const auto callback = coordinator.ObserveScanDone(acquired.lease);
        assert(callback.deferred_until_commit);
        callback_latched.ArriveAndWait();
    });

    callback_latched.ArriveAndWait();
    const auto failed_start =
        coordinator.CommitSubmission(acquired.lease, false);
    assert(!failed_start.accepted);
    assert(failed_start.consume_latched);
    assert(!failed_start.released);
    assert(failed_start.callback_won_error);
    assert(!coordinator.ObserveScanDone(acquired.lease).consume_now);
    assert(coordinator.FinishCompletion(acquired.lease));
    assert(!coordinator.FinishCompletion(acquired.lease));
    event_thread.join();
}

void ErrorFirstThenQueuedCallbackRetainsOwnershipUntilConsumed() {
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kConfigAp);
    assert(acquired.acquired);

    Barrier handler_snapshotted_owner(2);
    Barrier allow_callback(2);
    std::thread event_thread([&, handler_lease = acquired.lease]() {
        handler_snapshotted_owner.ArriveAndWait();
        allow_callback.ArriveAndWait();
        const auto callback = coordinator.ObserveScanDone(handler_lease);
        assert(callback.consume_now);
        assert(coordinator.FinishCompletion(handler_lease));
    });

    handler_snapshotted_owner.ArriveAndWait();
    const auto failed_start =
        coordinator.CommitSubmission(acquired.lease, false);
    assert(!failed_start.accepted);
    assert(!failed_start.consume_latched);
    assert(!failed_start.released);
    assert(!failed_start.callback_won_error);
    assert(failed_start.drain_required);
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);

    allow_callback.ArriveAndWait();
    event_thread.join();
    assert(coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
}

void ErrorWithoutCallbackRequiresSuccessfulBarrier() {
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kConfigAp);
    assert(acquired.acquired);

    const auto failed_start =
        coordinator.CommitSubmission(acquired.lease, false);
    assert(failed_start.drain_required);
    assert(!failed_start.released);
    const auto drain = coordinator.ArmDrainBarrier(acquired.lease);
    assert(drain.armed);
    assert(!coordinator.CompleteDrain(acquired.lease,
                                      RunDrainBarrier(drain, false)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
    const auto retry = coordinator.ArmDrainBarrier(acquired.lease);
    assert(retry.armed);
    assert(retry.drain_id != drain.drain_id);
    assert(!coordinator.CompleteDrain(acquired.lease,
                                      RunDrainBarrier(drain, true)));
    assert(coordinator.CompleteDrain(acquired.lease,
                                     RunDrainBarrier(retry, true)));
    assert(coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
}

void ForeignOrStaleCallbackCannotClaimLease() {
    Coordinator coordinator;
    const auto first = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(first.acquired);
    assert(coordinator.CommitSubmission(first.lease, false).drain_required);
    const auto first_drain = coordinator.ArmDrainBarrier(first.lease);
    assert(first_drain.armed);
    assert(coordinator.CompleteDrain(first.lease,
                                     RunDrainBarrier(first_drain, true)));

    const auto current = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(current.acquired);
    assert(current.lease.lease_id != first.lease.lease_id);
    assert(!coordinator.ObserveScanDone(first.lease).consume_now);

    const Coordinator::Lease foreign{
        Coordinator::Owner::kStation,
        current.lease.lease_id,
        current.lease.driver_incarnation,
    };
    assert(!coordinator.ObserveScanDone(foreign).consume_now);

    assert(coordinator.CommitSubmission(current.lease, true).accepted);
    assert(coordinator.ObserveScanDone(current.lease).consume_now);
    assert(coordinator.FinishCompletion(current.lease));
}

void BarrierFailureRetainsDrainingLease() {
    Coordinator coordinator;
    const auto station = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(station.acquired);
    assert(coordinator.CommitSubmission(station.lease, true).accepted);
    assert(coordinator.BeginDrain(station.lease));
    const auto drain = coordinator.ArmDrainBarrier(station.lease);
    assert(drain.armed);
    assert(!coordinator.CompleteDrain(station.lease,
                                      RunDrainBarrier(drain, false)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

    const auto callback = coordinator.ObserveScanDone(station.lease);
    assert(callback.consume_now);
    assert(!callback.deferred_until_commit);
    assert(coordinator.FinishCompletion(station.lease));
    assert(coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
}

void SuccessfulDrainRejectsQueuedOldCallback() {
    Coordinator coordinator;
    const auto station = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(station.acquired);
    assert(coordinator.CommitSubmission(station.lease, true).accepted);
    assert(coordinator.BeginDrain(station.lease));
    const auto drain = coordinator.ArmDrainBarrier(station.lease);
    assert(drain.armed);
    assert(coordinator.CompleteDrain(station.lease,
                                     RunDrainBarrier(drain, true)));

    const auto blufi = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(blufi.acquired);
    assert(!coordinator.ObserveScanDone(station.lease).consume_now);
    assert(!coordinator.ObserveScanDone(station.lease).deferred_until_commit);
    assert(coordinator.CommitSubmission(blufi.lease, false).drain_required);
    const auto blufi_drain = coordinator.ArmDrainBarrier(blufi.lease);
    assert(blufi_drain.armed);
    assert(coordinator.CompleteDrain(blufi.lease,
                                     RunDrainBarrier(blufi_drain, true)));
}

void StopDuringSubmissionCannotResurrectOrReleaseEarly() {
    for (const bool driver_accepted : {false, true}) {
        Coordinator coordinator;
        const auto acquired =
            coordinator.TryAcquire(Coordinator::Owner::kStation);
        assert(acquired.acquired);

        Barrier submission_entered(2);
        Barrier return_from_driver(2);
        Coordinator::CommitDecision committed;
        std::thread submission_thread([&]() {
            submission_entered.ArriveAndWait();
            return_from_driver.ArriveAndWait();
            committed = coordinator.CommitSubmission(acquired.lease,
                                                       driver_accepted);
        });

        submission_entered.ArriveAndWait();
        assert(coordinator.BeginDrain(acquired.lease));
        const auto premature = coordinator.ArmDrainBarrier(acquired.lease);
        assert(!premature.armed);
        const auto stale_precommit_proof = RunDrainBarrier(premature, true);
        assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

        return_from_driver.ArriveAndWait();
        submission_thread.join();
        assert(committed.accepted == driver_accepted);
        assert(committed.drain_required);
        assert(!committed.released);
        assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

        const auto fresh_drain = coordinator.ArmDrainBarrier(acquired.lease);
        assert(fresh_drain.armed);
        assert(!coordinator.CompleteDrain(acquired.lease,
                                          stale_precommit_proof));
        assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

        if (driver_accepted) {
            const auto callback = coordinator.ObserveScanDone(acquired.lease);
            assert(callback.consume_now);
            assert(coordinator.FinishCompletion(acquired.lease));
        } else {
            assert(coordinator.CompleteDrain(
                acquired.lease, RunDrainBarrier(fresh_drain, true)));
        }
        assert(coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
    }
}

void StopThenEarlyCallbackStillWaitsForSubmissionCommit() {
    for (const bool driver_accepted : {false, true}) {
        Coordinator coordinator;
        const auto acquired =
            coordinator.TryAcquire(Coordinator::Owner::kStation);
        assert(acquired.acquired);
        assert(coordinator.BeginDrain(acquired.lease));

        const auto callback = coordinator.ObserveScanDone(acquired.lease);
        assert(!callback.consume_now);
        assert(callback.deferred_until_commit);

        const auto committed =
            coordinator.CommitSubmission(acquired.lease, driver_accepted);
        assert(committed.accepted == driver_accepted);
        assert(committed.consume_latched);
        assert(committed.callback_won_error == !driver_accepted);
        assert(!committed.drain_required);
        assert(coordinator.FinishCompletion(acquired.lease));
    }
}

void RecoveryAdvancesIncarnationBeforeNextAcquire() {
    Coordinator coordinator;
    const auto lost = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(lost.acquired);
    assert(coordinator.CommitSubmission(lost.lease, true).accepted);
    const auto recovery = coordinator.BeginRecovery(lost.lease);
    assert(recovery.begun);

    assert(!coordinator.CompleteRecovery(
        lost.lease, RunRecovery(recovery, true, false)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
    assert(!coordinator.CompleteRecovery(
        lost.lease, RunRecovery(recovery, false, true)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);

    assert(coordinator.CompleteRecovery(
        lost.lease, RunRecovery(recovery, true, true)));

    const auto recovered =
        coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(recovered.acquired);
    assert(recovered.lease.driver_incarnation !=
           lost.lease.driver_incarnation);
    assert(coordinator.CommitSubmission(recovered.lease, true).accepted);
    const auto second_recovery = coordinator.BeginRecovery(recovered.lease);
    assert(second_recovery.begun);
    assert(second_recovery.recovery_id != recovery.recovery_id);
    assert(!coordinator.CompleteRecovery(
        recovered.lease, RunRecovery(recovery, true, true)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
    assert(coordinator.CompleteRecovery(
        recovered.lease, RunRecovery(second_recovery, true, true)));

    assert(!coordinator.ObserveScanDone(lost.lease).consume_now);
    const auto final = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(final.acquired);
    assert(final.lease.driver_incarnation !=
           recovered.lease.driver_incarnation);
    assert(coordinator.CommitSubmission(final.lease, false).drain_required);
    const auto recovered_drain =
        coordinator.ArmDrainBarrier(final.lease);
    assert(recovered_drain.armed);
    assert(coordinator.CompleteDrain(
        final.lease, RunDrainBarrier(recovered_drain, true)));
}

void ExhaustedLeaseIdsAndIncarnationsFailClosed() {
    const auto max_lease_id = std::numeric_limits<uint64_t>::max();
    const auto max_incarnation = std::numeric_limits<uint32_t>::max();

    Coordinator one_lease_left(max_lease_id - 1, 1);
    const auto last = one_lease_left.TryAcquire(Coordinator::Owner::kStation);
    assert(last.acquired);
    assert(last.lease.lease_id == max_lease_id);
    assert(one_lease_left.CommitSubmission(last.lease, false).drain_required);
    const auto last_drain = one_lease_left.ArmDrainBarrier(last.lease);
    assert(last_drain.armed);
    assert(one_lease_left.CompleteDrain(
        last.lease, RunDrainBarrier(last_drain, true)));
    assert(!one_lease_left.TryAcquire(Coordinator::Owner::kBlufi).acquired);

    Coordinator incarnation_exhausted(0, max_incarnation - 1);
    const auto lease =
        incarnation_exhausted.TryAcquire(Coordinator::Owner::kBlufi);
    assert(lease.acquired);
    assert(incarnation_exhausted.CommitSubmission(lease.lease, true).accepted);
    const auto first_recovery =
        incarnation_exhausted.BeginRecovery(lease.lease);
    assert(first_recovery.begun);
    assert(incarnation_exhausted.CompleteRecovery(
        lease.lease, RunRecovery(first_recovery, true, true)));

    const auto final_incarnation =
        incarnation_exhausted.TryAcquire(Coordinator::Owner::kBlufi);
    assert(final_incarnation.acquired);
    assert(final_incarnation.lease.driver_incarnation == max_incarnation);
    assert(incarnation_exhausted.CommitSubmission(final_incarnation.lease, true)
               .accepted);
    const auto exhausted_recovery =
        incarnation_exhausted.BeginRecovery(final_incarnation.lease);
    assert(exhausted_recovery.begun);
    assert(!incarnation_exhausted.CompleteRecovery(
        final_incarnation.lease,
        RunRecovery(exhausted_recovery, true, true)));
    assert(!incarnation_exhausted.TryAcquire(Coordinator::Owner::kStation)
                .acquired);

    Coordinator invalid_incarnation(0, 0);
    assert(!invalid_incarnation.TryAcquire(Coordinator::Owner::kStation)
                .acquired);

    Coordinator drain_id_exhausted(0, 1, max_lease_id, 0);
    const auto draining =
        drain_id_exhausted.TryAcquire(Coordinator::Owner::kStation);
    assert(draining.acquired);
    assert(drain_id_exhausted.CommitSubmission(draining.lease, false)
               .drain_required);
    assert(!drain_id_exhausted.ArmDrainBarrier(draining.lease).armed);
    assert(!drain_id_exhausted.TryAcquire(Coordinator::Owner::kBlufi)
                .acquired);

    Coordinator recovery_id_exhausted(0, 1, 0, max_lease_id);
    const auto recovering =
        recovery_id_exhausted.TryAcquire(Coordinator::Owner::kBlufi);
    assert(recovering.acquired);
    assert(recovery_id_exhausted.CommitSubmission(recovering.lease, true)
               .accepted);
    assert(!recovery_id_exhausted.BeginRecovery(recovering.lease).begun);
    assert(!recovery_id_exhausted.TryAcquire(Coordinator::Owner::kStation)
                .acquired);
}

}  // namespace

int main() {
    ExactIdentityOwnsEveryTransition();
    SimultaneousAcquireGrantsExactlyOneLease();
    EarlyMatchingCallbackWaitsForSuccessfulCommit();
    CallbackRacingSynchronousErrorWinsExactlyOnce();
    ErrorFirstThenQueuedCallbackRetainsOwnershipUntilConsumed();
    ErrorWithoutCallbackRequiresSuccessfulBarrier();
    ForeignOrStaleCallbackCannotClaimLease();
    BarrierFailureRetainsDrainingLease();
    SuccessfulDrainRejectsQueuedOldCallback();
    StopDuringSubmissionCannotResurrectOrReleaseEarly();
    StopThenEarlyCallbackStillWaitsForSubmissionCommit();
    RecoveryAdvancesIncarnationBeforeNextAcquire();
    ExhaustedLeaseIdsAndIncarnationsFailClosed();
    std::cout << "wifi_scan_lease_coordinator_host_test: PASS\n";
    return 0;
}
