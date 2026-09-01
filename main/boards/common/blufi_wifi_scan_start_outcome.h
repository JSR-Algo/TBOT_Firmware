#pragma once

#include "blufi_wifi_scan_controller.h"
#include "wifi_scan_lease_coordinator.h"

struct BlufiWifiScanStartOutcome {
    enum class LeaseDisposition : uint8_t {
        kUnsubmitted,
        kSubmitted,
        kReleased,
    };

    LeaseDisposition disposition = LeaseDisposition::kUnsubmitted;
    WifiScanLeaseCoordinator::CommitDecision physical;
    BlufiWifiScanController::StartDecision logical;
    BlufiWifiScanController::FinishDecision rollback;
    bool driver_attempted = false;
    bool driver_accepted = false;
    bool physical_committed = false;
    bool logical_committed = false;
    bool abandoned = false;
    bool rollback_intent_set = false;
    bool rollback_for_retry = false;
    bool retry_unsubmitted = false;

    bool OwnsLease() const noexcept {
        return disposition != LeaseDisposition::kReleased;
    }

    bool ShouldRecover() const noexcept {
        return disposition == LeaseDisposition::kSubmitted &&
            physical.drain_required;
    }

    bool ShouldRetryUnsubmitted() const noexcept {
        return retry_unsubmitted;
    }

    void PreserveDispositionAfterFailure() noexcept {
        if (abandoned) {
            disposition = LeaseDisposition::kReleased;
        } else if (driver_attempted || physical_committed) {
            disposition = LeaseDisposition::kSubmitted;
        } else {
            disposition = LeaseDisposition::kUnsubmitted;
        }
    }
};

template <typename AbandonPhysical, typename CommitPhysical,
          typename CommitLogical, typename RollbackLogical>
BlufiWifiScanStartOutcome AdvanceBlufiWifiScanStartCommit(
        BlufiWifiScanStartOutcome outcome, bool retry_on_abandon,
        AbandonPhysical abandon_physical, CommitPhysical commit_physical,
        CommitLogical commit_logical,
        RollbackLogical rollback_logical) noexcept {
    try {
        if (!outcome.driver_attempted && !outcome.abandoned) {
            if (!outcome.rollback_intent_set) {
                outcome.rollback_intent_set = true;
                outcome.rollback_for_retry = retry_on_abandon;
            }
            outcome.abandoned = abandon_physical();
            if (outcome.abandoned) {
                outcome.disposition =
                    BlufiWifiScanStartOutcome::LeaseDisposition::kReleased;
            }
        }
        if (outcome.abandoned) {
            if (!outcome.logical_committed) {
                if (outcome.rollback_for_retry) {
                    outcome.rollback = rollback_logical();
                    outcome.retry_unsubmitted = true;
                } else {
                    outcome.logical = commit_logical(false, false);
                }
                outcome.logical_committed = true;
            }
            return outcome;
        }
        if (!outcome.physical_committed) {
            outcome.physical = commit_physical(outcome.driver_accepted);
            outcome.physical_committed = true;
            outcome.disposition =
                BlufiWifiScanStartOutcome::LeaseDisposition::kSubmitted;
        }
        if (!outcome.logical_committed) {
            outcome.logical = commit_logical(
                outcome.driver_accepted ||
                    outcome.physical.callback_won_error,
                outcome.physical.drain_required);
            outcome.logical_committed = true;
        }
    } catch (...) {
        outcome.PreserveDispositionAfterFailure();
    }
    return outcome;
}
