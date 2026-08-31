#pragma once

#include <aes/esp_aes.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>
#include "esp_blufi_api.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "wifi_manager.h"
#include "blufi_wifi_scan_controller.h"
#include "blufi_transition_gate.h"
#include "audio/provisioning_session_binding.h"

class Blufi {
public:
    using ProvisioningToken = ProvisioningSessionBinding::Token;
    using ProvisioningReservation = ProvisioningSessionBinding::ReservationGuard;
    /**
     * @brief BLE setup state for heartbeat / observability.
     */
    enum class BleState {
        kOff,
        kAdvertising,
        kConnected,
        kTimeout,
    };

    /**
     * @brief Get the singleton instance of the Blufi class.
     */
    static Blufi &GetInstance();

    /**
     * @brief Initializes the Bluetooth controller, host, and Blufi profile.
     * This is the main entry point to start the Blufi process.
     * @return ESP_OK on success, otherwise an error code.
     */
    esp_err_t init();
    // prepare/on_current run while lifecycle ownership is held. They must not
    // re-enter a public Blufi lifecycle API, perform network I/O, or wait for
    // work that can require a BluFi callback.
    bool EnsureAdvertisingForSetupGeneration(
        uint32_t expected_generation, int timeout_seconds,
        ProvisioningToken* provisioning_token,
        const std::function<esp_err_t()>& prepare,
        const std::function<void()>& on_current = {});

    /**
     * @brief Deinitializes Blufi and the Bluetooth stack.
     * @return ESP_OK on success, otherwise an error code.
     */
    esp_err_t deinit();
    // on_current follows the lifecycle-owned callback contract documented on
    // EnsureAdvertisingForSetupGeneration().
    esp_err_t DeinitForSetupGeneration(
        uint32_t expected_generation,
        const std::function<void()>& on_current = {});

    /** Start a fresh provisioning generation for an explicit BOOT re-entry. */
    esp_err_t RestartForSetup();

    /** Run a short non-network action while setup generation ownership is stable. */
    bool RunIfSetupGenerationCurrent(uint32_t expected_generation,
                                     const std::function<void()>& action);
    /**
     * Run a bounded dispatch action while BOOT restart is excluded.
     * Lock order is lifecycle -> short finalization validation; action must not
     * perform network I/O or re-enter a public BluFi lifecycle API.
     */
    bool RunWithSetupGenerationCurrent(uint32_t expected_generation,
                                       const std::function<void()>& action);

    bool BindProvisioningSession(ProvisioningToken token);
    ProvisioningReservation TryReserveProvisioningSession();
    bool ClearProvisioningSession(ProvisioningToken token);
    ProvisioningToken CaptureProvisioningSession() const;
    bool IsBleStackFullyOff() const;
    bool AbortProvisioningSetup(ProvisioningToken token);
    bool CompleteSuccessfulProvisioningTeardown(const char* reason,
                                                ProvisioningToken provisioning_token);
    // on_current follows the lifecycle-owned callback contract documented on
    // EnsureAdvertisingForSetupGeneration().
    bool CompleteSuccessfulProvisioningTeardownForGeneration(
        const char* reason, ProvisioningToken provisioning_token,
        uint32_t expected_generation,
        const std::function<void()>& on_current = {});
    bool WasProvisioningSuccessfullyCompleted(ProvisioningToken provisioning_token) const;

    /**
     * @brief Returns the bootstrap token received via BluFi custom-data (tag=0x01).
     * Empty string if no token has been received yet.
     */
    const std::string& GetBootstrapToken() const { return bootstrap_token_; }

    /**
     * @brief Returns the provisioning code received via BluFi custom-data (tag=0x02).
     * Empty string if no code has been received yet.
     */
    const std::string& GetProvisioningCode() const { return provisioning_code_; }

    /**
     * @brief Zeroizes bootstrap_token_ and provisioning_code_ (call after successful report).
     */
    void ClearProvisioningSecrets(bool preserve_claim_token = true);

    /**
     * @brief Arm the BLE setup hard-timeout timer.
     *
     * Must be called from the Application task (or any task) immediately after
     * Blufi::init() succeeds.  When the timer fires without provisioning having
     * completed, the teardown is posted to the Application task (NOT executed
     * inside the timer callback) to avoid WDT/race conditions.
     *
     * @param seconds  Wall-clock budget for BLE provisioning (e.g. CONFIG_BLE_SETUP_TIMEOUT_SEC).
     */
    void StartBleSetupTimeout(int seconds);

