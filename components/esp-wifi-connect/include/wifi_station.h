#ifndef _WIFI_STATION_H_
#define _WIFI_STATION_H_

#include <string>
#include <vector>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <optional>

#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

#include "wifi_scan_lease_coordinator.h"

// WiFi power save level enumeration
enum class WifiPowerSaveLevel {
    LOW_POWER,    // Maximum power saving (WIFI_PS_MAX_MODEM)
    BALANCED,     // Minimum power saving (WIFI_PS_MIN_MODEM)
    PERFORMANCE,  // No power saving (WIFI_PS_NONE) - full power
};

struct WifiApRecord {
    std::string ssid;
    std::string password;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
};

/**
 * WifiStation - WiFi station mode handler
 *
 * This class handles connecting to WiFi access points in station mode.
 * Note: WiFi driver must be initialized before using this class.
 */
class WifiStation {
public:
    explicit WifiStation(WifiScanLeaseCoordinator& scan_lease_coordinator);
    ~WifiStation();

    // Delete copy constructor and assignment operator
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    void AddAuth(const std::string &&ssid, const std::string &&password);
    void Start();
    void Stop();
    bool IsConnected();
    bool WaitForConnected(int timeout_ms = 10000);
    int8_t GetRssi();
    std::string GetSsid() const;
    std::string GetIpAddress() const;
    uint8_t GetChannel();
    void SetPowerSaveLevel(WifiPowerSaveLevel level);
    struct ScanRecoveryClaim {
        WifiScanLeaseCoordinator::Lease lease;
        WifiScanLeaseCoordinator::RecoveryDecision recovery;
    };
    std::optional<ScanRecoveryClaim> ClaimScanRecovery();
    bool CompleteScanRecovery(
        const ScanRecoveryClaim& claim,
        const WifiScanLeaseCoordinator::RecoveryProof& proof);

    void OnConnect(std::function<void(const std::string& ssid)> on_connect);
    void OnConnected(std::function<void(const std::string& ssid)> on_connected);
    void OnDisconnected(std::function<void(int reason)> on_disconnected);
    void OnScanBegin(std::function<void()> on_scan_begin);
    void SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds);

private:
    EventGroupHandle_t event_group_;
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_event_handler_instance_t instance_any_id_ = nullptr;
    esp_event_handler_instance_t instance_got_ip_ = nullptr;
    esp_netif_t* station_netif_ = nullptr;
    std::string ssid_;
    std::string password_;
    std::string ip_address_;
    int8_t max_tx_power_;
    uint8_t remember_bssid_;
    int reconnect_count_ = 0;

    // Exponential backoff for scan interval
    int scan_min_interval_microseconds_ = 10 * 1000 * 1000;   // Default 10 seconds
    int scan_max_interval_microseconds_ = 300 * 1000 * 1000;  // Default 5 minutes
    int scan_current_interval_microseconds_ = 10 * 1000 * 1000;  // Current interval
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void(int reason)> on_disconnected_;
    std::function<void()> on_scan_begin_;
    std::vector<WifiApRecord> connect_queue_;
    bool was_connected_ = false;  // Track if we were connected before disconnection
    WifiScanLeaseCoordinator& scan_lease_coordinator_;
    // When nested: callback -> scan -> data. Never acquire scan while holding
    // data, and never hold data across driver calls or external callbacks.
    std::recursive_mutex session_callback_mutex_;
    mutable std::mutex scan_mutex_;
    mutable std::mutex session_data_mutex_;
    std::optional<WifiScanLeaseCoordinator::Lease> scan_lease_;
    bool scans_enabled_ = false;
    uint64_t scan_session_id_ = 0;
    uint64_t lease_session_id_ = 0;
    size_t in_flight_session_operations_ = 0;
    std::condition_variable session_operations_drained_;
    std::optional<WifiScanLeaseCoordinator::Lease> scan_recovery_lease_;

    bool StartOwnedScan();
    void CompleteOwnedScan(const WifiScanLeaseCoordinator::Lease& lease);
    std::optional<WifiApRecord> HandleScanResultLocked(
        std::vector<wifi_ap_record_t> ap_records,
        std::optional<int64_t>& retry_delay_microseconds);
    void ScheduleScanRetry(uint64_t expected_session,
                           int64_t delay_microseconds);
    void StartConnect();
    WifiApRecord PrepareNextConnectLocked();
    std::string StartConnectForSession(WifiApRecord ap_record);
    bool TryBeginSessionOperationLocked(uint64_t session_id);
    void FinishSessionOperation();
    void DispatchSessionCallback(uint64_t expected_session,
                                 std::function<void()> callback);
    void FinishDiscardedCompletionLocked(
        const WifiScanLeaseCoordinator::Lease& lease);
    void RetainRecoveryDebtLocked(
        const WifiScanLeaseCoordinator::Lease& lease);
    void ClearRecoveryDebtLocked(
        const WifiScanLeaseCoordinator::Lease& lease);
    void UpdateScanInterval();  // Exponential backoff for scan interval
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
};

#endif // _WIFI_STATION_H_
