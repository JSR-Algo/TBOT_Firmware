#pragma once
#include "wifi_station.h"
class WifiConfigurationAp {
public:
    explicit WifiConfigurationAp(WifiScanLeaseCoordinator& coordinator);
    using ScanRecoveryClaim = WifiStation::ScanRecoveryClaim;
    std::optional<ScanRecoveryClaim> ClaimScanRecovery(
        const WifiScanLeaseCoordinator::Lease&);
    bool HasScanRecoveryDebt(const WifiScanLeaseCoordinator::Lease&) const;
    bool CompleteScanRecovery(const ScanRecoveryClaim&,
                              const WifiScanLeaseCoordinator::RecoveryProof&);
    bool RestoreRadioAfterRecovery(const ScanRecoveryClaim&);
    void RetryScanAfterRecovery();
    void OnScanRecoveryNeeded(std::function<void(
        const WifiScanLeaseCoordinator::Lease&)> callback);
    void SetSsidPrefix(const std::string&) {}
    void SetLanguage(const std::string&) {}
    void OnExitRequested(std::function<void()> cb) { exit_cb_ = std::move(cb); }
    void Start();
    bool Stop();
    WifiScanLeaseCoordinator::Lease PublishDebt(bool scans_enabled = true);
    void CallbackWins();
    void FailCompletionOnce();
    void FailRestoreAt(const std::string& stage);
    void PublishDebtOnStop();
    int RestoreCalls() const;
    int RetryCalls() const;
    bool ScansEnabled() const { return scans_enabled_; }
    std::string GetSsid() const { return {}; }
    std::string GetWebServerUrl() const { return {}; }
private:
    WifiScanLeaseCoordinator& coordinator_;
    WifiRadioRecoveryRestorer restorer_;
    std::optional<WifiScanLeaseCoordinator::Lease> scan_lease_;
    std::optional<WifiScanLeaseCoordinator::Lease> recovery_lease_;
    WifiScanRecoveryGate::RestoreState restore_state_;
    uint64_t scan_session_id_ = 1;
    bool scans_enabled_ = false;
    bool fail_completion_once_ = false;
    bool publish_debt_on_stop_ = false;
    int restore_calls_ = 0;
    int retry_calls_ = 0;
    int starts_ = 0;
    int stops_ = 0;
    std::function<void(const WifiScanLeaseCoordinator::Lease&)> recovery_cb_;
    std::function<void()> exit_cb_;
};
