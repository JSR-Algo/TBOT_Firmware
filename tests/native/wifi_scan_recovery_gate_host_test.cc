#include "wifi_scan_recovery_gate.h"

#include <cassert>
#include <iostream>

class WifiScanRecoveryExecutor {
public:
    static WifiScanLeaseCoordinator::RecoveryProof Invalid() {
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }
    static WifiScanLeaseCoordinator::RecoveryProof Prove(
            const WifiScanLeaseCoordinator::RecoveryDecision& recovery) {
        return WifiScanLeaseCoordinator::RecoveryProof{
            recovery.recovery_id(), recovery.coordinator_identity_, true, true};
    }
};

int main() {
    for (const auto owner : {WifiScanLeaseCoordinator::Owner::kStation,
                             WifiScanLeaseCoordinator::Owner::kConfigAp}) {
        WifiScanLeaseCoordinator coordinator;
        auto acquired = coordinator.TryAcquire(owner);
        assert(acquired.acquired);
        assert(coordinator.CommitSubmission(acquired.lease, false).drain_required);
        std::optional<WifiScanLeaseCoordinator::Lease> scan = acquired.lease;
        std::optional<WifiScanLeaseCoordinator::Lease> debt = acquired.lease;
        bool scans_enabled = true;
        WifiScanRecoveryGate::RestoreState restore_state;
        auto claim = WifiScanRecoveryGate::TryClaim(
            coordinator, acquired.lease, scan, debt, 7, scans_enabled,
            restore_state);
        assert(claim.has_value());
        assert(!scans_enabled);
        assert(!coordinator.ObserveScanDone(acquired.lease).consume_now);

        const auto invalid = WifiScanRecoveryExecutor::Invalid();
        assert(!WifiScanRecoveryGate::Complete(
            coordinator, *claim, invalid, scan, debt, 7, true,
            scans_enabled, restore_state));
        assert(!scans_enabled && scan.has_value() && debt.has_value());

        const auto proof = WifiScanRecoveryExecutor::Prove(claim->recovery);
        assert(!WifiScanRecoveryGate::Complete(
            coordinator, *claim, proof, scan, debt, 7, true,
            scans_enabled, restore_state));
        assert(!scans_enabled && scan.has_value() && debt.has_value());
        assert(WifiScanRecoveryGate::MarkRestored(
            *claim, 7, true, restore_state));
        assert(WifiScanRecoveryGate::Complete(
            coordinator, *claim, proof, scan, debt, 7, true,
            scans_enabled, restore_state));
        assert(scans_enabled && !scan.has_value() && !debt.has_value());

        auto next = coordinator.TryAcquire(owner);
        assert(next.acquired);
        assert(coordinator.CommitSubmission(next.lease, false).drain_required);
        scan = next.lease;
        debt = next.lease;
        scans_enabled = false;
        auto inactive_claim = WifiScanRecoveryGate::TryClaim(
            coordinator, next.lease, scan, debt, 8, scans_enabled,
            restore_state);
        assert(inactive_claim.has_value());
        assert(!inactive_claim->scans_were_enabled);
        assert(WifiScanRecoveryGate::MarkRestored(
            *inactive_claim, 8, true, restore_state));
        const auto inactive_proof =
            WifiScanRecoveryExecutor::Prove(inactive_claim->recovery);
        assert(WifiScanRecoveryGate::Complete(
            coordinator, *inactive_claim, inactive_proof, scan, debt, 8,
            true, scans_enabled, restore_state));
        assert(!scans_enabled);
    }
    std::cout << "wifi scan recovery gate host tests passed\n";
}
