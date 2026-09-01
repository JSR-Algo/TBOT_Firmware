#ifndef _WIFI_CONFIGURATION_AP_H_
#define _WIFI_CONFIGURATION_AP_H_

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>
#include <optional>
#include <condition_variable>
#include <atomic>

#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

#include "dns_server.h"
#include "sdkconfig.h"
#include "wifi_scan_lease_coordinator.h"
#include "wifi_radio_recovery_restorer.h"
#include "wifi_scan_recovery_gate.h"

/**
 * WifiConfigurationAp - WiFi configuration access point
 *
 * Creates a WiFi hotspot with a captive portal for configuring WiFi credentials.
 * Note: WiFi driver must be initialized before using this class.
 */
class WifiConfigurationAp {
public:
    explicit WifiConfigurationAp(
        WifiScanLeaseCoordinator& scan_lease_coordinator);
    ~WifiConfigurationAp();

    // Delete copy constructor and assignment operator
    WifiConfigurationAp(const WifiConfigurationAp&) = delete;
    WifiConfigurationAp& operator=(const WifiConfigurationAp&) = delete;

    void SetSsidPrefix(const std::string &&ssid_prefix);
    void SetSsidPrefix(const std::string &ssid_prefix);
    void SetLanguage(const std::string &&language);
    void SetLanguage(const std::string &language);
    void Start();
    bool Stop();
#if !CONFIG_IDF_TARGET_ESP32P4
    void StartSmartConfig();
#endif
    bool ConnectToWifi(const std::string &ssid, const std::string &password);
    void Save(const std::string &ssid, const std::string &password);
    std::vector<wifi_ap_record_t> GetAccessPoints();
    std::string GetSsid();
    std::string GetWebServerUrl();
    using ScanRecoveryClaim = WifiScanRecoveryGate::Claim;
    std::optional<ScanRecoveryClaim> ClaimScanRecovery(
        const WifiScanLeaseCoordinator::Lease& expected_lease);
    bool HasScanRecoveryDebt(
        const WifiScanLeaseCoordinator::Lease& expected_lease) const;
    bool CompleteScanRecovery(
        const ScanRecoveryClaim& claim,
        const WifiScanLeaseCoordinator::RecoveryProof& proof);
    bool RestoreRadioAfterRecovery(const ScanRecoveryClaim& claim);
    void RetryScanAfterRecovery();
    bool RestoreRadioAfterExternalScanRecovery();
    void RetryAfterExternalScanRecovery();
    void OnScanRecoveryNeeded(std::function<void(
        const WifiScanLeaseCoordinator::Lease&)> callback);

    /**
     * Set callback for when exit is requested from config mode
     * This is called when user requests to exit config mode (e.g., via /exit endpoint)
     */
    void OnExitRequested(std::function<void()> callback);

private:
    std::mutex mutex_;
    std::unique_ptr<DnsServer> dns_server_;
    httpd_handle_t server_ = NULL;
    EventGroupHandle_t event_group_;
    std::string ssid_prefix_;
    std::string language_;
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    esp_timer_handle_t scan_timer_ = nullptr;
    bool is_connecting_ = false;
    esp_netif_t* ap_netif_ = nullptr;
    std::vector<wifi_ap_record_t> ap_records_;
    WifiScanLeaseCoordinator& scan_lease_coordinator_;
    WifiRadioRecoveryRestorer radio_recovery_restorer_;
    mutable std::mutex scan_mutex_;
    std::optional<WifiScanLeaseCoordinator::Lease> scan_lease_;
    bool scans_enabled_ = false;
    uint64_t scan_session_id_ = 0;
    uint64_t lease_session_id_ = 0;
    uint64_t connection_attempt_id_ = 0;
    uint64_t active_connection_attempt_id_ = 0;
    uint64_t active_connection_session_id_ = 0;
    bool connection_waiter_active_ = false;
    std::condition_variable connection_waiter_drained_;
    bool connection_boundary_waiting_ = false;
    bool connection_boundary_waiting_for_stop_ = false;
    bool connection_boundary_ready_ = true;
    bool teardown_faulted_ = false;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> started_{false};
    std::optional<WifiScanLeaseCoordinator::Lease> scan_recovery_lease_;
    WifiScanRecoveryGate::RestoreState scan_recovery_restore_state_;
    std::function<void(const WifiScanLeaseCoordinator::Lease&)>
        scan_recovery_needed_;

    // 高级配置项
    std::string ota_url_;
    int8_t max_tx_power_;
    bool remember_bssid_;
    bool sleep_mode_;

    // Callbacks
    std::function<void()> on_exit_requested_;

    void StartAccessPoint();
    void StartWebServer();
    bool StartOwnedScan();
    void CompleteOwnedScan(const WifiScanLeaseCoordinator::Lease& lease);
    void ScheduleScanRetry(uint64_t expected_session,
                           int64_t delay_microseconds);
    bool FinishConnectionAttemptBoundary(uint64_t attempt_session,
                                         uint64_t attempt_id,
                                         bool stop_wifi = false);
    void RetainRecoveryDebtLocked(
        const WifiScanLeaseCoordinator::Lease& lease);
    void ClearRecoveryDebtLocked(
        const WifiScanLeaseCoordinator::Lease& lease);
    void NotifyScanRecoveryNeeded(
        const WifiScanLeaseCoordinator::Lease& lease);

    // Event handlers
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
#if !CONFIG_IDF_TARGET_ESP32P4
    static void SmartConfigEventHandler(void* arg, esp_event_base_t event_base,
                                      int32_t event_id, void* event_data);
    esp_event_handler_instance_t sc_event_instance_ = nullptr;
#endif
};

#endif // _WIFI_CONFIGURATION_AP_H_
