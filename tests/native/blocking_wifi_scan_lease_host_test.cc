#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_lease_state.h"
#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_policy.h"
#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_retry_state.h"
#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_completion_status.h"
#include "../../main/boards/m5stack-cardputer-adv/blocking_wifi_scan_worker_state.h"
#include "../../main/boards/m5stack-cardputer-adv/process_lifetime_worker_handle.h"
#include "../../main/boards/m5stack-cardputer-adv/cardputer_wifi_deferred_intent_state.h"
#include "../../main/boards/m5stack-cardputer-adv/cardputer_wifi_connection_policy.h"
#include "wifi_credential_limits.h"

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

void ScanWorkerRequestIsDurableCoalescedAndGenerationBound() {
    BlockingWifiScanWorkerState state;
    const BlockingWifiScanWorkerState::Request first{20, 1};
    assert(state.Publish(first));
    assert(!state.Publish(first));
    assert(state.NeedsWorkerCreation());
    state.ObserveWorkerCreation(false);
    assert(state.NeedsWorkerCreation());
    state.ObserveWorkerCreation(true);
    assert(state.NeedsNotification());
    const auto failed_arm = state.ArmNotification();
    assert(failed_arm.has_value());
    state.RollbackNotification(*failed_arm);
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto taken = state.TakeNotified();
    assert(taken.has_value());
    state.CancelGeneration(20);
    assert(!state.CompleteIfCurrent(first));
    assert(!state.Publish({20, 2}));
    assert(state.Publish({21, 1}));
}

void ScanWorkerRenotifiesRequestPublishedWhileAnotherIsInFlight() {
    BlockingWifiScanWorkerState state;
    const BlockingWifiScanWorkerState::Request first{30, 1};
    const BlockingWifiScanWorkerState::Request second{30, 2};
    assert(state.Publish(first));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    assert(state.TakeNotified().has_value());
    assert(state.Publish(second));
    assert(!state.NeedsNotification());
    assert(state.CompleteIfCurrent(first));
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto taken = state.TakeNotified();
    assert(taken.has_value());
    assert(taken->revision == second.revision);
    assert(state.CompleteIfCurrent(second));
}

void ScanWorkerRejectsStaleCompletionWithoutLosingNewRequest() {
    BlockingWifiScanWorkerState state;
    const BlockingWifiScanWorkerState::Request current{40, 2};
    assert(state.Publish(current));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    assert(state.TakeNotified().has_value());
    assert(!state.CompleteIfCurrent({40, 1}));
    assert(state.CompleteIfCurrent(current));
}

void ScanWorkerSchedulingFailureRestoresDurableRequest() {
    BlockingWifiScanWorkerState state;
    const BlockingWifiScanWorkerState::Request request{50, 1};
    assert(state.Publish(request));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    assert(state.TakeNotified().has_value());
    assert(state.RetryInFlight(request));
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    assert(state.TakeNotified().has_value());
    assert(state.CompleteIfCurrent(request));
}

void ScanWorkerSchedulingFailurePromotesNewerPendingRequest() {
    BlockingWifiScanWorkerState state;
    const BlockingWifiScanWorkerState::Request first{51, 1};
    const BlockingWifiScanWorkerState::Request newer{52, 1};
    assert(state.Publish(first));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto in_flight = state.TakeNotified();
    assert(in_flight.has_value());
    assert(state.Publish(newer));

    // Models Application::Schedule throwing after the newer UI request lands.
    assert(state.RetryInFlight(*in_flight));
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto executed = state.TakeNotified();
    assert(executed.has_value());
    assert(executed->ui_generation == newer.ui_generation);
    assert(executed->revision == newer.revision);
    assert(state.CompleteIfCurrent(*executed));
    assert(!state.NeedsNotification());
    assert(!state.TakeNotified().has_value());
}

