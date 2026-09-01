#include "../../main/boards/common/blufi_wifi_scan_retry_state.h"
#include "../../main/boards/common/blufi_wifi_scan_controller.h"
#include "../../components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"

#include <cassert>
#include <stdexcept>

class WifiScanRecoveryExecutor {
public:
    static WifiScanLeaseCoordinator::RecoveryProof Prove(
            const WifiScanLeaseCoordinator::RecoveryDecision& recovery) {
        return WifiScanLeaseCoordinator::RecoveryProof{
            recovery.recovery_id(), recovery.coordinator_identity_, true, true};
    }
};

namespace {

using State = BlufiWifiScanRetryState;

State::ExactRequest Exact(uint64_t id, uint32_t generation, uint64_t session,
                          uint64_t connection) {
    return State::ExactRequest{id, generation, session, connection, true, true};
}

void AllImmediateSignalsCanFailWithoutLosingExactRequest() {
    State state;
    const auto exact = Exact(7, 1, 11, 101);
    state.Publish(exact);
    const auto signals = state.SignalPublished(
        []() { return false; },  // esp_timer arm failure
        []() { return false; },  // FreeRTOS command queue failure
        []() -> bool { throw std::bad_alloc(); },
        []() { return false; });  // manager notify failure
    assert(!signals.esp_timer && !signals.freertos_timer &&
           !signals.application && !signals.manager);
    assert(state.Snapshot().has_value());
    assert(state.Snapshot()->request_id == 7);
}

void RepeatedGetCoalescesAndLeaseReleaseStartsExactlyOnce() {
    State state;
    const auto exact = Exact(7, 1, 11, 101);
    for (int i = 0; i < 5; ++i) {
        state.Publish(exact);
    }
    int starts = 0;
    bool lease_busy = true;
    auto poll = [&]() {
        const auto pending = state.Snapshot();
        if (!pending.has_value() || lease_busy) {
            return;
        }
        if (state.ClearIfExact(*pending)) {
            ++starts;
        }
    };
    poll();
    assert(starts == 0);
    lease_busy = false;
    poll();
    poll();
    assert(starts == 1);
}

void LifecycleReplacementCancelsStaleRetry() {
    State state;
    const auto stale = Exact(7, 1, 11, 101);
    const auto current = Exact(8, 2, 22, 202);
    state.Publish(stale);
    const auto stale_snapshot = *state.Snapshot();
    state.Publish(current);
    assert(!state.ClearIfExact(stale_snapshot));
    assert(state.Snapshot()->request_id == current.request_id);
    assert(state.ClearIfExact(*state.Snapshot()));
    assert(!state.Snapshot().has_value());
}

void StaleRetryCannotOverwriteLifecycleReplacement() {
    State state;
    const auto stale = Exact(7, 1, 11, 101);
    const auto current = Exact(8, 2, 22, 202);
    state.Publish(stale);
    const auto stale_snapshot = *state.Snapshot();
    state.Publish(current);
    assert(!state.RepublishIfUnchanged(stale_snapshot));
    const auto pending = state.Snapshot();
    assert(pending.has_value());
    assert(pending->request_id == current.request_id);
    assert(pending->setup_generation == current.setup_generation);
}

void ProductionControllerAndLeaseReleaseStartExactlyOnce() {
    BlufiWifiScanController logical;
    WifiScanLeaseCoordinator physical;
    const BlufiWifiScanController::Request request{
        1, 11, 101, true, true};
    const auto requested = logical.RequestScan(request);
    assert(requested.start_now);
    State state;
    state.Publish(Exact(requested.request_id, 1, 11, 101));

    const auto blocker = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation);
    assert(blocker.acquired);
    int starts = 0;
    auto poll = [&]() {
        const auto exact = state.Snapshot();
        if (!exact.has_value() ||
            !logical.UnclaimedRequestIfCurrent(
                exact->request_id, exact->setup_generation,
                exact->ble_session_state,
                exact->ble_connection_epoch).has_value()) {
            return;
        }
        const auto acquired = physical.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kBlufi);
        if (!acquired.acquired) {
            return;
        }
        if (!state.ClearIfExact(*exact)) {
            assert(physical.AbandonUnsubmitted(acquired.lease));
            return;
        }
        assert(logical.SynchronizeDriverIncarnation(
            exact->request_id, acquired.lease.driver_incarnation));
        assert(logical.ClaimStart(exact->request_id).claimed);
        ++starts;
        assert(physical.AbandonUnsubmitted(acquired.lease));
    };
    poll();
    assert(starts == 0);
    assert(physical.AbandonUnsubmitted(blocker.lease));
    poll();
    poll();
    assert(starts == 1);
}

