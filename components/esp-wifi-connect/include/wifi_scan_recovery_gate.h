#pragma once

#include "wifi_scan_lease_coordinator.h"

#include <optional>

class WifiScanRecoveryGate {
public:
    struct Claim {
        WifiScanLeaseCoordinator::Lease lease;
        WifiScanLeaseCoordinator::RecoveryDecision recovery;
        uint64_t scan_session_id = 0;
        bool scans_were_enabled = false;
    };

    struct RestoreState {
        std::optional<WifiScanLeaseCoordinator::Lease> lease;
        uint64_t scan_session_id = 0;
    };

    static std::optional<Claim> TryClaim(
        WifiScanLeaseCoordinator& coordinator,
        const WifiScanLeaseCoordinator::Lease& expected,
        const std::optional<WifiScanLeaseCoordinator::Lease>& scan_lease,
        const std::optional<WifiScanLeaseCoordinator::Lease>& recovery_lease,
        uint64_t scan_session_id, bool& scans_enabled,
        RestoreState& restore_state);

    static bool MarkRestored(const Claim& claim, uint64_t current_session,
                             bool restored, RestoreState& restore_state);

    static bool HasDebt(
        const WifiScanLeaseCoordinator::Lease& expected,
        const std::optional<WifiScanLeaseCoordinator::Lease>& recovery_lease);

    static bool Complete(
        WifiScanLeaseCoordinator& coordinator, const Claim& claim,
        const WifiScanLeaseCoordinator::RecoveryProof& proof,
        std::optional<WifiScanLeaseCoordinator::Lease>& scan_lease,
        std::optional<WifiScanLeaseCoordinator::Lease>& recovery_lease,
        uint64_t current_session, bool allow_scans, bool& scans_enabled,
        RestoreState& restore_state);
};
