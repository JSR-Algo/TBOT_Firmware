#include "wifi_scan_recovery_gate.h"

namespace {

bool SameLease(const WifiScanLeaseCoordinator::Lease& left,
               const WifiScanLeaseCoordinator::Lease& right) {
    return left.owner == right.owner && left.lease_id == right.lease_id &&
           left.driver_incarnation == right.driver_incarnation;
}

}  // namespace

std::optional<WifiScanRecoveryGate::Claim> WifiScanRecoveryGate::TryClaim(
        WifiScanLeaseCoordinator& coordinator,
        const WifiScanLeaseCoordinator::Lease& expected,
        const std::optional<WifiScanLeaseCoordinator::Lease>& scan_lease,
        const std::optional<WifiScanLeaseCoordinator::Lease>& recovery_lease,
        uint64_t scan_session_id, bool& scans_enabled,
        RestoreState& restore_state) {
    if (!scan_lease.has_value() || !recovery_lease.has_value() ||
        !SameLease(*scan_lease, *recovery_lease) ||
        !SameLease(*recovery_lease, expected)) {
        return std::nullopt;
    }
    const auto recovery = coordinator.BeginRecovery(*recovery_lease);
    if (!recovery.begun()) {
        return std::nullopt;
    }
    const bool scans_were_enabled = scans_enabled;
    scans_enabled = false;
    restore_state = RestoreState{};
    return Claim{*recovery_lease, recovery, scan_session_id,
                 scans_were_enabled};
}

bool WifiScanRecoveryGate::MarkRestored(
        const Claim& claim, uint64_t current_session, bool restored,
        RestoreState& restore_state) {
    if (!restored) {
        restore_state = RestoreState{};
        return false;
    }
    restore_state.lease = claim.lease;
    restore_state.scan_session_id = current_session;
    return true;
}

bool WifiScanRecoveryGate::HasDebt(
        const WifiScanLeaseCoordinator::Lease& expected,
        const std::optional<WifiScanLeaseCoordinator::Lease>& recovery_lease) {
    return recovery_lease.has_value() && SameLease(*recovery_lease, expected);
}

bool WifiScanRecoveryGate::Complete(
        WifiScanLeaseCoordinator& coordinator, const Claim& claim,
        const WifiScanLeaseCoordinator::RecoveryProof& proof,
        std::optional<WifiScanLeaseCoordinator::Lease>& scan_lease,
        std::optional<WifiScanLeaseCoordinator::Lease>& recovery_lease,
        uint64_t current_session, bool allow_scans, bool& scans_enabled,
        RestoreState& restore_state) {
    if (!scan_lease.has_value() || !recovery_lease.has_value() ||
        !restore_state.lease.has_value() ||
        !SameLease(*scan_lease, claim.lease) ||
        !SameLease(*recovery_lease, claim.lease) ||
        !SameLease(*restore_state.lease, claim.lease) ||
        restore_state.scan_session_id != current_session ||
        !coordinator.CompleteRecovery(claim.lease, proof)) {
        return false;
    }
    scan_lease.reset();
    recovery_lease.reset();
    restore_state = RestoreState{};
    if (claim.scans_were_enabled &&
        claim.scan_session_id == current_session && allow_scans) {
        scans_enabled = true;
    }
    return true;
}
