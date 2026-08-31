#include "../../components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"

#include <cassert>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using Coordinator = WifiScanLeaseCoordinator;

enum class ExecutorStep : uint8_t {
    kScanStop,
    kWifiStop,
    kWifiDeinit,
    kBarrier,
    kWifiInit,
};

struct RecoveryEnvironment {
    bool driver_will_recover = true;
    bool barrier_will_drain = true;
    std::vector<ExecutorStep> steps;
};

class WifiScanRecoveryExecutor {
public:
    static Coordinator::RecoveryProof Execute(
            const Coordinator::RecoveryDecision& recovery,
            RecoveryEnvironment& environment) {
        if (!recovery.begun() || recovery.recovery_id() == 0) {
            return Coordinator::RecoveryProof{};
        }

        environment.steps.push_back(ExecutorStep::kScanStop);
        environment.steps.push_back(ExecutorStep::kWifiStop);
        environment.steps.push_back(ExecutorStep::kWifiDeinit);
        environment.steps.push_back(ExecutorStep::kBarrier);
        environment.steps.push_back(ExecutorStep::kWifiInit);
        return Coordinator::RecoveryProof{
            recovery.recovery_id(),
            environment.driver_will_recover,
            environment.barrier_will_drain,
        };
    }
};

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

Coordinator::RecoveryProof RunRecovery(
        const Coordinator::RecoveryDecision& recovery, bool driver_ready,
        bool barrier_drained) {
    RecoveryEnvironment environment;
    environment.driver_will_recover = driver_ready;
    environment.barrier_will_drain = barrier_drained;
    return WifiScanRecoveryExecutor::Execute(recovery, environment);
}

static_assert(!std::is_aggregate<Coordinator::RecoveryProof>::value);
static_assert(!std::is_constructible<Coordinator::RecoveryProof, uint64_t,
                                     bool, bool>::value);
static_assert(!std::is_aggregate<Coordinator::RecoveryDecision>::value);
static_assert(!std::is_constructible<Coordinator::RecoveryDecision, bool,
                                     uint64_t>::value);

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
    assert(coordinator.ObserveScanDone(winner).consume_now);
    assert(coordinator.FinishCompletion(winner));
}

void StationAndConfigTimersReachOnePhysicalSubmission() {
    Coordinator coordinator;
    Barrier start(3);
    Coordinator::AcquireDecision station;
    Coordinator::AcquireDecision config_ap;
    unsigned station_submissions = 0;
    unsigned config_submissions = 0;

    std::thread station_timer([&]() {
        start.ArriveAndWait();
        station = coordinator.TryAcquire(Coordinator::Owner::kStation);
        if (station.acquired) {
            ++station_submissions;
        }
    });
    std::thread config_timer([&]() {
        start.ArriveAndWait();
        config_ap = coordinator.TryAcquire(Coordinator::Owner::kConfigAp);
        if (config_ap.acquired) {
            ++config_submissions;
        }
    });

    start.ArriveAndWait();
    station_timer.join();
    config_timer.join();
    assert(station.acquired != config_ap.acquired);
    assert(station_submissions + config_submissions == 1);

    const auto winner = station.acquired ? station.lease : config_ap.lease;
    assert(coordinator.CommitSubmission(winner, true).accepted);
    assert(coordinator.ObserveScanDone(winner).consume_now);
    assert(coordinator.FinishCompletion(winner));
}