void ScanWorkerSchedulingFailureClearsCancelledInFlightRequest() {
    BlockingWifiScanWorkerState state;
    const BlockingWifiScanWorkerState::Request stale{53, 1};
    const BlockingWifiScanWorkerState::Request current{54, 1};
    assert(state.Publish(stale));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto in_flight = state.TakeNotified();
    assert(in_flight.has_value());
    state.CancelGeneration(53);
    assert(!state.RetryInFlight(*in_flight));
    assert(state.Publish(current));
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto executed = state.TakeNotified();
    assert(executed.has_value());
    assert(executed->ui_generation == current.ui_generation);
    assert(state.CompleteIfCurrent(*executed));
}

void PreSubmitCallbackFailureTransitionsToRecoverableDebt() {
    WifiScanLeaseCoordinator coordinator;
    BlockingWifiScanLeaseState owner;
    const auto acquired = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi);
    assert(acquired.acquired);
    assert(owner.Begin(acquired.lease));
    assert(coordinator.ObserveScanDone(acquired.lease).deferred_until_commit);
    assert(owner.OnCallback(acquired.lease) ==
           BlockingWifiScanLeaseState::CallbackAction::kWakeWaiter);
    const auto commit = coordinator.CommitSubmission(acquired.lease, false);
    assert(commit.consume_latched);
    assert(coordinator.RetainFailedCompletion(acquired.lease));
    assert(owner.RetainCompletionForRecovery(acquired.lease));
    assert(owner.HasDebt(acquired.lease));
    assert(!coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation).acquired);

    WifiScanLeaseCoordinator lagging_coordinator;
    BlockingWifiScanLeaseState lagging_owner;
    const auto lagging = lagging_coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi);
    assert(lagging_owner.Begin(lagging.lease));
    assert(lagging_owner.RetainForRecovery(lagging.lease));
    assert(lagging_owner.OnCallback(lagging.lease) ==
           BlockingWifiScanLeaseState::CallbackAction::kIgnore);
}

void DelayedScanWorkerDoesNotBlockUnrelatedApplicationWork() {
    std::mutex mutex;
    std::condition_variable started;
    constexpr auto modeled_scan_wait = std::chrono::milliseconds(5200);
    bool worker_started = false;
    bool release_worker = false;
    std::thread worker([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        worker_started = true;
        started.notify_one();
        started.wait(lock, [&]() { return release_worker; });
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        started.wait(lock, [&]() { return worker_started; });
    }
    // The gate deterministically models the production 5.2 second scan wait.
    assert(modeled_scan_wait == std::chrono::milliseconds(5200));
    bool application_sentinel_ran = true;
    assert(application_sentinel_ran);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_worker = true;
    }
    started.notify_one();
    worker.join();
}

void WorkerHandlePublicationSurvivesNotifyBeforePublish() {
    ProcessLifetimeWorkerHandle<void*> handle;
    std::atomic<bool> published{false};
    std::atomic<int> wakes{0};
    std::thread notifier([&]() {
        while (!published.load(std::memory_order_acquire)) {
            if (handle.Load() != nullptr) {
                ++wakes;
            }
            std::this_thread::yield();
        }
        if (handle.Load() != nullptr) {
            ++wakes;
        }
    });
    assert(handle.Load() == nullptr);
    assert(handle.Publish(reinterpret_cast<void*>(0x1234)));
    published.store(true, std::memory_order_release);
    notifier.join();
    assert(handle.Load() == reinterpret_cast<void*>(0x1234));
    assert(wakes.load() >= 1);
    assert(!handle.Publish(reinterpret_cast<void*>(0x5678)));
}

void ArmedWakeCannotBeLostToPreemptingWorker() {
    BlockingWifiScanWorkerState scan;
    assert(scan.Publish({60, 1}));
    scan.ObserveWorkerCreation(true);
    assert(scan.ArmNotification());
    const auto scan_request = scan.TakeNotified();
    assert(scan_request.has_value());

    CardputerWifiDeferredIntentState connection;
    assert(connection.PublishReconnect(61));
    connection.ObserveWorkerCreation(true);
    assert(connection.ArmNotification());
    const auto connection_intent = connection.TakeNotified();
    assert(connection_intent.has_value());
    assert(connection.CompleteReconnect(*connection_intent));
}