    /**
     * @brief Cancel the BLE setup timeout timer (call on provisioning success or Wi-Fi connect).
     *
     * Safe to call even if the timer has already fired or was never started.
     */
    void CancelBleSetupTimeout();

    /**
     * @brief Return the current BLE state for heartbeat / observability.
     */
    BleState GetBleState() const;

    /**
     * @brief Return the current BLE state as a JSON-safe string.
     * @return One of "off", "advertising", "connected", "timeout".
     */
    const char* GetBleStateString() const;

    // Delete copy constructor and assignment operator for singleton
    Blufi(const Blufi &) = delete;

    Blufi &operator=(const Blufi &) = delete;

private:
    BlufiTransitionGate transition_gate_{ESP_ERR_INVALID_STATE};
    bool inited_ = false;
    std::atomic<bool> teardown_failed_{false};
    bool host_active_ = false;
    bool controller_active_ = false;
    bool profile_active_ = false;
    bool host_enabled_ = false;
    bool host_initialized_ = false;
    bool nimble_services_active_ = false;
    bool controller_enabled_ = false;
    bool controller_initialized_ = false;
    ProvisioningSessionBinding provisioning_session_;

    Blufi();

    ~Blufi();

    // Call only while ble_lifecycle_mutex_ is owned by the current task.
    esp_err_t InitWithLifecycleOwned();
    esp_err_t DeinitWithLifecycleOwned();
    bool StartBleSetupTimeoutWithLifecycleOwned(int seconds);
    esp_err_t _init_impl();
    esp_err_t _deinit_impl();

    // Initialization logic
    esp_err_t _controller_init();

    esp_err_t _controller_deinit();

    esp_err_t _host_init();

    esp_err_t _host_deinit();

    esp_err_t _gap_register_callback();

    esp_err_t _host_and_cb_init();

    void _security_init();

    void _security_deinit();

    void _dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len,
                                    bool *need_free);

    int _aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    int _aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _crc_checksum(uint8_t iv8, uint8_t *data, int len);

    bool _require_secure_session_for_credentials();

    void _handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

    static int _get_softap_conn_num();

    // WiFi scan methods
    bool EnsureWifiScanEventHandlerRegistered();
    void InvalidateWifiScanSession(uint32_t generation, uint64_t session,
                                   uint64_t connection_epoch);
    void UpdateWifiScanSession(uint32_t expected_generation,
                               uint64_t expected_session,
                               uint64_t expected_connection_epoch,
                               uint32_t generation, uint64_t session,
                               uint64_t connection_epoch);
    bool IsWifiScanCacheFresh() const;
    void RequestWifiListScan(bool save_results, bool send_list);
    bool StartOwnedWifiScan(uint64_t request_id);
    void SchedulePendingWifiScan(
        uint64_t request_id,
        const BlufiWifiScanController::Request& request);
    void ScheduleWifiScanFailure(
        const BlufiWifiScanController::Request& request,
        const char* reason);
    void ScheduleClaimRefreshAfterTokenHandoff();
    void TryReportProvisioningAuthenticated(const char* reason, uint32_t expected_generation);
    bool CompleteSuccessfulProvisioningTeardownImpl(
        const char* reason, ProvisioningToken provisioning_token,
        std::optional<uint32_t> expected_generation,
        const std::function<void()>& on_current = {});
    bool CompleteSuccessfulProvisioningTeardownWithLifecycleOwned(
        const char* reason, ProvisioningToken provisioning_token,
        const std::function<void()>& on_current = {});
    bool ReleaseBleForStationAssociation(uint32_t expected_generation);
    void RestoreBleAfterStationFailure(uint32_t expected_generation);
    void StartStationConnectFromCredentials(const char* reason);
    void SendStationConnectFailureReport();
    void ScheduleStationConnectFallback();
    void _send_wifi_list(std::vector<wifi_ap_record_t> ap_records);
    void ScheduleWifiListSend(uint32_t expected_generation,
                              uint64_t expected_ble_session_state,
                              uint64_t expected_ble_connection_epoch,
                              uint64_t expected_wifi_list_dispatch_epoch,
                              std::vector<wifi_ap_record_t> ap_records);
    static void _wifi_scan_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                         void *event_data);

    // These C-style functions are registered with ESP-IDF and call the corresponding instance
    // methods.

    static void _event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

    static void _negotiate_data_handler_trampoline(uint8_t *data, int len, uint8_t **output_data,
                                                   int *output_len, bool *need_free);

    static int _encrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static int _decrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _checksum_func_trampoline(uint8_t iv8, uint8_t *data, int len);

