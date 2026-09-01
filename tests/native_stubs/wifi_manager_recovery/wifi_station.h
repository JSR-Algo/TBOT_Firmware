#pragma once
#include "wifi_radio_recovery_restorer.h"
#include "wifi_scan_recovery_gate.h"
#include "wifi_scan_lease_coordinator.h"
#include <functional>
#include <optional>
#include <string>
enum class WifiPowerSaveLevel { None, Minimum, Maximum };
WifiRadioRecoveryRestorer::Driver& TestWifiRecoveryDriver();
class WifiStation {
public:
    explicit WifiStation(WifiScanLeaseCoordinator& coordinator);
    using ScanRecoveryClaim = WifiScanRecoveryGate::Claim;
    std::optional<ScanRecoveryClaim> ClaimScanRecovery(
        const WifiScanLeaseCoordinator::Lease&);
    bool HasScanRecoveryDebt(const WifiScanLeaseCoordinator::Lease&) const;
    bool CompleteScanRecovery(const ScanRecoveryClaim&,
                              const WifiScanLeaseCoordinator::RecoveryProof&);
    bool RestoreRadioAfterRecovery(const ScanRecoveryClaim&);
    void RetryScanAfterRecovery();
    void OnScanRecoveryNeeded(std::function<void(
        const WifiScanLeaseCoordinator::Lease&)> callback);
    void SetScanIntervalRange(int, int) {}
    void OnScanBegin(std::function<void()> cb) { on_scan_begin_ = std::move(cb); }
    void OnConnect(std::function<void(const std::string&)>) {}
    void OnConnected(std::function<void(const std::string&)>) {}
    void OnDisconnected(std::function<void(int)>) {}
    void Start();
    void Stop();
    WifiScanLeaseCoordinator::Lease PublishDebt(bool scans_enabled = true);
    void CallbackWins();
    void FailCompletionOnce();
    void FailRestoreAt(const std::string& stage);
    void PublishDebtOnStop();
    int RestoreCalls() const;
    int RetryCalls() const;
    bool RestoreRadioAfterExternalScanRecovery();
    void RetryAfterExternalScanRecovery(bool reconnect);
    int ExternalRestoreCalls() const { return external_restore_calls_; }
    int ExternalReconnectCalls() const { return external_reconnect_calls_; }
    bool ScansEnabled() const { return scans_enabled_; }
    bool IsConnected() const { return connected_; }
    std::string GetSsid() const { return {}; }
    std::string GetIpAddress() const { return {}; }
    int GetRssi() const { return 0; }
    int GetChannel() const { return 0; }
    void SetPowerSaveLevel(WifiPowerSaveLevel) {}
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
    int external_restore_calls_ = 0;
    int external_reconnect_calls_ = 0;
    bool connected_ = false;
    std::function<void(const WifiScanLeaseCoordinator::Lease&)> recovery_cb_;
    std::function<void()> on_scan_begin_;
};
