#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_lease_state.h"
#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_policy.h"
#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_retry_state.h"
#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_completion_status.h"

#include <cassert>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using Coordinator = WifiScanLeaseCoordinator;
using State = BlockingWifiScanLeaseState;

namespace {

Coordinator::Lease Lease(uint64_t id) {
    return {Coordinator::Owner::kBlockingUi, id, 7};
}

void EarlyCallbackWakesOnlyAfterCommit() {
    Coordinator coordinator;
    State state;
    const auto acquired =
        coordinator.TryAcquire(Coordinator::Owner::kBlockingUi);
    assert(acquired.acquired);
    const auto lease = acquired.lease;
    assert(state.Begin(lease));
    const auto callback = coordinator.ObserveScanDone(lease);
    assert(callback.deferred_until_commit);
    assert(state.OnCallback(lease) == State::CallbackAction::kWakeWaiter);
    assert(coordinator.CommitSubmission(lease, true).consume_latched);
    assert(state.CallbackClaimed(lease));
    assert(coordinator.FinishCompletion(lease));
    assert(state.FinishNormally(lease));
}

void BusyAndForeignCallbacksDoNotDisturbOwner() {
    Coordinator coordinator;
    State state;
    const auto acquired =
        coordinator.TryAcquire(Coordinator::Owner::kBlockingUi);
    assert(acquired.acquired);
    const auto lease = acquired.lease;
    assert(state.Begin(lease));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kStation).acquired);
    assert(!state.Begin(Lease(3)));
    assert(state.OnCallback(Lease(3)) == State::CallbackAction::kIgnore);
    assert(!state.CallbackClaimed(Lease(3)));
    assert(state.Owns(lease));
}

void FailedInitializationCanAbandonOnlyTheExactUnsubmittedLease() {
    State state;
    const auto lease = Lease(30);
    assert(state.Begin(lease));
    assert(!state.AbandonUnsubmitted(Lease(31)));
    assert(state.AbandonUnsubmitted(lease));
    assert(!state.Owns(lease));
}

void MissingCallbackRetainsExactRecoveryDebt() {
    Coordinator coordinator;
    State state;
    const auto acquired =
        coordinator.TryAcquire(Coordinator::Owner::kBlockingUi);
    assert(acquired.acquired);
    const auto lease = acquired.lease;
    assert(state.Begin(lease));
    assert(coordinator.CommitSubmission(lease, true).accepted);
    assert(state.DetachWaiterForRecovery(lease));
    assert(coordinator.BeginDrain(lease));
    assert(state.HasDebt(lease));
    assert(!state.HasDebt(Lease(5)));
    assert(state.OnCallback(lease) ==
           State::CallbackAction::kCompleteWithoutWaiter);
}

void CallbackBeatingTimeoutStaysWithWaiter() {
    State state;
    const auto lease = Lease(6);
    assert(state.Begin(lease));
    assert(state.OnCallback(lease) == State::CallbackAction::kWakeWaiter);
    assert(!state.DetachWaiterForRecovery(lease));
    assert(!state.HasDebt(lease));
}

void BlockingReturnBeforeEventWaitsForAuthenticatedCallback() {
    Coordinator coordinator;
    State state;
    const auto acquired =
        coordinator.TryAcquire(Coordinator::Owner::kBlockingUi);
    assert(acquired.acquired);
    const auto lease = acquired.lease;
    assert(state.Begin(lease));
    assert(coordinator.CommitSubmission(lease, true).accepted);
    assert(!state.CallbackClaimed(lease));
    assert(coordinator.ObserveScanDone(lease).consume_now);
    assert(state.OnCallback(lease) == State::CallbackAction::kWakeWaiter);
    assert(state.CallbackClaimed(lease));
}

void CallbackAndTimeoutChooseOneCompletionOwner() {
    State state;
    const auto lease = Lease(20);
    assert(state.Begin(lease));
    std::mutex mutex;
    std::condition_variable condition;
    bool start = false;
    State::CallbackAction callback_action = State::CallbackAction::kIgnore;
    bool timeout_detached = false;
    std::thread callback([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&]() { return start; });
        lock.unlock();
        callback_action = state.OnCallback(lease);
    });
    std::thread timeout([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&]() { return start; });
        lock.unlock();
        timeout_detached = state.DetachWaiterForRecovery(lease);
    });
    {
        std::lock_guard<std::mutex> lock(mutex);
        start = true;
    }
    condition.notify_all();
    callback.join();
    timeout.join();
    assert(timeout_detached !=
           (callback_action == State::CallbackAction::kWakeWaiter));
    assert(timeout_detached == state.HasDebt(lease));
}