#ifdef CONFIG_BT_NIMBLE_ENABLED
    static void _nimble_on_reset(int reason);
    static void _nimble_on_sync();
    static void _nimble_host_task(void *param);
#endif

    // Security context, formerly blufi_sec struct
    struct BlufiSecurity {
#define DH_SELF_PUB_KEY_LEN 128
        uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN 128
        uint8_t share_key[SHARE_KEY_LEN];
        size_t share_len;
#define PSK_LEN 16
        uint8_t psk[PSK_LEN];
        uint8_t *dh_param;
        int dh_param_len;
        uint8_t iv[16];
        mbedtls_dhm_context *dhm;
        // Use the mbedTLS AES context type (matches the mbedtls_aes_* API used in
        // blufi.cpp). With CONFIG_MBEDTLS_HARDWARE_AES on, this is aliased to
        // esp_aes_context; with HW AES off (to free internal/DMA RAM for the WSS
        // TLS path) it is the software context. Correct under both. CFB128 output
        // is identical, so BluFi provisioning is unaffected.
        mbedtls_aes_context *aes;
    };

    BlufiSecurity *m_sec;
    bool m_blufi_security_negotiated;

    // Bootstrap token received via BluFi custom-data TLV tag=0x01 (RAM only, never written to NVS)
    std::string bootstrap_token_;
    // Provisioning code received via BluFi custom-data TLV tag=0x02
    std::string provisioning_code_;
    std::optional<uint32_t> provisioning_report_owner_generation_;

    // State variables
    wifi_config_t m_sta_config{};
    bool m_ble_is_connected;
    bool m_sta_connected;
    bool m_sta_got_ip;
    bool m_provisioned;
    bool m_deinited;
    uint8_t m_sta_bssid[6]{};
    uint8_t m_sta_ssid[32]{};
    int m_sta_ssid_len;
    size_t m_sta_config_ssid_len_ = 0;
    std::atomic<bool> m_sta_is_connecting{false};
    std::atomic<bool> m_wifi_connect_task_started{false};
    std::atomic<uint32_t> setup_generation_{0};
    std::atomic<uint64_t> ble_session_state_{0};
    // Never reset at reconnect: distinguishes clients within one setup generation.
    std::atomic<uint64_t> ble_connection_epoch_{0};
    std::atomic<uint32_t> ssid_transaction_id_{0};
    // Serializes multi-step BLE teardown/restart sequences without blocking callbacks.
    std::mutex ble_lifecycle_mutex_;
    std::mutex provisioning_finalization_mutex_;
    esp_blufi_extra_info_t m_sta_conn_info{};

    // BLE hard-timeout safety gate (#1)
    esp_timer_handle_t ble_setup_timer_ = nullptr;  // one-shot timer; nullptr when not armed
    std::mutex ble_setup_timer_mutex_;
    bool ble_timed_out_ = false;                    // set by timer callback; prevents adv restart
    std::atomic<uint32_t> ble_timeout_generation_{0};

    // BLE re-advertise cap (C8): bound how many times advertising is restarted
    // after a peer disconnect so a flapping/aborting central cannot make BLE
    // tight-loop restart. Reset to 0 in init(); the 300s hard-timeout is the
    // outer backstop. After the cap BLE stays down until the next setup entry.
    int ble_readvertise_count_ = 0;
    static constexpr int kMaxBleReadvertiseAttempts = 5;

    // Static trampoline for the esp_timer callback
    static void _ble_setup_timeout_cb(void* arg);

    // WiFi scan related
    std::vector<wifi_ap_record_t> m_ap_records;
    int64_t m_ap_records_updated_us = 0;
    // Protected by provisioning_finalization_mutex_ together with the records
    // and timestamp so cached APs cannot cross a BLE/setup ownership boundary.
    std::optional<BlufiWifiScanController::Request> m_ap_records_owner_;
    static constexpr int64_t kWifiScanCacheMaxAgeUs = 10LL * 1000 * 1000;
    BlufiWifiScanController wifi_scan_controller_;
    esp_event_handler_instance_t scan_event_instance_ = nullptr;
    std::atomic<uint64_t> m_wifi_list_dispatch_epoch_{0};
    // Zero means no queued response; otherwise this is the owning dispatch token.
    std::atomic<uint64_t> m_wifi_list_dispatch_pending_epoch_{0};
};
