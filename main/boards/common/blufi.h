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

class Blufi {
public:
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
     * @brief Start WiFi scan for Blufi provisioning
     * This method intelligently handles WiFi scanning based on current WiFi state:
     * - If WiFi config mode is active, it uses the existing scan results from WifiConfigurationAp
     * - Otherwise, it performs a dedicated scan without interfering with normal WiFi operations
     * @return true if a scan was started (or was already in progress); false on failure.
     */
    bool start_wifi_scan();

    /**
     * @brief Initializes the Bluetooth controller, host, and Blufi profile.
     * This is the main entry point to start the Blufi process.
     * @return ESP_OK on success, otherwise an error code.
     */
    esp_err_t init();

    /**
     * @brief Deinitializes Blufi and the Bluetooth stack.
     * @return ESP_OK on success, otherwise an error code.
     */
    esp_err_t deinit();

    /** Start a fresh provisioning generation for an explicit BOOT re-entry. */
    esp_err_t RestartForSetup();

    /** Run a short non-network action while setup generation ownership is stable. */
    bool RunIfSetupGenerationCurrent(uint32_t expected_generation,
                                     const std::function<void()>& action);

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
    void ClearProvisioningSecrets();

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
    bool inited_ = false;
    std::atomic<bool> teardown_failed_{false};

    Blufi();

    ~Blufi();

    // Initialization logic
    static esp_err_t _controller_init();

    static esp_err_t _controller_deinit();

    static esp_err_t _host_init();

    static esp_err_t _host_deinit();

    static esp_err_t _gap_register_callback();

    static esp_err_t _host_and_cb_init();

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
    bool IsWifiScanCacheFresh() const;
    void ScheduleClaimRefreshAfterTokenHandoff();
    void TryReportProvisioningAuthenticated(const char* reason, uint32_t expected_generation);
    void StartStationConnectFromCredentials(const char* reason);
    void SendStationConnectFailureReport();
    void ScheduleStationConnectFallback();
    void _send_wifi_list();
    void _start_dedicated_wifi_scan();
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
    std::atomic<uint32_t> ssid_transaction_id_{0};
    std::mutex provisioning_finalization_mutex_;
    esp_blufi_extra_info_t m_sta_conn_info{};

    // BLE hard-timeout safety gate (#1)
    esp_timer_handle_t ble_setup_timer_ = nullptr;  // one-shot timer; nullptr when not armed
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
    static constexpr int64_t kWifiScanCacheMaxAgeUs = 10LL * 1000 * 1000;
    bool m_scan_in_progress = false;
    esp_event_handler_instance_t scan_event_instance_ = nullptr;
    // When true, scan results are stored in m_ap_records on scan completion.
    // Cleared during connect-to-AP so that the connect-time scan does not
    // overwrite the cache with results gathered for connection purposes.
    bool m_scan_should_save_ssid = true;
    // When true, the next scan-done event responds to a pending GET_WIFI_LIST
    // request from the App. Set by the GET_WIFI_LIST handler when no cache is
    // available or a scan is already in flight; cleared by the scan-done
    // handler after dispatching the response.
    bool m_send_list_after_scan = false;
};
