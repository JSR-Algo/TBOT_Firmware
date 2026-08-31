/*
 * WiFi Manager - Unified WiFi connection management
 *
 * Thread Safety:
 * - All public methods are thread-safe (protected by internal mutex)
 * - Event callback is invoked from WiFi event task
 *
 * Usage:
 *   auto& wifi = WifiManager::GetInstance();
 *
 *   EventGroupHandle_t events = xEventGroupCreate();
 *   wifi.SetEventCallback([events](WifiEvent e) {
 *       if (e == WifiEvent::Connected) xEventGroupSetBits(events, BIT0);
 *       if (e == WifiEvent::ConfigModeExit) xEventGroupSetBits(events, BIT1);
 *   });
 *
 *   wifi.Initialize(config);
 *   wifi.StartStation();
 *   xEventGroupWaitBits(events, BIT0 | BIT1, pdTRUE, pdFALSE, portMAX_DELAY);
 */

#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef TBOT_WIFI_MANAGER_TESTING
#include <wifi_station.h>
#include <wifi_scan_recovery_executor.h>
#else
#include "wifi_station.h"
#include "wifi_scan_recovery_executor.h"
#endif
#include "wifi_scan_lease_coordinator.h"

class WifiStation;
class WifiConfigurationAp;

// WiFi events
enum class WifiEvent {
    Scanning,          // Started scanning for networks
    Connecting,        // Connecting to network (call GetSsid() for target)
    Connected,         // Successfully connected
    Disconnected,      // Disconnected from network
    ConfigModeEnter,   // Entered config AP mode
    ConfigModeExit,    // Exited config AP mode
};

// Configuration
struct WifiManagerConfig {
    std::string ssid_prefix = "ESP32";    // AP mode SSID prefix
    std::string language = "vi-VN";       // Web UI language

    // Station mode scan interval with exponential backoff
    int station_scan_min_interval_seconds = 10;   // Initial scan interval (fast retry)
    int station_scan_max_interval_seconds = 300;  // Maximum scan interval (5 minutes)
};

/**
 * WifiManager - Singleton for WiFi management
 */
class WifiManager {
public:
    static WifiManager& GetInstance();

    // ==================== Lifecycle ====================

    bool Initialize(const WifiManagerConfig& config = WifiManagerConfig{});
    bool StopRadio();
    bool IsInitialized() const;
    bool HasTeardownFault() const;

    // ==================== Station Mode ====================

    void StartStation();   // Non-blocking, auto-stops config AP if active
    void StopStation();    // Non-blocking

    bool IsConnected() const;
    std::string GetSsid() const;
    std::string GetIpAddress() const;
    int GetRssi() const;
    int GetChannel() const;
    std::string GetMacAddress() const;

    // ==================== Config AP Mode ====================

    void StartConfigAp();  // Non-blocking, auto-stops station if active
    void StopConfigAp();   // Non-blocking

    bool IsConfigMode() const;
    std::string GetApSsid() const;
    std::string GetApWebUrl() const;

    // ==================== Power ====================

    void SetPowerSaveLevel(WifiPowerSaveLevel level);

    // ==================== Event ====================

    void SetEventCallback(std::function<void(WifiEvent, const std::string&)> callback);

    const WifiManagerConfig& GetConfig() const { return config_; }

    // Callback paths use this accessor without taking the manager lifecycle lock.
    WifiScanLeaseCoordinator& ScanLeaseCoordinator() {
        return scan_lease_coordinator_;
    }

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

#ifdef TBOT_WIFI_MANAGER_TESTING
    WifiManager();
    void TestScheduleScanRecovery(
        const WifiScanLeaseCoordinator::Lease& lease) {
        ScheduleScanRecovery(lease);
    }
    void TestRunScanRecovery() { RunScanRecovery(); }
    WifiStation* TestStation() { return station_.get(); }
    WifiConfigurationAp* TestConfigAp() { return config_ap_.get(); }
    bool TestRecoveryActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return scan_recovery_active_;
    }
    bool TestStationActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return station_active_;
    }
    bool TestConfigActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_mode_active_;
    }
    void TestAdvanceLifecycleGeneration() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++lifecycle_generation_;
    }
#endif

private:
#ifndef TBOT_WIFI_MANAGER_TESTING
    WifiManager();
#endif
    // Event handlers and callbacks capture scanners, the coordinator, and
    // manager state. The singleton therefore owns the whole graph for the
    // process lifetime; production teardown is intentionally unavailable.
    ~WifiManager() = delete;

    void NotifyEvent(WifiEvent event, const std::string& data = "");
    void ScheduleScanRecovery(
        const WifiScanLeaseCoordinator::Lease& lease);
    static void ScanRecoveryTask(void* context);
    void RunScanRecovery();

    enum class PendingLifecycleTarget : uint8_t {
        kNone,
        kStation,
        kConfigAp,
    };
    bool DeferLifecycleTransitionForRecovery(
        PendingLifecycleTarget target, uint64_t transition_generation);
    void ResumePendingLifecycleTransition();
    void StartStationTarget(WifiStation* station,
                            const WifiManagerConfig& config,
                            uint64_t transition_generation);
    void StartConfigApTarget(WifiConfigurationAp* config_ap,
                             const WifiManagerConfig& config,
                             uint64_t transition_generation);

    struct ScanRecoveryWork {
        WifiScanLeaseCoordinator::Lease lease;
        WifiScanLeaseCoordinator::RecoveryDecision recovery;
        std::optional<WifiScanLeaseCoordinator::RecoveryProof> proof;
        uint64_t scan_session_id = 0;
        bool scans_were_enabled = false;
    };

    WifiManagerConfig config_;
    WifiScanLeaseCoordinator scan_lease_coordinator_;
    WifiScanRecoveryExecutor scan_recovery_executor_;
    std::unique_ptr<WifiStation> station_;
    std::unique_ptr<WifiConfigurationAp> config_ap_;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    bool wifi_runtime_ready_ = false;
    bool station_active_ = false;
    bool config_mode_active_ = false;
    uint64_t lifecycle_generation_ = 0;
    bool lifecycle_transition_in_progress_ = false;
    bool wifi_teardown_faulted_ = false;
    TaskHandle_t scan_recovery_task_ = nullptr;
    bool scan_recovery_active_ = false;
    bool scan_recovery_retry_pending_ = false;
    std::optional<WifiScanLeaseCoordinator::Lease> scan_recovery_debt_;
    std::optional<ScanRecoveryWork> scan_recovery_claim_;
    PendingLifecycleTarget pending_lifecycle_target_ =
        PendingLifecycleTarget::kNone;
    uint64_t pending_lifecycle_generation_ = 0;

    std::function<void(WifiEvent, const std::string&)> event_callback_;
    mutable std::string mac_address_;
};

#endif // _WIFI_MANAGER_H_