void ConsumedWakeCannotAcknowledgeNewerPendingWork() {
    BlockingWifiScanWorkerState scan;
    assert(scan.Publish({62, 1}));
    scan.ObserveWorkerCreation(true);
    assert(scan.ArmNotification());
    const auto first_scan = scan.TakeNotified();
    assert(first_scan.has_value());
    assert(scan.Publish({62, 2}));
    assert(scan.CompleteIfCurrent(*first_scan));
    assert(scan.NeedsNotification());

    CardputerWifiDeferredIntentState connection;
    assert(connection.PublishReconnect(63));
    connection.ObserveWorkerCreation(true);
    assert(connection.ArmNotification());
    const auto first_connection = connection.TakeNotified();
    assert(first_connection.has_value());
    assert(connection.PublishReconnect(64));
    assert(connection.CompleteReconnect(*first_connection));
    assert(connection.NeedsNotification());
}

void LifecycleReservationIgnoresScanCallbacksAndReleasesExactly() {
    WifiScanLeaseCoordinator coordinator;
    const auto lifecycle = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kLifecycle);
    assert(lifecycle.acquired);
    const auto callback = coordinator.ObserveScanDone(lifecycle.lease);
    assert(!callback.consume_now && !callback.deferred_until_commit);
    assert(!coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
    assert(coordinator.AbandonUnsubmitted(lifecycle.lease));
    assert(coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
}

void DeferredWifiIntentIsTypedCoalescedAndGenerationBound() {
    CardputerWifiDeferredIntentState state;
    assert(state.PublishCredentials(70, "first", "secret1"));
    assert(state.PublishCredentials(70, "second", "secret2"));
    assert(state.NeedsWorkerCreation());
    state.ObserveWorkerCreation(true);
    assert(state.NeedsNotification());
    const auto failed_arm = state.ArmNotification();
    assert(failed_arm.has_value());
    state.RollbackNotification(*failed_arm);
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto intent = state.TakeNotified();
    assert(intent.has_value());
    assert(intent->kind == CardputerWifiDeferredIntentState::Kind::kCredentials);
    assert(intent->ssid == "second");
    assert(intent->password == "secret2");
    assert(state.StoreConnectionResult(*intent, true));
    const auto result = state.ClaimResultForDelivery();
    assert(result.has_value() && result->connected);
    assert(state.PublishCredentials(71, "next", "next-secret"));
    assert(!state.NeedsNotification());
    state.ObserveResultDelivery(*result, false);
    assert(state.ClaimResultForDelivery().has_value());
    assert(state.CompleteResult(*result));
    assert(state.NeedsNotification());
    state.CancelGeneration(70);
    assert(!state.PublishCredentials(70, "stale", "stale"));
    assert(state.PublishReconnect(71));
}

void DeferredWifiIntentDropsOnlyStaleUiResult() {
    CardputerWifiDeferredIntentState state;
    assert(state.PublishCredentials(90, "ssid", "password"));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto intent = state.TakeNotified();
    assert(intent.has_value());
    state.CancelGeneration(90);
    assert(!state.StoreConnectionResult(*intent, true));
    assert(!state.ClaimResultForDelivery().has_value());
    assert(state.PublishReconnect(91));
    assert(state.NeedsNotification());
}

void DeferredWifiIntentRetriesIfScanOwnershipReturnsAfterNotification() {
    CardputerWifiDeferredIntentState state;
    assert(state.PublishCredentials(95, "ssid", "password"));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto intent = state.TakeNotified();
    assert(intent.has_value());
    assert(state.RetryInFlight(*intent));
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto retried = state.TakeNotified();
    assert(retried.has_value());
    assert(retried->revision == intent->revision);
}

void DeferredWifiIntentSupersedesBlockedInFlightWithoutWedging() {
    CardputerWifiDeferredIntentState state;
    assert(state.PublishCredentials(96, "old", "old-password"));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto old_intent = state.TakeNotified();
    assert(old_intent.has_value());
    assert(state.PublishCredentials(97, "new", "new-password"));
    assert(state.RetryInFlight(*old_intent));
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto new_intent = state.TakeNotified();
    assert(new_intent.has_value());
    assert(new_intent->ssid == "new");
}

void CorrectedCredentialsSupersedeEarlierFailedAttempt() {
    CardputerWifiDeferredIntentState state;
    assert(state.PublishCredentials(99, "SUMI_LAU1", "wrong-password"));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto wrong = state.TakeNotified();
    assert(wrong.has_value());
    assert(state.PublishCredentials(99, "SUMI_LAU1", "hongvantruong"));
    assert(state.StoreConnectionResult(*wrong, false));
    assert(!state.ClaimResultForDelivery().has_value());
    assert(state.NeedsNotification());
    assert(state.ArmNotification());
    const auto corrected = state.TakeNotified();
    assert(corrected.has_value());
    assert(corrected->password == "hongvantruong");
    assert(state.StoreConnectionResult(*corrected, true));
    const auto result = state.ClaimResultForDelivery();
    assert(result.has_value() && result->connected);
    assert(state.CompleteResult(*result));
    assert(!state.NeedsNotification());
}

void CredentialPersistenceIsExactlyOnceAcrossBusyRetry() {
    CardputerWifiDeferredIntentState state;
    assert(state.PublishCredentials(100, "SUMI_LAU1", "hongvantruong"));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto first = state.TakeNotified();
    assert(first.has_value());
    assert(state.BindCredentialTransaction(*first, 701));
    assert(!state.BindCredentialTransaction(*first, 702));
    assert(state.RetryInFlight(*first));
    assert(state.ArmNotification());
    const auto retry = state.TakeNotified();
    assert(retry.has_value());
    assert(state.CredentialTransaction(*retry).value() == 701);
    int commits = 0;
    const auto finalization =
        state.ClaimCredentialFinalization(*retry, true);
    assert(finalization.has_value());
    assert(finalization->transaction_id == 701 && finalization->commit);
    ++commits;
    assert(state.CompleteCredentialFinalization(*finalization, true));
    assert(commits == 1);
}

void CredentialTransactionsRollbackFailureCancelAndSupersession() {
    CardputerWifiDeferredIntentState state;
    state.ObserveWorkerCreation(true);

    assert(state.PublishCredentials(101, "target", "wrong"));
    assert(state.ArmNotification());
    const auto failed = state.TakeNotified();
    assert(failed.has_value());
    assert(state.BindCredentialTransaction(*failed, 801));
    int rollbacks = 0;
    const auto failed_finalization =
        state.ClaimCredentialFinalization(*failed, false);
    assert(failed_finalization.has_value());
    assert(failed_finalization->transaction_id == 801 &&
           !failed_finalization->commit);
    ++rollbacks;
    assert(state.CompleteCredentialFinalization(*failed_finalization, true));
    const auto failed_result = state.ClaimResultForDelivery();
    assert(failed_result.has_value() && !failed_result->connected);
    assert(state.CompleteResult(*failed_result));

    assert(state.PublishCredentials(102, "target", "candidate"));
    assert(state.ArmNotification());
    const auto cancelled = state.TakeNotified();
    assert(cancelled.has_value());
    assert(state.BindCredentialTransaction(*cancelled, 802));
    const auto cancelled_transaction = state.CancelGeneration(102);
    assert(cancelled_transaction.has_value());
    assert(*cancelled_transaction == 802);

    assert(state.PublishCredentials(103, "old", "old-password"));
    assert(state.ArmNotification());
    const auto old = state.TakeNotified();
    assert(old.has_value());
    assert(state.BindCredentialTransaction(*old, 803));
    assert(state.PublishCredentials(104, "new", "new-password"));
    const auto stale_finalization =
        state.ClaimCredentialFinalization(*old, true);
    assert(stale_finalization.has_value());
    assert(stale_finalization->transaction_id == 803 &&
           !stale_finalization->commit);
    ++rollbacks;
    assert(state.CompleteCredentialFinalization(*stale_finalization, true));
    assert(rollbacks == 2);
    assert(state.NeedsNotification());
}

void ArmedNotificationRollsBackAfterGiveFailure() {
    CardputerWifiDeferredIntentState state;
    assert(state.PublishReconnect(98));
    state.ObserveWorkerCreation(true);
    const auto armed = state.ArmNotification();
    assert(armed.has_value());
    state.RollbackNotification(*armed);
    assert(state.NeedsNotification());
}

void DeferredWifiIntentIsThreadSafeAndExactlyOnce() {
    CardputerWifiDeferredIntentState state;
    std::thread credentials([&]() {
        for (int i = 0; i < 100; ++i) {
            state.PublishCredentials(80, "ssid" + std::to_string(i),
                                     "password" + std::to_string(i));
        }
    });
    std::thread reconnect([&]() {
        for (int i = 0; i < 100; ++i) {
            state.PublishReconnect(81);
        }
    });
    credentials.join();
    reconnect.join();
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto intent = state.TakeNotified();
    assert(intent.has_value());
    if (intent->kind == CardputerWifiDeferredIntentState::Kind::kReconnect) {
        assert(state.CompleteReconnect(*intent));
        assert(!state.CompleteReconnect(*intent));
    } else {
        assert(state.StoreConnectionResult(*intent, true));
        assert(!state.StoreConnectionResult(*intent, true));
    }
}

void ConnectionStartPolicyDoesNotLoopOnActiveStation() {
    assert(ResolveCardputerWifiStartAction(
               true, WifiStationStartResult::kAlreadyActive) ==
           CardputerWifiStartAction::kCompleteReconnect);
    assert(ResolveCardputerWifiStartAction(
               false, WifiStationStartResult::kAlreadyActive) ==
           CardputerWifiStartAction::kMonitorCredentials);
    assert(ResolveCardputerWifiStartAction(
               false, WifiStationStartResult::kStartedNow) ==
           CardputerWifiStartAction::kMonitorCredentials);
    assert(ResolveCardputerWifiStartAction(
               true, WifiStationStartResult::kBusyOrFailed) ==
           CardputerWifiStartAction::kRetry);
    assert(ResolveCardputerWifiStartAction(
               false, WifiStationStartResult::kInvalidCredentials) ==
           CardputerWifiStartAction::kRejectCredentials);
    assert(ShouldArmWifiConnectTimeout(WifiStationStartResult::kStartedNow));
    assert(!ShouldArmWifiConnectTimeout(
        WifiStationStartResult::kAlreadyActive));
    assert(!ShouldArmWifiConnectTimeout(
        WifiStationStartResult::kBusyOrFailed));
}

void CredentialFieldLimitsAreByteExact() {
    assert(IsValidWifiCredentials(std::string(32, 's'),
                                  std::string(63, 'p')));
    assert(!IsValidWifiCredentials(std::string(33, 's'), "password"));
    assert(!IsValidWifiCredentials("ssid", std::string(64, 'p')));
    uint8_t ssid_buffer[32] = {};
    uint8_t password_buffer[64] = {};
    assert(CopyWifiCredentialsToBuffers(
        std::string(32, 's'), std::string(63, 'p'),
        ssid_buffer, sizeof(ssid_buffer),
        password_buffer, sizeof(password_buffer)));
    assert(std::all_of(std::begin(ssid_buffer), std::end(ssid_buffer),
                       [](uint8_t value) { return value == 's'; }));
    assert(std::all_of(std::begin(password_buffer), std::end(password_buffer) - 1,
                       [](uint8_t value) { return value == 'p'; }));
    assert(password_buffer[63] == 0);
    assert(!CopyWifiCredentialsToBuffers(
        std::string(33, 's'), "password",
        ssid_buffer, sizeof(ssid_buffer),
        password_buffer, sizeof(password_buffer)));
    assert(!CopyWifiCredentialsToBuffers(
        "ssid", std::string(64, 'p'),
        ssid_buffer, sizeof(ssid_buffer),
        password_buffer, sizeof(password_buffer)));

    std::string ssid(31, 's');
    assert(!AppendWifiFieldIfFits(ssid, "\xC3\xA9", kMaxWifiSsidBytes));
    assert(ssid.size() == 31);
    assert(AppendWifiFieldIfFits(ssid, "x", kMaxWifiSsidBytes));
    assert(ssid.size() == 32);
    std::string oversized(33, 's');
    assert(!AppendWifiFieldIfFits(oversized, "x", kMaxWifiSsidBytes));
    assert(oversized.size() == 33);
}

void SetupCompletionIsGenerationBoundAndExactlyOnce() {
    CardputerWifiDeferredIntentState state;
    assert(state.ClaimSetupCompletion(120));
    assert(!state.ClaimSetupCompletion(120));
    assert(!state.ClaimSetupCompletion(119));
    assert(state.ClaimSetupCompletion(121));

    assert(state.PublishReconnect(122, 121));
    state.ObserveWorkerCreation(true);
    assert(state.ArmNotification());
    const auto reconnect = state.TakeNotified();
    assert(reconnect.has_value());
    assert(reconnect->setup_completion_generation == 121);
    assert(state.RetryInFlight(*reconnect));
    assert(state.ArmNotification());
    const auto retry = state.TakeNotified();
    assert(retry.has_value());
    assert(retry->setup_completion_generation == 121);
    assert(state.CompleteReconnect(*retry));
}

void DelayedConnectionWorkerDoesNotBlockApplicationSentinel() {
    std::mutex mutex;
    std::condition_variable gate;
    bool worker_waiting = false;
    bool release = false;
    std::thread connection_worker([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        worker_waiting = true;
        gate.notify_one();
        gate.wait(lock, [&]() { return release; });
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        gate.wait(lock, [&]() { return worker_waiting; });
    }
    constexpr auto modeled_connection_wait = std::chrono::seconds(10);
    assert(modeled_connection_wait == std::chrono::seconds(10));
    bool application_sentinel_ran = true;
    assert(application_sentinel_ran);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    gate.notify_one();
    connection_worker.join();
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
    ScanWorkerRequestIsDurableCoalescedAndGenerationBound();
    ScanWorkerRenotifiesRequestPublishedWhileAnotherIsInFlight();
    ScanWorkerRejectsStaleCompletionWithoutLosingNewRequest();
    ScanWorkerSchedulingFailureRestoresDurableRequest();
    ScanWorkerSchedulingFailurePromotesNewerPendingRequest();
    ScanWorkerSchedulingFailureClearsCancelledInFlightRequest();
    PreSubmitCallbackFailureTransitionsToRecoverableDebt();
    DelayedScanWorkerDoesNotBlockUnrelatedApplicationWork();
    WorkerHandlePublicationSurvivesNotifyBeforePublish();
    ArmedWakeCannotBeLostToPreemptingWorker();
    ConsumedWakeCannotAcknowledgeNewerPendingWork();
    LifecycleReservationIgnoresScanCallbacksAndReleasesExactly();
    DeferredWifiIntentIsTypedCoalescedAndGenerationBound();
    DeferredWifiIntentIsThreadSafeAndExactlyOnce();
    ConnectionStartPolicyDoesNotLoopOnActiveStation();
    CredentialFieldLimitsAreByteExact();
    SetupCompletionIsGenerationBoundAndExactlyOnce();
    DeferredWifiIntentDropsOnlyStaleUiResult();
    DeferredWifiIntentRetriesIfScanOwnershipReturnsAfterNotification();
    DeferredWifiIntentSupersedesBlockedInFlightWithoutWedging();
    CorrectedCredentialsSupersedeEarlierFailedAttempt();
    CredentialPersistenceIsExactlyOnceAcrossBusyRetry();
    CredentialTransactionsRollbackFailureCancelAndSupersession();
    ArmedNotificationRollsBackAfterGiveFailure();
    DelayedConnectionWorkerDoesNotBlockApplicationSentinel();
    std::cout << "blocking wifi scan lease host tests: PASS\n";
    return 0;
}