void StationAndConfigEarlyCallbackCompletesAfterCommit() {
    Coordinator coordinator;
    const auto config =
        coordinator.TryAcquire(Coordinator::Owner::kConfigAp);
    assert(config.acquired);
    const auto callback = coordinator.ObserveScanDone(config.lease);
    assert(callback.deferred_until_commit);
    assert(!callback.consume_now);
    const auto commit = coordinator.CommitSubmission(config.lease, true);
    assert(commit.consume_latched);
    assert(coordinator.FinishCompletion(config.lease));
    assert(coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
}

void StationStopDuringStartingOrRunningBlocksConfigUntilCallback() {
    for (const bool stop_while_starting : {false, true}) {
        Coordinator coordinator;
        const auto station =
            coordinator.TryAcquire(Coordinator::Owner::kStation);
        assert(station.acquired);
        if (!stop_while_starting) {
            assert(coordinator.CommitSubmission(station.lease, true).accepted);
        }
        assert(coordinator.BeginDrain(station.lease));
        if (stop_while_starting) {
            const auto commit =
                coordinator.CommitSubmission(station.lease, true);
            assert(commit.drain_required);
        }
        assert(!coordinator.TryAcquire(Coordinator::Owner::kConfigAp).acquired);
        assert(coordinator.ObserveScanDone(station.lease).consume_now);
        assert(coordinator.FinishCompletion(station.lease));
        assert(coordinator.TryAcquire(Coordinator::Owner::kConfigAp).acquired);
    }
}

void MissingStationCallbackKeepsConfigBlocked() {
    Coordinator coordinator;
    const auto station = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(station.acquired);
    assert(coordinator.CommitSubmission(station.lease, true).accepted);
    assert(coordinator.BeginDrain(station.lease));

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        assert(!coordinator.TryAcquire(Coordinator::Owner::kConfigAp).acquired);
    }

    const auto callback = coordinator.ObserveScanDone(station.lease);
    assert(callback.consume_now);
    assert(coordinator.FinishCompletion(station.lease));
    assert(coordinator.TryAcquire(Coordinator::Owner::kConfigAp).acquired);
}

class SerializedScannerModel {
public:
    SerializedScannerModel(Coordinator& coordinator, Coordinator::Owner owner)
        : coordinator_(coordinator), owner_(owner) {}

    bool Start(Barrier* driver_entered = nullptr,
               Barrier* allow_driver_return = nullptr) {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (!enabled_ || lease_.lease_id != 0) {
            return false;
        }
        const auto acquired = coordinator_.TryAcquire(owner_);
        if (!acquired.acquired) {
            return false;
        }
        lease_ = acquired.lease;
        ++physical_starts_;
        if (driver_entered != nullptr) {
            driver_entered->ArriveAndWait();
        }
        if (allow_driver_return != nullptr) {
            allow_driver_return->ArriveAndWait();
        }
        coordinator_.CommitSubmission(lease_, true);
        return true;
    }

    void Stop() {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        enabled_ = false;
        if (lease_.lease_id != 0) {
            coordinator_.BeginDrain(lease_);
            ++physical_stops_;
        }
    }

    void Complete(bool publish, Barrier* completion_holds_lock = nullptr,
                  Barrier* allow_delivery = nullptr) {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (completion_holds_lock != nullptr) {
            completion_holds_lock->ArriveAndWait();
        }
        if (allow_delivery != nullptr) {
            allow_delivery->ArriveAndWait();
        }
        if (coordinator_.FinishCompletion(lease_)) {
            lease_ = Coordinator::Lease{};
            if (enabled_) {
                if (publish) {
                    ++published_results_;
                } else {
                    ++connect_starts_;
                }
            }
        }
    }

    Coordinator::Lease lease() const { return lease_; }
    unsigned physical_starts() const { return physical_starts_.load(); }
    unsigned physical_stops() const { return physical_stops_.load(); }
    unsigned connect_starts() const { return connect_starts_.load(); }
    unsigned published_results() const { return published_results_.load(); }

private:
    Coordinator& coordinator_;
    Coordinator::Owner owner_;
    std::mutex lifecycle_mutex_;
    bool enabled_ = true;
    Coordinator::Lease lease_;
    std::atomic<unsigned> physical_starts_{0};
    std::atomic<unsigned> physical_stops_{0};
    std::atomic<unsigned> connect_starts_{0};
    std::atomic<unsigned> published_results_{0};
};