void ProductionLifecycleReplacementDropsStaleTuple() {
    BlufiWifiScanController logical;
    const auto requested = logical.RequestScan({1, 11, 101, true, true});
    State state;
    state.Publish(Exact(requested.request_id, 1, 11, 101));
    logical.InvalidateSession(2, 22, 202);
    const auto stale = *state.Snapshot();
    assert(!logical.UnclaimedRequestIfCurrent(
        stale.request_id, stale.setup_generation, stale.ble_session_state,
        stale.ble_connection_epoch).has_value());
    assert(state.ClearIfExact(stale));
}

void ExceptionAfterClaimRollsBackAndRetriesExactlyOnce() {
    BlufiWifiScanController logical;
    WifiScanLeaseCoordinator physical;
    const auto requested = logical.RequestScan({1, 11, 101, true, true});
    State state;
    state.Publish(Exact(requested.request_id, 1, 11, 101));
    const auto exact = *state.Snapshot();
    const auto acquired = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(acquired.acquired);
    assert(state.ClearIfExact(exact));
    assert(logical.SynchronizeDriverIncarnation(
        exact.request_id, acquired.lease.driver_incarnation));
    assert(logical.ClaimStart(exact.request_id).claimed);

    // Model an exception before esp_wifi_scan_start owns the lease.
    assert(physical.AbandonUnsubmitted(acquired.lease));
    assert(!logical.ReleaseStartClaimForRetry(exact.request_id).start_pending);
    assert(state.RepublishIfUnchanged(exact));

    int starts = 0;
    const auto retry = *state.Snapshot();
    const auto retry_lease = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(retry_lease.acquired);
    assert(state.ClearIfExact(retry));
    assert(logical.SynchronizeDriverIncarnation(
        retry.request_id, retry_lease.lease.driver_incarnation));
    assert(logical.ClaimStart(retry.request_id).claimed);
    ++starts;
    assert(physical.AbandonUnsubmitted(retry_lease.lease));
    assert(starts == 1);
    assert(!state.Snapshot().has_value());
}

void ExceptionAfterSubmissionRoutesThroughRecovery() {
    BlufiWifiScanController logical;
    WifiScanLeaseCoordinator physical;
    const auto requested = logical.RequestScan({1, 11, 101, true, true});
    const auto acquired = physical.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(acquired.acquired);
    assert(logical.SynchronizeDriverIncarnation(
        requested.request_id, acquired.lease.driver_incarnation));
    assert(logical.ClaimStart(requested.request_id).claimed);
    assert(physical.CommitSubmission(acquired.lease, true).accepted);
    assert(logical.CommitStart(requested.request_id, true).accepted);

    // Model an exception after the driver accepted the scan.
    assert(!physical.AbandonUnsubmitted(acquired.lease));
    assert(physical.BeginDrain(acquired.lease));
    const auto logical_ticket = logical.BeginRecovery(requested.request_id);
    const auto physical_recovery = physical.BeginRecovery(acquired.lease);
    assert(logical_ticket.valid);
    assert(physical_recovery.begun());
    const auto proof = WifiScanRecoveryExecutor::Prove(physical_recovery);
    assert(physical.CompleteRecovery(acquired.lease, proof));
    const auto finish = logical.CompleteRecovery(logical_ticket, true);
    assert(finish.start_pending);
}

}  // namespace

int main() {
    AllImmediateSignalsCanFailWithoutLosingExactRequest();
    RepeatedGetCoalescesAndLeaseReleaseStartsExactlyOnce();
    LifecycleReplacementCancelsStaleRetry();
    StaleRetryCannotOverwriteLifecycleReplacement();
    ProductionControllerAndLeaseReleaseStartExactlyOnce();
    ProductionLifecycleReplacementDropsStaleTuple();
    ExceptionAfterClaimRollsBackAndRetriesExactlyOnce();
    ExceptionAfterSubmissionRoutesThroughRecovery();
}
