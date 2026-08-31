#include "../../components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

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
    assert(coordinator.CommitSubmission(winner, false).released);
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

void SynchronousErrorWithoutCallbackReleasesLease() {
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kConfigAp);
    assert(acquired.acquired);

    const auto failed_start =
        coordinator.CommitSubmission(acquired.lease, false);
    assert(!failed_start.accepted);
    assert(!failed_start.consume_latched);
    assert(failed_start.released);
    assert(!failed_start.callback_won_error);
    assert(!coordinator.ObserveScanDone(acquired.lease).consume_now);
    assert(coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
}

void ForeignOrStaleCallbackCannotClaimLease() {
    Coordinator coordinator;
    const auto first = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(first.acquired);
    assert(coordinator.CommitSubmission(first.lease, false).released);

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
    assert(!coordinator.CompleteDrain(station.lease, false));
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
    assert(coordinator.CompleteDrain(station.lease, true));

    const auto blufi = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(blufi.acquired);
    assert(!coordinator.ObserveScanDone(station.lease).consume_now);
    assert(!coordinator.ObserveScanDone(station.lease).deferred_until_commit);
    assert(coordinator.CommitSubmission(blufi.lease, false).released);
}

void RecoveryAdvancesIncarnationBeforeNextAcquire() {
    Coordinator coordinator;
    const auto lost = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(lost.acquired);
    assert(coordinator.CommitSubmission(lost.lease, true).accepted);
    assert(coordinator.BeginRecovery(lost.lease));

    assert(!coordinator.CompleteRecovery(lost.lease, true, false));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
    assert(!coordinator.CompleteRecovery(lost.lease, false, true));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
    assert(coordinator.CompleteRecovery(lost.lease, true, true));

    const auto recovered =
        coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(recovered.acquired);
    assert(recovered.lease.driver_incarnation !=
           lost.lease.driver_incarnation);
    assert(!coordinator.ObserveScanDone(lost.lease).consume_now);
    assert(coordinator.CommitSubmission(recovered.lease, false).released);
}

}  // namespace

int main() {
    ExactIdentityOwnsEveryTransition();
    SimultaneousAcquireGrantsExactlyOneLease();
    EarlyMatchingCallbackWaitsForSuccessfulCommit();
    CallbackRacingSynchronousErrorWinsExactlyOnce();
    SynchronousErrorWithoutCallbackReleasesLease();
    ForeignOrStaleCallbackCannotClaimLease();
    BarrierFailureRetainsDrainingLease();
    SuccessfulDrainRejectsQueuedOldCallback();
    RecoveryAdvancesIncarnationBeforeNextAcquire();
    std::cout << "wifi_scan_lease_coordinator_host_test: PASS\n";
    return 0;
}
