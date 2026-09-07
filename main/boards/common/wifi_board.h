#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include "wifi_station_start_result.h"
#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <atomic>

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    std::atomic<bool> in_config_mode_{false};
    std::atomic<bool> wifi_config_entry_pending_{false};
    std::atomic<uint32_t> wifi_config_entry_intent_{0};
    std::atomic<uint32_t> wifi_config_entry_request_generation_{0};
    std::atomic<uint32_t> wifi_recovery_generation_{0};
    std::atomic<uint32_t> wifi_config_entry_recovery_generation_{0};
    std::atomic<int64_t> wifi_recovery_deadline_us_{0};
    NetworkEventCallback network_event_callback_ = nullptr;

    enum class WifiConfigEntryResult : uint8_t {
        kStarted,
        kCancelled,
        kRetry,
    };
    static constexpr uint32_t kWifiConfigIntentConditional = 1u << 0;
    static constexpr uint32_t kWifiConfigIntentExplicit = 1u << 1;
    static constexpr uint32_t kWifiConfigIntentNotify = 1u << 2;

    // AP-setup hard-timeout safety gate (mirrors the BLE gate in blufi.cpp).
    // SoftAP/Hotspot provisioning must NOT run forever: when this one-shot timer
    // fires without provisioning completing, StopConfigAp() is posted to the
    // Application task (never executed in the timer callback) to avoid WDT/race
    // conditions, and ap_timed_out_ latches so the AP is not re-opened.
    esp_timer_handle_t ap_setup_timer_ = nullptr;  // one-shot; nullptr when not armed
    bool ap_timed_out_ = false;                    // set by timer cb; blocks AP re-open

    /**
     * @brief Arm the AP-setup hard-timeout timer.
     * @param seconds Wall-clock budget for SoftAP provisioning (CONFIG_AP_SETUP_TIMEOUT_SEC).
     */
    void StartApSetupTimeout(int seconds);

    /**
     * @brief Cancel the AP-setup timeout (call on Wi-Fi connect / config-mode exit).
     * Safe to call when never armed or already fired.
     */
    void CancelApSetupTimeout();

    /**
     * @brief AP state string for heartbeat / observability.
     * @return "off" | "active" | "timeout".
     */
    const char* GetApStateString() const;

    static void OnApSetupTimeout(void* arg);

    virtual std::string GetBoardJson() override;

    /**
     * Handle network event (called from WiFi manager callbacks)
     * @param event The network event type
     * @param data Additional data (e.g., SSID for Connecting/Connected events)
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * Start WiFi connection attempt
     */
    void EnsureWifiRecoveryTimeout();
    WifiStationStartResult TryWifiConnect();

    /**
     * Enter WiFi configuration mode
     */
    void RequestWifiConfigMode(bool show_notification = false,
                               bool require_disconnected = false,
                               uint32_t recovery_generation = 0);
    void ScheduleWifiConfigIntentDrain();
    void ArmWifiConfigIntentRetry();
    WifiConfigEntryResult StartWifiConfigMode(bool show_notification = false,
                                              bool require_disconnected = false,
                                              uint32_t recovery_generation = 0);

    /**
     * WiFi connection timeout callback
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * Start network connection asynchronously
     * This function returns immediately. Network events are notified through the callback set by SetNetworkEventCallback().
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     * Enter WiFi configuration mode (thread-safe, can be called from any task)
     */
    void EnterWifiConfigMode();
    
    /**
     * Check if in WiFi config mode
     */
    bool IsInWifiConfigMode() const;
    void ResumePendingWifiConfigMode();
};

#endif // WIFI_BOARD_H