void StopCannotOvertakePhysicalStartAndCommit() {
    Coordinator coordinator;
    SerializedScannerModel scanner(coordinator, Coordinator::Owner::kStation);
    Barrier driver_entered(2);
    Barrier allow_driver_return(2);

    std::thread start([&]() {
        assert(scanner.Start(&driver_entered, &allow_driver_return));
    });
    driver_entered.ArriveAndWait();
    std::thread stop([&]() { scanner.Stop(); });
    assert(scanner.physical_starts() == 1);
    assert(scanner.physical_stops() == 0);
    allow_driver_return.ArriveAndWait();
    start.join();
    stop.join();
    assert(scanner.physical_stops() == 1);
    assert(!scanner.Start());
    assert(scanner.physical_starts() == 1);
}

void CompletionAndStopHaveOneDeterministicWinner() {
    for (const auto scenario : {
             std::pair<Coordinator::Owner, bool>{
                 Coordinator::Owner::kStation, false},
             std::pair<Coordinator::Owner, bool>{
                 Coordinator::Owner::kConfigAp, true},
         }) {
        Coordinator coordinator;
        SerializedScannerModel scanner(coordinator, scenario.first);
        assert(scanner.Start());
        const auto lease = scanner.lease();
        assert(coordinator.ObserveScanDone(lease).consume_now);
        Barrier completion_holds_lock(2);
        Barrier allow_delivery(2);
        std::thread completion([&]() {
            scanner.Complete(scenario.second, &completion_holds_lock,
                             &allow_delivery);
        });
        completion_holds_lock.ArriveAndWait();
        std::thread stop([&]() { scanner.Stop(); });
        assert(scanner.connect_starts() == 0);
        assert(scanner.published_results() == 0);
        allow_delivery.ArriveAndWait();
        completion.join();
        stop.join();
        assert(scanner.connect_starts() == (scenario.second ? 0U : 1U));
        assert(scanner.published_results() == (scenario.second ? 1U : 0U));
    }

    for (const auto scenario : {
             std::pair<Coordinator::Owner, bool>{
                 Coordinator::Owner::kStation, false},
             std::pair<Coordinator::Owner, bool>{
                 Coordinator::Owner::kConfigAp, true},
         }) {
        Coordinator coordinator;
        SerializedScannerModel scanner(coordinator, scenario.first);
        assert(scanner.Start());
        const auto lease = scanner.lease();
        scanner.Stop();
        assert(coordinator.ObserveScanDone(lease).consume_now);
        scanner.Complete(scenario.second);
        assert(scanner.connect_starts() == 0);
        assert(scanner.published_results() == 0);
    }
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

void ErrorWithoutCallbackStaysBlockedUntilRecovery() {
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kConfigAp);
    assert(acquired.acquired);

    const auto failed_start =
        coordinator.CommitSubmission(acquired.lease, false);
    assert(failed_start.drain_required);
    assert(!failed_start.released);
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);

    const auto recovery = coordinator.BeginRecovery(acquired.lease);
    assert(recovery.begun());
    assert(!coordinator.CompleteRecovery(
        acquired.lease, RunRecovery(recovery, true, false)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
    assert(coordinator.CompleteRecovery(
        acquired.lease, RunRecovery(recovery, true, true)));
    assert(coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
}

void ForeignOrStaleCallbackCannotClaimLease() {
    Coordinator coordinator;
    const auto first = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(first.acquired);
    assert(coordinator.CommitSubmission(first.lease, false).drain_required);
    assert(coordinator.ObserveScanDone(first.lease).consume_now);
    assert(coordinator.FinishCompletion(first.lease));

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

void DrainingCallbackReleasesLease() {
    Coordinator coordinator;
    const auto station = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(station.acquired);
    assert(coordinator.CommitSubmission(station.lease, true).accepted);
    assert(coordinator.BeginDrain(station.lease));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

    const auto callback = coordinator.ObserveScanDone(station.lease);
    assert(callback.consume_now);
    assert(!callback.deferred_until_commit);
    assert(coordinator.FinishCompletion(station.lease));
    assert(coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
}

void DrainingLeaseBlocksUntilCallbackAndRejectsItAfterRelease() {
    Coordinator coordinator;
    const auto station = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(station.acquired);
    assert(coordinator.CommitSubmission(station.lease, true).accepted);
    assert(coordinator.BeginDrain(station.lease));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
    assert(coordinator.ObserveScanDone(station.lease).consume_now);
    assert(coordinator.FinishCompletion(station.lease));

    const auto blufi = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(blufi.acquired);
    assert(!coordinator.ObserveScanDone(station.lease).consume_now);
    assert(!coordinator.ObserveScanDone(station.lease).deferred_until_commit);
    assert(coordinator.CommitSubmission(blufi.lease, false).drain_required);
    const auto recovery = coordinator.BeginRecovery(blufi.lease);
    assert(recovery.begun());
    assert(coordinator.CompleteRecovery(
        blufi.lease, RunRecovery(recovery, true, true)));
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
        assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

        return_from_driver.ArriveAndWait();
        submission_thread.join();
        assert(committed.accepted == driver_accepted);
        assert(committed.drain_required);
        assert(!committed.released);
        assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

        if (driver_accepted) {
            const auto callback = coordinator.ObserveScanDone(acquired.lease);
            assert(callback.consume_now);
            assert(coordinator.FinishCompletion(acquired.lease));
        } else {
            const auto recovery = coordinator.BeginRecovery(acquired.lease);
            assert(recovery.begun());
            assert(coordinator.CompleteRecovery(
                acquired.lease, RunRecovery(recovery, true, true)));
        }
        assert(coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
    }
}

void StopAndStandaloneBarrierCannotReleaseDrainingLease() {
    Coordinator coordinator;
    const auto acquired =
        coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(acquired.acquired);

    assert(coordinator.BeginDrain(acquired.lease));
    std::vector<ExecutorStep> external_steps{
        ExecutorStep::kScanStop,
        ExecutorStep::kBarrier,
    };

    const auto committed = coordinator.CommitSubmission(acquired.lease, true);
    assert(committed.accepted);
    assert(committed.drain_required);
    const std::vector<ExecutorStep> expected{
        ExecutorStep::kScanStop,
        ExecutorStep::kBarrier,
    };
    assert(external_steps == expected);
    assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);

    const auto recovery = coordinator.BeginRecovery(acquired.lease);
    assert(recovery.begun());
    assert(coordinator.CompleteRecovery(
        acquired.lease, RunRecovery(recovery, true, true)));
    assert(coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
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
    assert(recovery.begun());

    assert(!coordinator.CompleteRecovery(
        lost.lease, RunRecovery(recovery, true, false)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
    assert(!coordinator.CompleteRecovery(
        lost.lease, RunRecovery(recovery, false, true)));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);

    RecoveryEnvironment recovery_environment;
    const auto recovery_proof =
        WifiScanRecoveryExecutor::Execute(recovery, recovery_environment);
    const std::vector<ExecutorStep> expected_recovery{
        ExecutorStep::kScanStop,
        ExecutorStep::kWifiStop,
        ExecutorStep::kWifiDeinit,
        ExecutorStep::kBarrier,
        ExecutorStep::kWifiInit,
    };
    assert(recovery_environment.steps == expected_recovery);
    assert(coordinator.CompleteRecovery(lost.lease, recovery_proof));

    const auto recovered =
        coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(recovered.acquired);
    assert(recovered.lease.driver_incarnation !=
           lost.lease.driver_incarnation);
    assert(coordinator.CommitSubmission(recovered.lease, true).accepted);
    const auto second_recovery = coordinator.BeginRecovery(recovered.lease);
    assert(second_recovery.begun());
    assert(second_recovery.recovery_id() != recovery.recovery_id());
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
    const auto final_recovery = coordinator.BeginRecovery(final.lease);
    assert(final_recovery.begun());
    assert(coordinator.CompleteRecovery(
        final.lease, RunRecovery(final_recovery, true, true)));
}

void ExhaustedLeaseIdsAndIncarnationsFailClosed() {
    const auto max_lease_id = std::numeric_limits<uint64_t>::max();
    const auto max_incarnation = std::numeric_limits<uint32_t>::max();

    Coordinator one_lease_left(max_lease_id - 1, 1);
    const auto last = one_lease_left.TryAcquire(Coordinator::Owner::kStation);
    assert(last.acquired);
    assert(last.lease.lease_id == max_lease_id);
    assert(one_lease_left.CommitSubmission(last.lease, false).drain_required);
    assert(one_lease_left.ObserveScanDone(last.lease).consume_now);
    assert(one_lease_left.FinishCompletion(last.lease));
    assert(!one_lease_left.TryAcquire(Coordinator::Owner::kBlufi).acquired);

    Coordinator incarnation_exhausted(0, max_incarnation - 1);
    const auto lease =
        incarnation_exhausted.TryAcquire(Coordinator::Owner::kBlufi);
    assert(lease.acquired);
    assert(incarnation_exhausted.CommitSubmission(lease.lease, true).accepted);
    const auto first_recovery =
        incarnation_exhausted.BeginRecovery(lease.lease);
    assert(first_recovery.begun());
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
    assert(exhausted_recovery.begun());
    assert(!incarnation_exhausted.CompleteRecovery(
        final_incarnation.lease,
        RunRecovery(exhausted_recovery, true, true)));
    assert(!incarnation_exhausted.TryAcquire(Coordinator::Owner::kStation)
                .acquired);

    Coordinator invalid_incarnation(0, 0);
    assert(!invalid_incarnation.TryAcquire(Coordinator::Owner::kStation)
                .acquired);

    Coordinator recovery_id_exhausted(0, 1, max_lease_id);
    const auto recovering =
        recovery_id_exhausted.TryAcquire(Coordinator::Owner::kBlufi);
    assert(recovering.acquired);
    assert(recovery_id_exhausted.CommitSubmission(recovering.lease, true)
               .accepted);
    assert(!recovery_id_exhausted.BeginRecovery(recovering.lease).begun());
    assert(!recovery_id_exhausted.TryAcquire(Coordinator::Owner::kStation)
                .acquired);
}

}  // namespace

int main() {
    ExactIdentityOwnsEveryTransition();
    SimultaneousAcquireGrantsExactlyOneLease();
    StationAndConfigTimersReachOnePhysicalSubmission();
    StationAndConfigEarlyCallbackCompletesAfterCommit();
    StationStopDuringStartingOrRunningBlocksConfigUntilCallback();
    MissingStationCallbackKeepsConfigBlocked();
    StopCannotOvertakePhysicalStartAndCommit();
    CompletionAndStopHaveOneDeterministicWinner();
    EarlyMatchingCallbackWaitsForSuccessfulCommit();
    CallbackRacingSynchronousErrorWinsExactlyOnce();
    ErrorFirstThenQueuedCallbackRetainsOwnershipUntilConsumed();
    ErrorWithoutCallbackStaysBlockedUntilRecovery();
    ForeignOrStaleCallbackCannotClaimLease();
    DrainingCallbackReleasesLease();
    DrainingLeaseBlocksUntilCallbackAndRejectsItAfterRelease();
    StopDuringSubmissionCannotResurrectOrReleaseEarly();
    StopAndStandaloneBarrierCannotReleaseDrainingLease();
    StopThenEarlyCallbackStillWaitsForSubmissionCommit();
    RecoveryAdvancesIncarnationBeforeNextAcquire();
    ExhaustedLeaseIdsAndIncarnationsFailClosed();
    std::cout << "wifi_scan_lease_coordinator_host_test: PASS\n";
    return 0;
}