void CleanupFailureAndRecoveryRemainExact() {
    Coordinator coordinator;
    State state;
    const auto acquired =
        coordinator.TryAcquire(Coordinator::Owner::kBlockingUi);
    assert(acquired.acquired);
    const auto lease = acquired.lease;
    assert(state.Begin(lease));
    assert(coordinator.CommitSubmission(lease, true).accepted);
    assert(coordinator.ObserveScanDone(lease).consume_now);
    assert(state.OnCallback(lease) == State::CallbackAction::kWakeWaiter);
    assert(coordinator.RetainFailedCompletion(lease));
    assert(state.RetainCompletionForRecovery(lease));
    assert(state.HasDebt(lease));
    assert(!state.FinishRecovery(Lease(8)));
    assert(state.FinishRecovery(lease));
    assert(!state.Owns(lease));
    assert(state.Begin(Lease(9)));
}

void RepeatedScansResetAllState() {
    State state;
    for (uint64_t id = 10; id != 13; ++id) {
        const auto lease = Lease(id);
        assert(state.Begin(lease));
        assert(state.OnCallback(lease) == State::CallbackAction::kWakeWaiter);
        assert(state.FinishNormally(lease));
    }
}

void FullChannelActiveScanWaitIncludesEveryChannelAndSchedulingMargin() {
    constexpr uint32_t wait_ms = BlockingWifiScanPolicy::CompletionWaitMs(
        14, 300, 1000);
    static_assert(wait_ms == 5200);
    assert(wait_ms > 14 * 300);
}

void SlowFullChannelCallbackAtBoundaryIsNotClassifiedAsLost() {
    const auto wait_ms = BlockingWifiScanPolicy::CompletionWaitMs(14, 300, 1000);
    assert(BlockingWifiScanPolicy::CallbackArrivedBeforeDeadline(
        wait_ms - 1, wait_ms));
    assert(!BlockingWifiScanPolicy::CallbackArrivedBeforeDeadline(
        wait_ms, wait_ms));
}

void RecoveryRetrySurvivesBusyAndConsumesOnlyAfterStart() {
    BlockingWifiScanRetryState retry;
    const BlockingWifiScanRetryState::Token token{Lease(40), 9};
    retry.Publish(token);
    assert(retry.Peek(9).has_value());
    // A role scanner winning the lease must leave the UI retry durable.
    retry.PublishIfAbsent({Coordinator::Lease{}, 9});
    assert(retry.Peek(9).has_value());
    assert(retry.Peek(9)->recovered_lease.lease_id == 40);
    assert(retry.ConsumeIfExact(token));
    assert(!retry.Peek(9).has_value());
    assert(!retry.ConsumeIfExact(token));
}

void RecoveryRetryCoalescesAndStaleUiGenerationCancels() {
    BlockingWifiScanRetryState retry;
    const BlockingWifiScanRetryState::Token token{Lease(41), 10};
    retry.Publish(token);
    retry.Publish(token);
    assert(retry.Peek(10).has_value());
    retry.CancelGeneration(9);
    assert(retry.Peek(10).has_value());
    retry.CancelGeneration(10);
    assert(!retry.Peek(10).has_value());
    // A recovery callback arriving after UI destruction must not resurrect it.
    retry.Publish(token);
    assert(!retry.Peek(10).has_value());
}

void AuthenticatedDriverFailureCannotPublishScanResults() {
    BlockingWifiScanCompletionStatus status;
    const auto lease = Lease(50);
    status.Observe(lease, 1);
    assert(status.IsObserved(lease));
    assert(!status.Succeeded(lease));
    assert(!status.Succeeded(Lease(51)));
}

void AuthenticatedZeroStatusAllowsResultConsumption() {
    BlockingWifiScanCompletionStatus status;
    const auto lease = Lease(52);
    status.Observe(lease, 0);
    assert(status.Succeeded(lease));
}

}  // namespace

int main() {
    EarlyCallbackWakesOnlyAfterCommit();
    BusyAndForeignCallbacksDoNotDisturbOwner();
    FailedInitializationCanAbandonOnlyTheExactUnsubmittedLease();
    MissingCallbackRetainsExactRecoveryDebt();
    CallbackBeatingTimeoutStaysWithWaiter();
    BlockingReturnBeforeEventWaitsForAuthenticatedCallback();
    CallbackAndTimeoutChooseOneCompletionOwner();
    CleanupFailureAndRecoveryRemainExact();
    RepeatedScansResetAllState();
    FullChannelActiveScanWaitIncludesEveryChannelAndSchedulingMargin();
    SlowFullChannelCallbackAtBoundaryIsNotClassifiedAsLost();
    RecoveryRetrySurvivesBusyAndConsumesOnlyAfterStart();
    RecoveryRetryCoalescesAndStaleUiGenerationCancels();
    AuthenticatedDriverFailureCannotPublishScanResults();
    AuthenticatedZeroStatusAllowsResultConsumption();
    std::cout << "blocking wifi scan lease host tests: PASS\n";
    return 0;
}
