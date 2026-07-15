#include "blufi.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#ifdef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED
#include "provisioning_status_reporter.h"
#endif
#include "application.h"
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "wifi_manager.h"

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "console/console.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
extern void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
extern int esp_blufi_gatt_svr_init(void);
extern void esp_blufi_gatt_svr_deinit(void);
extern void esp_blufi_btc_init(void);
extern void esp_blufi_btc_deinit(void);
#endif

extern "C" {
void esp_blufi_adv_start(void);

void esp_blufi_adv_stop(void);

void esp_blufi_disconnect(void);

void btc_blufi_report_error(esp_blufi_error_state_t state);

#ifdef CONFIG_BT_BLUEDROID_ENABLED
void esp_blufi_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
int esp_blufi_gatt_svr_init(void);
void esp_blufi_gatt_svr_deinit(void);
void esp_blufi_btc_init(void);
void esp_blufi_btc_deinit(void);
#endif
}

#include <wifi_station.h>
#include "esp_crc.h"
#include "esp_random.h"
#include "mbedtls/md5.h"
#include "settings.h"
#include "ssid_manager.h"

static const char* BLUFI_TAG = "BLUFI_CLASS";
static constexpr int kClaimRefreshAfterTokenHandoffDelayMs = 2500;
static constexpr size_t kMaxBlufiWifiListApRecords = 4;

struct DelayedClaimRefreshContext {
    Blufi* self;
    Blufi::ProvisioningToken provisioning_token;
};

struct WifiConnectTaskContext {
    Blufi* self;
    Blufi::ProvisioningToken provisioning_token;
};

static void CaptureFirstError(esp_err_t& first_error, esp_err_t error) {
    if (first_error == ESP_OK && error != ESP_OK) {
        first_error = error;
    }
}

static std::string SanitizedSerial(const uint8_t* bytes, size_t max_len) {
    std::string serial;
    for (size_t i = 0; i < max_len && bytes[i] != 0; ++i) {
        const char ch = static_cast<char>(bytes[i]);
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') || ch == '-') {
            serial.push_back(ch);
        }
    }
    return serial;
}

static std::string GetBlufiDeviceName() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        const std::string serial = SanitizedSerial(serial_number, 32);
        if (!serial.empty()) {
            // The mobile allowlist only accepts names beginning with the TBOT
            // brand prefix, so a serial-provisioned unit must advertise
            // "TBOT-<serial>" (product doc) or it never appears for pairing.
            const bool already_prefixed =
                serial.size() >= 5 &&
                (serial[0] == 'T' || serial[0] == 't') &&
                (serial[1] == 'B' || serial[1] == 'b') &&
                (serial[2] == 'O' || serial[2] == 'o') &&
                (serial[3] == 'T' || serial[3] == 't') &&
                serial[4] == '-';
            return already_prefixed ? serial : (std::string("TBOT-") + serial);
        }
    }
#endif

    uint8_t mac[6] = {0};
    // Use the Wi-Fi STA MAC because this is the device identity used by the
    // backend, mobile pairing, lesson assignment, and production diagnostics.
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[24] = {0};
    snprintf(name, sizeof(name), "TBOT-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(name);
}

// Default esp_blufi_adv_start packs flags + TX power + 128-bit UUID + full local
// name into the 31-byte ADV → "Partial data write into ADV". Android/Xiaomi then
// often drops name and/or UUID, so the phone never allowlists the robot.
// Compact raw layout (always ≤31 bytes):
//   ADV:      flags + complete 16-bit UUID list (0xFFFF = BluFi)
//   Scan RSP: complete local name (TBOT-<MAC>)
static void StartTbotBlufiAdvertising(const char* device_name) {
#ifdef CONFIG_BT_BLUEDROID_ENABLED
    if (device_name != nullptr && device_name[0] != '\0') {
        esp_err_t name_err = esp_ble_gap_set_device_name(device_name);
        if (name_err != ESP_OK) {
            ESP_LOGW(BLUFI_TAG, "set device name failed: %s", esp_err_to_name(name_err));
        }
    }

    // Flags (LE General Discoverable | BR/EDR Not Supported) + 16-bit UUID 0xFFFF.
    static const uint8_t adv_raw[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0xFF, 0xFF,
    };

    uint8_t scan_rsp[31] = {};
    size_t rsp_len = 0;
    if (device_name != nullptr && device_name[0] != '\0') {
        size_t name_len = std::strlen(device_name);
        if (name_len > 29) {
            name_len = 29;
        }
        scan_rsp[0] = static_cast<uint8_t>(name_len + 1);
        scan_rsp[1] = 0x09;  // Complete Local Name
        std::memcpy(scan_rsp + 2, device_name, name_len);
        rsp_len = name_len + 2;
    }

    esp_err_t err = esp_ble_gap_config_adv_data_raw(const_cast<uint8_t*>(adv_raw), sizeof(adv_raw));
    if (err != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "raw ADV failed (%s); falling back to esp_blufi_adv_start",
                 esp_err_to_name(err));
        esp_blufi_adv_start();
        return;
    }
    if (rsp_len > 0) {
        err = esp_ble_gap_config_scan_rsp_data_raw(scan_rsp, rsp_len);
        if (err != ESP_OK) {
            ESP_LOGW(BLUFI_TAG, "raw scan RSP failed: %s", esp_err_to_name(err));
        }
    }

    // BluFi's GAP handler only auto-starts advertising on structured ADV set
    // complete — not on raw. Start explicitly with BluFi-compatible params.
    esp_ble_adv_params_t params = {};
    params.adv_int_min = 0x100;
    params.adv_int_max = 0x100;
    params.adv_type = ADV_TYPE_IND;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.channel_map = ADV_CHNL_ALL;
    params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    err = esp_ble_gap_start_advertising(&params);
    if (err != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "start advertising failed (%s); falling back to esp_blufi_adv_start",
                 esp_err_to_name(err));
        esp_blufi_adv_start();
        return;
    }
    ESP_LOGI(BLUFI_TAG, "TBOT compact ADV: UUID16 0xFFFF + name in scan RSP");
#else
    (void)device_name;
    esp_blufi_adv_start();
#endif
}

static wifi_mode_t GetWifiModeWithFallback(const WifiManager& wifi) {
    if (wifi.IsConfigMode()) {
        return WIFI_MODE_AP;
    }
    if (wifi.IsInitialized() && wifi.IsConnected()) {
        return WIFI_MODE_STA;
    }

    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    return mode;
}

Blufi& Blufi::GetInstance() {
    static Blufi instance;
    return instance;
}

Blufi::Blufi()
    : m_sec(nullptr),
      m_blufi_security_negotiated(false),
      m_ble_is_connected(false),
      m_sta_connected(false),
      m_sta_got_ip(false),
      m_provisioned(false),
      m_deinited(false),
      m_sta_ssid_len(0),
      m_sta_is_connecting(false),
      m_wifi_connect_task_started(false),
      ble_setup_timer_(nullptr),
      ble_timed_out_(false) {
    memset(&m_sta_config, 0, sizeof(m_sta_config));
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
    memset(&m_sta_conn_info, 0, sizeof(m_sta_conn_info));
}

Blufi::~Blufi() {
    if (m_sec) {
        _security_deinit();
    }
}

esp_err_t Blufi::init() {
    auto turn = transition_gate_.Acquire(
        BlufiTransitionGate::Operation::kInit,
        reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()));
    if (!turn.owner()) {
        return static_cast<esp_err_t>(turn.result());
    }
    const esp_err_t result = _init_impl();
    transition_gate_.Complete(turn, result);
    return result;
}

esp_err_t Blufi::_init_impl() {
    esp_err_t ret = ESP_FAIL;
    if (host_active_ || controller_active_) {
        ESP_LOGE(BLUFI_TAG, "BLUFI init rejected while prior teardown is incomplete");
        return ESP_ERR_INVALID_STATE;
    }

    // inited_ is set true only after both stack layers initialize successfully.
    inited_ = false;
    m_provisioned = false;
    m_deinited = false;
    ble_timed_out_ = false;
    ble_readvertise_count_ = 0;  // fresh setup window -> reset the re-adv cap
    provisioning_report_in_flight_ = false;
    m_scan_should_save_ssid = true;
    m_wifi_connect_task_started = false;

    auto& wifi_manager = WifiManager::GetInstance();
    if (wifi_manager.IsInitialized() && wifi_manager.IsConfigMode()) {
        ESP_LOGE(BLUFI_TAG,
                 "Blufi and WiFi hotspot network configuration cannot "
                 "be used simultaneously.");
        inited_ = false;
        m_deinited = true;
        return ret;
    }

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = _controller_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI controller init failed: %s", esp_err_to_name(ret));
        const esp_err_t cleanup_error = _controller_deinit();
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "BLUFI controller cleanup failed: %s",
                     esp_err_to_name(cleanup_error));
        }
        inited_ = false;
        m_deinited = !host_active_ && !controller_active_;
        return ret;
    }
#endif

    ret = _host_and_cb_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI host and cb init failed: %s", esp_err_to_name(ret));
        esp_err_t cleanup_error = ESP_OK;
        if (host_active_) {
            cleanup_error = _host_deinit();
            if (cleanup_error != ESP_OK) {
                ESP_LOGE(BLUFI_TAG, "BLUFI host cleanup failed: %s",
                         esp_err_to_name(cleanup_error));
            }
        }
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
        if (!host_active_) {
            const esp_err_t controller_error = _controller_deinit();
            if (controller_error != ESP_OK) {
                ESP_LOGE(BLUFI_TAG, "BLUFI controller cleanup failed: %s",
                         esp_err_to_name(controller_error));
            }
        }
#endif
        inited_ = false;
        m_deinited = !host_active_ && !controller_active_;
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "BLUFI VERSION %04x", esp_blufi_get_version());
    inited_ = true;
    return ESP_OK;
}

esp_err_t Blufi::deinit() {
    auto turn = transition_gate_.Acquire(
        BlufiTransitionGate::Operation::kDeinit,
        reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()));
    if (!turn.owner()) {
        return static_cast<esp_err_t>(turn.result());
    }
    const esp_err_t result = _deinit_impl();
    transition_gate_.Complete(turn, result);
    return result;
}

esp_err_t Blufi::_deinit_impl() {
    esp_err_t first_error = ESP_OK;

    if (m_deinited && !host_active_ && !controller_active_) {
        return ESP_OK;
    }
    if (scan_event_instance_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              scan_event_instance_);
        scan_event_instance_ = nullptr;
    }
    m_scan_in_progress = false;
    m_send_list_after_scan = false;

    if (host_active_) {
        const esp_err_t host_error = _host_deinit();
        CaptureFirstError(first_error, host_error);
        if (host_error != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Host deinit failed: %s", esp_err_to_name(host_error));
        }
        if (host_error == ESP_OK && host_active_) {
            CaptureFirstError(first_error, ESP_ERR_INVALID_STATE);
        }
        if (host_error != ESP_OK || host_active_) {
            return first_error;
        }
    }
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    if (controller_active_) {
        const esp_err_t controller_error = _controller_deinit();
        CaptureFirstError(first_error, controller_error);
        if (controller_error != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Controller deinit failed: %s",
                     esp_err_to_name(controller_error));
        }
        if (controller_error == ESP_OK && controller_active_) {
            CaptureFirstError(first_error, ESP_ERR_INVALID_STATE);
        }
    }
#endif

    if (first_error == ESP_OK && !host_active_ && !controller_active_) {
        m_deinited = true;
        inited_ = false;
    }
    return first_error;
}

void Blufi::BindProvisioningSession(ProvisioningToken token) {
    provisioning_session_.Bind(token);
}

Blufi::ProvisioningToken Blufi::CaptureProvisioningSession() const {
    return provisioning_session_.Capture();
}

bool Blufi::CompleteSuccessfulProvisioningTeardown(
        const char* reason, ProvisioningToken provisioning_token) {
    if (!provisioning_session_.Matches(provisioning_token)) {
        ESP_LOGW(BLUFI_TAG, "Ignoring stale provisioning teardown: reason=%s token=%llu",
                 reason ? reason : "unknown",
                 static_cast<unsigned long long>(provisioning_token.generation));
        return false;
    }
    ESP_LOGI(BLUFI_TAG, "Successful provisioning teardown requested: reason=%s",
             reason ? reason : "unknown");
    CancelBleSetupTimeout();
    const esp_err_t deinit_error = deinit();
    if (deinit_error != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Successful provisioning teardown failed: reason=%s error=%s",
                 reason ? reason : "unknown", esp_err_to_name(deinit_error));
        return false;
    }

    const bool rearmed = Application::GetInstance().GetAudioService().EndWifiProvisioningAndRearm(
        provisioning_token);
    if (!rearmed) {
        ESP_LOGW(BLUFI_TAG, "Provisioning teardown did not rearm: reason=%s token=%llu",
                 reason ? reason : "unknown",
                 static_cast<unsigned long long>(provisioning_token.generation));
        return false;
    }
    provisioning_session_.ClearIfMatches(provisioning_token);
    ESP_LOGI(BLUFI_TAG, "Successful provisioning teardown complete: reason=%s rearmed=%d",
             reason ? reason : "unknown", static_cast<int>(rearmed));
    return true;
}

#ifdef CONFIG_BT_BLUEDROID_ENABLED
esp_err_t Blufi::_host_init() {
    esp_err_t ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s init bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    host_initialized_ = true;
    host_active_ = true;
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    host_enabled_ = true;
    ESP_LOGI(BLUFI_TAG, "BD ADDR: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit() {
    if (profile_active_) {
        const esp_err_t ret = esp_blufi_profile_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "%s deinit profile failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
        profile_active_ = false;
    }
    if (host_enabled_) {
        const esp_err_t ret = esp_bluedroid_disable();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "%s disable bluedroid failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
        host_enabled_ = false;
    }
    if (host_initialized_) {
        const esp_err_t ret = esp_bluedroid_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "%s deinit bluedroid failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
        host_initialized_ = false;
    }
    host_active_ = profile_active_ || host_enabled_ || host_initialized_;
    return ESP_OK;
}

esp_err_t Blufi::_gap_register_callback() {
    esp_err_t rc = esp_ble_gap_register_callback(esp_blufi_gap_event_handler);
    if (rc) {
        return rc;
    }
    return esp_blufi_profile_init();
}

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = _host_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialise host failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s blufi register failed, error code = %x", __func__, ret);
        return ret;
    }
    ret = _gap_register_callback();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s gap register failed, error code = %x", __func__, ret);
        return ret;
    }
    profile_active_ = true;
    host_active_ = true;
    return ESP_OK;
}
#endif /* CONFIG_BT_BLUEDROID_ENABLED */

#ifdef CONFIG_BT_NIMBLE_ENABLED
// Stubs for NimBLE specific store functionality
void ble_store_config_init();

void Blufi::_nimble_on_reset(int reason) {
    ESP_LOGE(BLUFI_TAG, "NimBLE Resetting state; reason=%d", reason);
}

void Blufi::_nimble_on_sync() {
    if (esp_blufi_profile_init() == ESP_OK) {
        auto& blufi = GetInstance();
        blufi.profile_active_ = true;
        blufi.host_active_ = true;
    }
}

void Blufi::_nimble_host_task(void* param) {
    ESP_LOGI(BLUFI_TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t Blufi::_host_init() {
    ble_hs_cfg.reset_cb = _nimble_on_reset;
    ble_hs_cfg.sync_cb = _nimble_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;

    ble_hs_cfg.sm_io_cap = 4;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif

    int rc = esp_blufi_gatt_svr_init();
    assert(rc == 0);

    ble_store_config_init();
    esp_blufi_btc_init();
    nimble_services_active_ = true;
    host_active_ = true;

    esp_err_t err = esp_nimble_enable(_nimble_host_task);
    if (err) {
        ESP_LOGE(BLUFI_TAG, "%s failed: %s", __func__, esp_err_to_name(err));
        return ESP_FAIL;
    }
    host_enabled_ = true;
    host_initialized_ = true;
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit(void) {
    if (profile_active_) {
        const esp_err_t ret = esp_blufi_profile_deinit();
        if (ret != ESP_OK) {
            return ret;
        }
        profile_active_ = false;
    }
    if (host_enabled_) {
        const esp_err_t ret = nimble_port_stop();
        if (ret != ESP_OK) {
            return ret;
        }
        host_enabled_ = false;
    }
    if (host_initialized_) {
        const esp_err_t ret = esp_nimble_deinit();
        if (ret != ESP_OK) {
            return ret;
        }
        host_initialized_ = false;
    }
    if (nimble_services_active_) {
        esp_blufi_gatt_svr_deinit();
        esp_blufi_btc_deinit();
        nimble_services_active_ = false;
    }
    host_active_ = profile_active_ || host_enabled_ || host_initialized_ ||
                   nimble_services_active_;
    return ESP_OK;
}

esp_err_t Blufi::_gap_register_callback(void) { return ESP_OK; }

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s blufi register failed, error code = %x", __func__, ret);
        return ret;
    }

    // Host init must be called after registering callbacks for NimBLE
    ret = _host_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialise host failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
esp_err_t Blufi::_controller_init() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    controller_initialized_ = true;
    controller_active_ = true;
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    controller_enabled_ = true;

#ifdef CONFIG_BT_NIMBLE_ENABLED
    ret = esp_nimble_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "esp_nimble_init() failed: %s", esp_err_to_name(ret));
        return ret;
    }
    host_initialized_ = true;
    host_active_ = true;
#endif
    return ESP_OK;
}

esp_err_t Blufi::_controller_deinit() {
    if (controller_enabled_) {
        const esp_err_t ret = esp_bt_controller_disable();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "%s disable controller failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
        controller_enabled_ = false;
    }
    if (controller_initialized_) {
        const esp_err_t ret = esp_bt_controller_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "%s deinit controller failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
        controller_initialized_ = false;
    }
    controller_active_ = controller_enabled_ || controller_initialized_;
    return ESP_OK;
}
#endif

static int myrand(void* rng_state, unsigned char* output, size_t len) {
    esp_fill_random(output, len);
    return 0;
}

void Blufi::_security_init() {
    m_blufi_security_negotiated = false;
    m_sec = new BlufiSecurity();
    if (m_sec == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate security context");
        return;
    }
    memset(m_sec, 0, sizeof(BlufiSecurity));
    m_sec->dhm = new mbedtls_dhm_context();
    m_sec->aes = new mbedtls_aes_context();

    mbedtls_dhm_init(m_sec->dhm);
    mbedtls_aes_init(m_sec->aes);

    memset(m_sec->iv, 0x0, sizeof(m_sec->iv));
}

void Blufi::_security_deinit() {
    m_blufi_security_negotiated = false;
    if (m_sec == nullptr)
        return;

    if (m_sec->dh_param) {
        free(m_sec->dh_param);
    }
    mbedtls_dhm_free(m_sec->dhm);
    mbedtls_aes_free(m_sec->aes);
    delete m_sec->dhm;
    delete m_sec->aes;
    delete m_sec;
    m_sec = nullptr;
}

void Blufi::_dh_negotiate_data_handler(uint8_t* data, int len, uint8_t** output_data,
                                       int* output_len, bool* need_free) {
    if (m_sec == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Security not initialized in DH handler");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    if (len < 1) {
        ESP_LOGE(BLUFI_TAG, "DH handler: data too short");
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint8_t type = data[0];
    switch (type) {
        case 0x00:
            if (len < 3) {
                ESP_LOGE(BLUFI_TAG, "DH_PARAM_LEN packet too short");
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }

            m_sec->dh_param_len = (data[1] << 8) | data[2];
            if (m_sec->dh_param) {
                free(m_sec->dh_param);
                m_sec->dh_param = nullptr;
            }
            m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH malloc failed");
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            }
            break;
        case 0x01: {
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH param not allocated");
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            uint8_t* param = m_sec->dh_param;
            memcpy(m_sec->dh_param, &data[1], m_sec->dh_param_len);
            int ret = mbedtls_dhm_read_params(m_sec->dhm, &param, &param[m_sec->dh_param_len]);
            if (ret) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_read_params failed %d", ret);
                btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
                return;
            }

            const int dhm_len = mbedtls_dhm_get_len(m_sec->dhm);

            ret = mbedtls_dhm_make_public(m_sec->dhm, dhm_len, m_sec->self_public_key, dhm_len,
                                          myrand, NULL);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_make_public failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
                return;
            }
            ret = mbedtls_dhm_calc_secret(m_sec->dhm, m_sec->share_key, SHARE_KEY_LEN,
                                          &m_sec->share_len, myrand, NULL);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_calc_secret failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }

            ret = mbedtls_md5(m_sec->share_key, m_sec->share_len, m_sec->psk);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_md5 failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
                return;
            }
            ret = mbedtls_aes_setkey_enc(m_sec->aes, m_sec->psk, PSK_LEN * 8);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_aes_setkey_enc failed: -0x%04X", -ret);
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }
            m_blufi_security_negotiated = true;
            *output_data = m_sec->self_public_key;
            *output_len = dhm_len;
            *need_free = false;
            ESP_LOGI(BLUFI_TAG, "DH negotiation completed successfully");

            free(m_sec->dh_param);
            m_sec->dh_param = nullptr;
            m_sec->dh_param_len = 0;
            break;
        }
        default:
            ESP_LOGE(BLUFI_TAG, "DH handler unknown type: %d", type);
    }
}

int Blufi::_aes_encrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len <= 0) {
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES encryption");
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_ENCRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);

    if (ret == 0) {
        return crypt_len;
    } else {
        ESP_LOGE(BLUFI_TAG, "AES encrypt failed: %d", ret);
        return ret;
    }
}

int Blufi::_aes_decrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len < 0) {
        // NOTE: must not dereference m_sec->aes here — the guard is reached when
        // m_sec itself is nullptr (short-circuit), so reading m_sec->aes would be
        // a NULL dereference. Log a bare string like the encrypt path does.
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES decryption");
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_DECRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);
    if (ret != 0) {
        ESP_LOGE(BLUFI_TAG, "AES decrypt failed: %d", ret);
        return ret;
    } else {
        return crypt_len;
    }
}

uint16_t Blufi::_crc_checksum(uint8_t iv8, uint8_t* data, int len) {
    return esp_crc16_be(0, data, len);
}

bool Blufi::_require_secure_session_for_credentials() {
    if (m_blufi_security_negotiated && m_sec != nullptr && m_sec->aes != nullptr) {
        return true;
    }
    ESP_LOGW(BLUFI_TAG, "Rejecting BluFi Wi-Fi credential frame before DH/AES negotiation");
    esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
    return false;
}

int Blufi::_get_softap_conn_num() {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || !wifi.IsConfigMode()) {
        return 0;
    }

    wifi_sta_list_t sta_list{};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

bool Blufi::EnsureWifiScanEventHandlerRegistered() {
    if (scan_event_instance_ != nullptr) {
        return true;
    }

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                        &Blufi::_wifi_scan_event_handler, this,
                                                        &scan_event_instance_);
    if (err != ESP_OK) {
        scan_event_instance_ = nullptr;
        ESP_LOGE(BLUFI_TAG, "Failed to register WiFi scan handler: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool Blufi::IsWifiScanCacheFresh() const {
    if (m_ap_records.empty() || m_ap_records_updated_us <= 0) {
        return false;
    }
    return esp_timer_get_time() - m_ap_records_updated_us <= kWifiScanCacheMaxAgeUs;
}

void Blufi::ScheduleClaimRefreshAfterTokenHandoff() {
    auto* ctx = new (std::nothrow) DelayedClaimRefreshContext{
        this, CaptureProvisioningSession()};
    if (ctx == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate claim token delay context");
        return;
    }
    BaseType_t created = xTaskCreate(
        [](void* raw_ctx) {
            auto* ctx = static_cast<DelayedClaimRefreshContext*>(raw_ctx);
            auto* self = ctx->self;
            const auto provisioning_token = ctx->provisioning_token;
            delete ctx;
            vTaskDelay(pdMS_TO_TICKS(kClaimRefreshAfterTokenHandoffDelayMs));
            Application::GetInstance().Schedule([self, provisioning_token]() {
                if (self->m_sta_is_connecting) {
                    ESP_LOGI(BLUFI_TAG,
                             "Deferring claim refresh: WiFi credential handoff in progress");
                    return;
                }
                if (!WifiManager::GetInstance().IsConnected()) {
                    ESP_LOGI(BLUFI_TAG,
                             "Skipping connected-WiFi claim refresh: WiFi is not connected");
                    return;
                }

                if (!self->CompleteSuccessfulProvisioningTeardown(
                        "connected_wifi_token_handoff", provisioning_token)) {
                    return;
                }
                Application::GetInstance().SchedulePendingTbotClaimRefresh();
            });
            vTaskDelete(nullptr);
        },
        "claim_token_delay", 4096, ctx, 5, nullptr);
    if (created != pdPASS) {
        delete ctx;
        ESP_LOGE(BLUFI_TAG, "Failed to create claim token delay task");
    }
}

void Blufi::TryReportProvisioningAuthenticated(const char* reason) {
#ifndef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED
    (void)reason;
    return;
#else
    Settings websocket_settings("websocket", false);
    if (!websocket_settings.GetString("claim_device_id").empty()) {
        // The mobile claim flow uses the same single-use bootstrap token for
        // /claim/confirm. Do not spend it on the legacy provisioning-status
        // report first, or the robot can fetch a pending claim but then fail the
        // strict confirm auth with a consumed token.
        ESP_LOGI(BLUFI_TAG,
                 "Skipping provisioning authenticated report during claim flow: reason=%s",
                 reason ? reason : "unknown");
        return;
    }

    const bool wifi_connected = WifiManager::GetInstance().IsConnected();
    const bool token_empty = bootstrap_token_.empty();
    const bool code_empty = provisioning_code_.empty();
    const auto ble_state = GetBleState();

    if (!wifi_connected || token_empty || code_empty || provisioning_report_in_flight_) {
        ESP_LOGI(BLUFI_TAG,
                 "Reporting provisioning authenticated skipped: reason=%s wifi_connected=%d token_empty=%d code_empty=%d in_flight=%d",
                 reason ? reason : "unknown", (int)wifi_connected, (int)token_empty,
                 (int)code_empty, (int)provisioning_report_in_flight_);
        return;
    }

    if (ble_state != BleState::kOff) {
        if (!m_sta_is_connecting) {
            auto* self = this;
            const auto provisioning_token = CaptureProvisioningSession();
            Application::GetInstance().Schedule([self, reason, provisioning_token]() {
                if (!self->CompleteSuccessfulProvisioningTeardown(
                        "authenticated_report_ble_release", provisioning_token)) {
                    return;
                }
                self->TryReportProvisioningAuthenticated(reason);
            });
        }
        ESP_LOGI(BLUFI_TAG,
                 "Reporting provisioning authenticated deferred until BLE is off: reason=%s ble_state=%s",
                 reason ? reason : "unknown", GetBleStateString());
        return;
    }

    struct ReportTaskContext {
        Blufi* self;
        std::string token;
        std::string code;
    };

    auto* ctx = new (std::nothrow) ReportTaskContext{this, bootstrap_token_, provisioning_code_};
    if (ctx == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate provisioning authenticated report context");
        return;
    }

    provisioning_report_in_flight_ = true;
    BaseType_t created = xTaskCreate(
        [](void* raw_ctx) {
            auto* ctx = static_cast<ReportTaskContext*>(raw_ctx);
            bool ok = ProvisioningStatusReporter::Report(
                ProvisioningStatusReporter::Status::DeviceAuthenticated,
                ctx->token, ctx->code);
            auto* self = ctx->self;
            std::fill(ctx->token.begin(), ctx->token.end(), '\0');
            std::fill(ctx->code.begin(), ctx->code.end(), '\0');
            delete ctx;

            Application::GetInstance().Schedule([self, ok]() {
                self->provisioning_report_in_flight_ = false;
                if (ok) {
                    ESP_LOGI(BLUFI_TAG, "Provisioning report succeeded after BLE teardown");
                    self->ClearProvisioningSecrets();
                } else {
                    ESP_LOGW(BLUFI_TAG,
                             "Provisioning report failed after BLE teardown; secrets retained for retry");
                }
            });
            vTaskDelete(nullptr);
        },
        "prov_auth_report", 6144, ctx, 5, nullptr);
    if (created != pdPASS) {
        provisioning_report_in_flight_ = false;
        std::fill(ctx->token.begin(), ctx->token.end(), '\0');
        std::fill(ctx->code.begin(), ctx->code.end(), '\0');
        delete ctx;
        ESP_LOGE(BLUFI_TAG, "Failed to create provisioning authenticated report task");
        return;
    }

    ESP_LOGI(BLUFI_TAG,
             "Provisioning authenticated report started after BLE teardown: reason=%s",
             reason ? reason : "unknown");
#endif
}

void Blufi::StartStationConnectFromCredentials(const char* reason) {
    if (m_wifi_connect_task_started) {
        ESP_LOGI(BLUFI_TAG, "WiFi connect already started; ignoring duplicate trigger: %s",
                 reason ? reason : "unknown");
        return;
    }

    std::string ssid(reinterpret_cast<const char*>(m_sta_config.sta.ssid));
    std::string password(reinterpret_cast<const char*>(m_sta_config.sta.password));
    if (ssid.empty()) {
        ESP_LOGW(BLUFI_TAG, "Ignoring WiFi connect trigger with empty SSID: %s",
                 reason ? reason : "unknown");
        m_sta_is_connecting = false;
        return;
    }
    const auto provisioning_token = CaptureProvisioningSession();

    ESP_LOGI(BLUFI_TAG, "Starting WiFi connect from BluFi credentials: %s",
             reason ? reason : "unknown");
    m_wifi_connect_task_started = true;
    SsidManager::GetInstance().AddSsid(ssid, password);
    m_scan_should_save_ssid = false;

    m_sta_ssid_len = static_cast<int>(std::min(ssid.size(), sizeof(m_sta_ssid)));
    memcpy(m_sta_ssid, ssid.c_str(), m_sta_ssid_len);
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    m_sta_connected = false;
    m_sta_got_ip = false;
    m_sta_is_connecting = true;
    m_sta_conn_info = {};
    m_sta_conn_info.sta_ssid = m_sta_ssid;
    m_sta_conn_info.sta_ssid_len = m_sta_ssid_len;

    auto& wifi_manager = WifiManager::GetInstance();

    if (wifi_manager.IsInitialized()) {
        if (wifi_manager.IsConfigMode()) {
            wifi_manager.StopConfigAp();
        }
        wifi_manager.StopStation();
    }

    if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
        ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager");
        m_wifi_connect_task_started = false;
        m_sta_is_connecting = false;
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_manager.StartStation();

    auto* task_ctx = new (std::nothrow) WifiConnectTaskContext{
        this, provisioning_token};
    if (task_ctx == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate WiFi connect task context");
        m_wifi_connect_task_started = false;
        m_sta_is_connecting = false;
        return;
    }
    const BaseType_t created = xTaskCreate(
        [](void* raw_ctx) {
            auto* ctx = static_cast<WifiConnectTaskContext*>(raw_ctx);
            auto* self = ctx->self;
            const auto provisioning_token = ctx->provisioning_token;
            delete ctx;
            auto& wifi = WifiManager::GetInstance();
            constexpr int kConnectTimeoutMs = 60000;
            constexpr TickType_t kDelayTick = pdMS_TO_TICKS(200);
            int waited_ms = 0;

            while (waited_ms < kConnectTimeoutMs && !wifi.IsConnected()) {
                vTaskDelay(kDelayTick);
                waited_ms += 200;
            }

            wifi_mode_t mode = GetWifiModeWithFallback(wifi);
            const int softap_conn_num = _get_softap_conn_num();

            if (wifi.IsConnected()) {
                self->m_sta_is_connecting = false;
                self->m_sta_connected = true;
                self->m_sta_got_ip = true;
                self->m_provisioned = true;

                auto current_ssid = wifi.GetSsid();
                if (!current_ssid.empty()) {
                    self->m_sta_ssid_len = static_cast<int>(
                        std::min(current_ssid.size(), sizeof(self->m_sta_ssid)));
                    memcpy(self->m_sta_ssid, current_ssid.c_str(), self->m_sta_ssid_len);
                }

                wifi_ap_record_t ap_info{};
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    memcpy(self->m_sta_bssid, ap_info.bssid, sizeof(self->m_sta_bssid));
                }

                esp_blufi_extra_info_t info = {};
                memcpy(info.sta_bssid, self->m_sta_bssid, sizeof(self->m_sta_bssid));
                info.sta_bssid_set = true;
                info.sta_ssid = self->m_sta_ssid;
                info.sta_ssid_len = self->m_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS,
                                                softap_conn_num, &info);
                ESP_LOGI(BLUFI_TAG, "connected to WiFi");

                if (self->m_ble_is_connected) {
                    esp_blufi_disconnect();
                }

                // BluFi has delivered the Wi-Fi credentials and the attempt
                // bootstrap token by this point. Keep the connection report
                // above for the phone, then free the BLE stack before the claim
                // poll performs HTTPS/TLS on ESP32-S3. Real hardware otherwise
                // fails inside mbedTLS AES allocation while BLE remains active.
                Application::GetInstance().Schedule([self, provisioning_token]() {
                    if (!self->CompleteSuccessfulProvisioningTeardown(
                            "wifi_credentials_connected", provisioning_token)) {
                        return;
                    }
                    self->TryReportProvisioningAuthenticated("wifi_success_after_ble_teardown");
                    Application::GetInstance().SchedulePendingTbotClaimRefresh();
                });
            } else {
                self->m_wifi_connect_task_started = false;
                self->m_sta_is_connecting = false;
                self->m_sta_connected = false;
                self->m_sta_got_ip = false;

                esp_blufi_extra_info_t info = {};
                info.sta_ssid = self->m_sta_ssid;
                info.sta_ssid_len = self->m_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                                softap_conn_num, &info);
                ESP_LOGE(BLUFI_TAG, "Failed to connect to WiFi via esp-wifi-connect");
#ifdef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED
                {
                    const std::string& token = self->bootstrap_token_;
                    const std::string& code  = self->provisioning_code_;
                    if (!token.empty()) {
                        ESP_LOGI(BLUFI_TAG, "Reporting provisioning status: failed");
                        // Mirror the success lane (~706-714): clear the
                        // temporary secrets ONLY if the backend report
                        // succeeded. ADR-0018 requires the firmware to keep the
                        // secrets for retry when the report fails
                        // (provisioning_code_ has no NVS backup).
                        bool ok = ProvisioningStatusReporter::Report(
                            ProvisioningStatusReporter::Status::Failed,
                            token, code, "wifi_connect_failed");
                        if (ok) {
                            self->ClearProvisioningSecrets();
                        } else {
                            ESP_LOGW(BLUFI_TAG,
                                     "failed-status report failed; secrets retained for retry");
                        }
                        // Deliberate difference vs the success lane: do NOT
                        // tear down BLE here. On wifi_connect_failed the phone
                        // must retry Wi-Fi over the SAME live BLE session, so
                        // the BLE stack stays up.
                    }
                }
#endif
            }
            vTaskDelete(nullptr);
        },
        "blufi_wifi_conn", 4096, task_ctx, 5, nullptr);
    if (created != pdPASS) {
        delete task_ctx;
        m_wifi_connect_task_started = false;
        m_sta_is_connecting = false;
        ESP_LOGE(BLUFI_TAG, "Failed to create WiFi connect task");
    }
}

void Blufi::ScheduleStationConnectFallback() {
    xTaskCreate(
        [](void* ctx) {
            auto* self = static_cast<Blufi*>(ctx);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (!self->m_sta_is_connecting || self->m_wifi_connect_task_started ||
                self->m_sta_config.sta.ssid[0] == '\0') {
                vTaskDelete(nullptr);
                return;
            }
            ESP_LOGW(BLUFI_TAG,
                     "CONNECT_TO_AP not observed after password; starting WiFi fallback");
            self->StartStationConnectFromCredentials("password_fallback");
            vTaskDelete(nullptr);
        },
        "blufi_conn_fb", 3072, this, 5, nullptr);
}

bool Blufi::start_wifi_scan() {
    ESP_LOGI(BLUFI_TAG, "Starting dedicated WiFi scan");

    // Already running: caller can rely on the in-flight scan and await its done event.
    if (m_scan_in_progress) {
        ESP_LOGW(BLUFI_TAG, "Scan already in progress, skipping");
        return true;
    }

    m_scan_in_progress = true;

    if (!EnsureWifiScanEventHandlerRegistered()) {
        m_scan_in_progress = false;
        return false;
    }

    // Get current WiFi mode
    wifi_mode_t current_mode;
    esp_err_t err = esp_wifi_get_mode(&current_mode);

    if (current_mode == WIFI_MODE_AP) {
        // If in AP mode, temporarily switch to APSTA to allow scanning
        ESP_LOGI(BLUFI_TAG, "WiFi in AP mode");
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to set WiFi mode to STA: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
        // Need to restart WiFi for mode change to take effect
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi after mode switch: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
        // Start scan
        err = esp_wifi_scan_start(NULL, false);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi scan: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
    } else if (current_mode == WIFI_MODE_STA || current_mode == WIFI_MODE_APSTA) {
        // Ensure WiFi driver is started (may have been stopped during config mode transition)
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi before scan: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
        err = esp_wifi_scan_start(NULL, false);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi scan: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
    } else {
        ESP_LOGE(BLUFI_TAG, "Unexpected WiFi mode: %d", current_mode);
        m_scan_in_progress = false;
        return false;
    }

    ESP_LOGI(BLUFI_TAG, "WiFi scan started");
    return true;
}

void Blufi::_send_wifi_list() {
    if (m_ap_records.empty()) {
        ESP_LOGW(BLUFI_TAG, "No AP records available, sending WiFi scan fail");
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %u scanned APs", static_cast<unsigned>(m_ap_records.size()));

    std::vector<wifi_ap_record_t> sorted_ap_records = m_ap_records;
    std::stable_sort(sorted_ap_records.begin(), sorted_ap_records.end(),
                     [](const wifi_ap_record_t& lhs, const wifi_ap_record_t& rhs) {
                         return lhs.rssi > rhs.rssi;
                     });

    std::vector<esp_blufi_ap_record_t> blufi_ap_list;
    std::vector<std::string> seen_ssids;
    for (const auto& ap : sorted_ap_records) {
        size_t ssid_len = 0;
        while (ssid_len < sizeof(ap.ssid) && ap.ssid[ssid_len] != 0) {
            ++ssid_len;
        }
        std::string ssid(reinterpret_cast<const char*>(ap.ssid), ssid_len);
        if (ssid.empty() || std::find(seen_ssids.begin(), seen_ssids.end(), ssid) != seen_ssids.end()) {
            continue;
        }
        seen_ssids.push_back(ssid);

        esp_blufi_ap_record_t blufi_ap;
        memset(&blufi_ap, 0, sizeof(blufi_ap));
        memcpy(blufi_ap.ssid, ap.ssid, std::min((size_t)32, sizeof(ap.ssid)));
        blufi_ap.rssi = ap.rssi;
        blufi_ap_list.push_back(blufi_ap);
    }

    if (blufi_ap_list.size() > kMaxBlufiWifiListApRecords) {
        blufi_ap_list.resize(kMaxBlufiWifiListApRecords);
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %u APs after cap/dedupe", static_cast<unsigned>(blufi_ap_list.size()));

    esp_blufi_send_wifi_list(blufi_ap_list.size(), blufi_ap_list.data());

    m_ap_records.clear();
    m_ap_records_updated_us = 0;
}

void Blufi::_wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                     void* event_data) {
    Blufi* self = static_cast<Blufi*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (!self->m_scan_in_progress) {
            ESP_LOGI(BLUFI_TAG, "Ignoring WiFi scan done event not owned by BluFi");
            return;
        }

        ESP_LOGI(BLUFI_TAG, "WiFi scan done");

        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);

        if (ap_num == 0) {
            ESP_LOGW(BLUFI_TAG, "No APs found");
            self->m_ap_records.clear();
            self->m_ap_records_updated_us = 0;
        } else {
            if (self->m_scan_should_save_ssid) {
                self->m_ap_records.resize(ap_num);
                esp_err_t err = esp_wifi_scan_get_ap_records(&ap_num, self->m_ap_records.data());
                if (err != ESP_OK) {
                    ESP_LOGE(BLUFI_TAG, "Failed to read WiFi scan records: %s", esp_err_to_name(err));
                    self->m_ap_records.clear();
                    self->m_ap_records_updated_us = 0;
                } else {
                    self->m_ap_records.resize(ap_num);
                    self->m_ap_records_updated_us = esp_timer_get_time();

                    ESP_LOGI(BLUFI_TAG, "Found %d APs", ap_num);
                }
            }
        }
        self->m_scan_in_progress = false;
        // Dispatch a pending GET_WIFI_LIST response if one is waiting on this scan.
        if (self->m_send_list_after_scan) {
            self->m_send_list_after_scan = false;
            self->_send_wifi_list();
        }
    }
}

void Blufi::_handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI init finish");
            {
                static const std::string device_name = GetBlufiDeviceName();
                ESP_LOGI(BLUFI_TAG, "BLUFI advertised name: %s", device_name.c_str());
                StartTbotBlufiAdvertising(device_name.c_str());
            }
            break;
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI deinit finish");
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT:
            ESP_LOGI(BLUFI_TAG, "BLUFI ble connect");
            m_ble_is_connected = true;
            // A successful client connect proves re-advertising still works, so
            // clear the re-advertise cap. This makes the cap count CONSECUTIVE
            // failed auto-readvertises (a flapping peer that never connects),
            // not legitimate disconnect+reconnect sessions in one setup window —
            // otherwise a user whose phone reconnects a few times trips the cap
            // and is left with BLE down until the next explicit BOOT entry.
            ble_readvertise_count_ = 0;
            esp_blufi_adv_stop();
            _security_init();
            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            ESP_LOGI(BLUFI_TAG, "BLUFI ble disconnect");
            m_ble_is_connected = false;
            _security_deinit();
            if (!m_provisioned) {
                // Only restart advertising if the hard-timeout has NOT fired.
                // If ble_timed_out_ is true the timer callback has already
                // posted (or is about to post) a deinit to the Application
                // task — restarting advertising here would be unsafe.
                if (ble_timed_out_) {
                    ESP_LOGW(BLUFI_TAG, "BLE disconnect after timeout — NOT restarting advertising");
                } else if (ble_readvertise_count_ >= kMaxBleReadvertiseAttempts) {
                    // C8: re-advertise cap reached. A flapping peer must not be
                    // able to tight-loop restart advertising; stop here and let
                    // the 300s hard-timeout tear the stack down. BLE stays down
                    // until the next explicit setup entry (init() resets count).
                    ESP_LOGW(BLUFI_TAG,
                             "BLE re-advertise cap reached (%d) — NOT restarting advertising",
                             kMaxBleReadvertiseAttempts);
                } else {
                    ++ble_readvertise_count_;
                    ESP_LOGI(BLUFI_TAG, "BLE re-advertise %d/%d after disconnect",
                             ble_readvertise_count_, kMaxBleReadvertiseAttempts);
                    StartTbotBlufiAdvertising(GetBlufiDeviceName().c_str());
                }
            } else {
                esp_blufi_adv_stop();
                if (!m_deinited) {
                    auto* self = this;
                    const auto provisioning_token = CaptureProvisioningSession();
                    Application::GetInstance().Schedule([self, provisioning_token]() {
                        self->CompleteSuccessfulProvisioningTeardown(
                            "provisioned_ble_disconnect", provisioning_token);
                    });
                }
            }
            break;
        case ESP_BLUFI_EVENT_SET_WIFI_OPMODE: {
            ESP_LOGI(BLUFI_TAG, "BLUFI Set WIFI opmode %d", param->wifi_mode.op_mode);
            auto& wifi_manager = WifiManager::GetInstance();
            if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
                ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager for opmode change");
                break;
            }
            switch (param->wifi_mode.op_mode) {
                case WIFI_MODE_STA:
                    wifi_manager.StartStation();
                    break;
                case WIFI_MODE_AP:
                    wifi_manager.StartConfigAp();
                    break;
                case WIFI_MODE_APSTA:
                    ESP_LOGW(BLUFI_TAG, "APSTA mode not supported, starting station only");
                    wifi_manager.StartStation();
                    break;
                default:
                    wifi_manager.StopStation();
                    wifi_manager.StopConfigAp();
                    break;
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
            ESP_LOGI(BLUFI_TAG, "BLUFI request wifi connect to AP via esp-wifi-connect");
            StartStationConnectFromCredentials("blufi_connect_request");
            break;
        }
        case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
            ESP_LOGI(BLUFI_TAG, "BLUFI request wifi disconnect from AP");
            if (WifiManager::GetInstance().IsInitialized()) {
                WifiManager::GetInstance().StopStation();
            }
            m_sta_is_connecting = false;
            m_wifi_connect_task_started = false;
            m_sta_connected = false;
            m_sta_got_ip = false;
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
            auto& wifi = WifiManager::GetInstance();
            wifi_mode_t mode = GetWifiModeWithFallback(wifi);
            const int softap_conn_num = _get_softap_conn_num();

            if (wifi.IsInitialized() && wifi.IsConnected()) {
                m_sta_connected = true;
                m_sta_got_ip = true;

                auto current_ssid = wifi.GetSsid();
                if (!current_ssid.empty()) {
                    m_sta_ssid_len =
                        static_cast<int>(std::min(current_ssid.size(), sizeof(m_sta_ssid)));
                    memcpy(m_sta_ssid, current_ssid.c_str(), m_sta_ssid_len);
                }

                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, m_sta_bssid, 6);
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_conn_num,
                                                &info);
            } else if (m_sta_is_connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_conn_num,
                                                &m_sta_conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_conn_num,
                                                &m_sta_conn_info);
            }
            ESP_LOGI(BLUFI_TAG, "BLUFI get wifi status");
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_BSSID:
            memcpy(m_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
            m_sta_config.sta.bssid_set = true;
            ESP_LOGI(BLUFI_TAG, "Recv STA BSSID");
            break;
        case ESP_BLUFI_EVENT_RECV_STA_SSID: {
            if (!_require_secure_session_for_credentials()) {
                break;
            }
            // Bound the copy: a NUL written at [len] overflows when the frame
            // reports len >= sizeof(buffer). Clamp to sizeof()-1.
            size_t ssid_n = std::min<size_t>(param->sta_ssid.ssid_len,
                                             sizeof(m_sta_config.sta.ssid) - 1);
            memcpy(m_sta_config.sta.ssid, param->sta_ssid.ssid, ssid_n);
            m_sta_config.sta.ssid[ssid_n] = '\0';
            m_sta_is_connecting = true;
            m_wifi_connect_task_started = false;
            // Do NOT log the SSID value: the home network name is user PII and a
            // serial/log dump would leak it. Mirror the RECV_STA_PASSWD handler
            // below, which deliberately logs no credential value.
            ESP_LOGI(BLUFI_TAG, "Recv STA SSID (len=%u)", static_cast<unsigned>(ssid_n));
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
            if (!_require_secure_session_for_credentials()) {
                break;
            }
            // Bound the copy as above. Never log the password value.
            size_t passwd_n = std::min<size_t>(param->sta_passwd.passwd_len,
                                               sizeof(m_sta_config.sta.password) - 1);
            memcpy(m_sta_config.sta.password, param->sta_passwd.passwd, passwd_n);
            m_sta_config.sta.password[passwd_n] = '\0';
            ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD");
            ScheduleStationConnectFallback();
            break;
        }
        case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
            ESP_LOGI(BLUFI_TAG, "BLUFI get wifi list");
            // Case 1: a scan is already in flight (init scan or refresh scan started by
            // the previous _send_wifi_list()). Defer the response to its done handler
            // instead of blocking the BluFi task.
            if (m_scan_in_progress) {
                m_scan_should_save_ssid = true;
                m_send_list_after_scan = true;
                break;
            }
            // Case 2: cache is populated and fresh. Respond immediately.
            if (!m_ap_records.empty() && IsWifiScanCacheFresh()) {
                _send_wifi_list();
                break;
            }
            // Case 3: no fresh cache (e.g. driver was stopped during a
            // config-mode transition, init scan never completed, or cache is
            // stale). Trigger a real scan and dispatch from the scan-done
            // handler. If the scan cannot start, return an error frame so the
            // App exits its wait state instead of timing out.
            m_ap_records.clear();
            m_ap_records_updated_us = 0;
            m_scan_should_save_ssid = true;
            m_send_list_after_scan = true;
            if (!start_wifi_scan()) {
                m_send_list_after_scan = false;
                esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
            }
            break;
        }
        case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA: {
            if (!_require_secure_session_for_credentials()) {
                break;
            }
            // Parse TLV stream: [tag(1) | len(1) | value(len)] ...
            // tag=0x01 → bootstrap_token (base64url ASCII, max 64 chars)
            // tag=0x02 → provisioning_code (max 16 chars)
            const uint8_t* data = param->custom_data.data;
            int data_len = static_cast<int>(param->custom_data.data_len);
            ESP_LOGI(BLUFI_TAG, "BLUFI recv custom data, len=%d", data_len);

            // Claim TLV payloads may arrive as token/code/device_id in a single
            // custom-data frame. Persist device_id before handling token/code so
            // TryReportProvisioningAuthenticated() can reliably identify claim
            // flow and avoid spending the single-use claim token on the legacy
            // provisioning-status endpoint.
            int prescan_offset = 0;
            while (prescan_offset + 2 <= data_len) {
                uint8_t tag = data[prescan_offset];
                uint8_t len = data[prescan_offset + 1];
                prescan_offset += 2;
                if (prescan_offset + static_cast<int>(len) > data_len) {
                    break;
                }
                if (tag == 0x03) {
                    uint8_t safe_len = (len > 64) ? 64 : len;
                    const char* value = reinterpret_cast<const char*>(data + prescan_offset);
                    Settings websocket_settings("websocket", true);
                    websocket_settings.SetString("claim_device_id", std::string(value, safe_len));
                    ESP_LOGI(BLUFI_TAG, "Received claim device_id (%u bytes)", safe_len);
                    break;
                }
                prescan_offset += static_cast<int>(len);
            }

            int offset = 0;
            while (offset + 2 <= data_len) {
                uint8_t tag = data[offset];
                uint8_t len = data[offset + 1];
                offset += 2;

                // Bounds check: ensure value bytes are within payload
                if (offset + static_cast<int>(len) > data_len) {
                    ESP_LOGW(BLUFI_TAG, "TLV truncated at tag=0x%02x, len=%u, remaining=%d",
                             tag, len, data_len - offset);
                    break;
                }

                const char* value = reinterpret_cast<const char*>(data + offset);
                offset += static_cast<int>(len);

                if (tag == 0x01) {
                    // Bootstrap token: cap at 64 chars, no null terminator expected
                    uint8_t safe_len = (len > 64) ? 64 : len;
                    bootstrap_token_.assign(value, safe_len);
                    Settings websocket_settings("websocket", true);
                    websocket_settings.SetString("bootstrap_token", bootstrap_token_);
                    if (WifiManager::GetInstance().IsConnected() && !m_sta_is_connecting) {
                        ScheduleClaimRefreshAfterTokenHandoff();
                    }
                    TryReportProvisioningAuthenticated("custom_data_token");
                    ESP_LOGI(BLUFI_TAG, "Received bootstrap token (%u bytes)", safe_len);
                } else if (tag == 0x02) {
                    // Provisioning code: cap at 16 chars
                    uint8_t safe_len = (len > 16) ? 16 : len;
                    provisioning_code_.assign(value, safe_len);
                    TryReportProvisioningAuthenticated("custom_data_code");
                    ESP_LOGI(BLUFI_TAG, "Received provisioning code (%u bytes)", safe_len);
                } else if (tag == 0x03) {
                    // Claim device_id: the backend's serial-keyed device id the
                    // phone resolved. The robot MUST claim/confirm under THIS id
                    // (see GetTbotClaimDeviceId in claim_confirmation_reporter) —
                    // its random Board UUID otherwise mismatches the phone's claim
                    // attempt and pairing hangs forever. UUIDs are 36 chars; cap
                    // at 64 defensively.
                    ESP_LOGD(BLUFI_TAG, "Claim device_id already persisted by custom-data prescan");
                } else {
                    ESP_LOGD(BLUFI_TAG, "Unknown TLV tag=0x%02x, len=%u, skipping", tag, len);
                }
            }
            break;
        }
        default:
            ESP_LOGW(BLUFI_TAG, "Unhandled event: %d", event);
            break;
    }
}

// ---------------------------------------------------------------------------
// BLE hard-timeout safety gate (#1)
// ---------------------------------------------------------------------------

void Blufi::_ble_setup_timeout_cb(void* arg) {
    // IMPORTANT: This callback runs in the esp_timer task context.
    // Calling deinit() here would race with BLE stack tasks and risk a WDT
    // crash (§9 of master plan). We only set the flag and post the teardown
    // to the Application task where it is safe.
    Blufi* self = static_cast<Blufi*>(arg);
    ESP_LOGW(BLUFI_TAG, "BLE setup TIMEOUT -> teardown posted to Application task");

    Application::GetInstance().Schedule([self]() {
        if (self->m_ble_is_connected || self->m_sta_is_connecting) {
            ESP_LOGW(BLUFI_TAG,
                     "BLE setup TIMEOUT deferred while phone session active: connected=%d sta_connecting=%d",
                     (int)self->m_ble_is_connected, (int)self->m_sta_is_connecting);
            self->StartBleSetupTimeout(30);
            return;
        }

        self->ble_timed_out_ = true;
        ESP_LOGW(BLUFI_TAG, "BLE setup TIMEOUT teardown executing on Application task");
        // Stop advertising before tearing down to minimise the window where
        // the radio is on but we are about to pull the stack.
        esp_blufi_adv_stop();
        self->deinit();
    });
}

void Blufi::StartBleSetupTimeout(int seconds) {
    std::lock_guard<std::mutex> lock(ble_setup_timer_mutex_);
    if (ble_setup_timer_ != nullptr) {
        // Already armed — stop and delete so we can re-create cleanly.
        esp_timer_stop(ble_setup_timer_);
        esp_timer_delete(ble_setup_timer_);
        ble_setup_timer_ = nullptr;
    }
    ble_timed_out_ = false;

    esp_timer_create_args_t args = {
        .callback = _ble_setup_timeout_cb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_setup_timer",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &ble_setup_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to create BLE setup timer: %s", esp_err_to_name(err));
        ble_setup_timer_ = nullptr;
        return;
    }
    err = esp_timer_start_once(ble_setup_timer_, static_cast<uint64_t>(seconds) * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to start BLE setup timer: %s", esp_err_to_name(err));
        esp_timer_delete(ble_setup_timer_);
        ble_setup_timer_ = nullptr;
        return;
    }
    ESP_LOGI(BLUFI_TAG, "BLE setup timer armed %ds", seconds);
}

void Blufi::CancelBleSetupTimeout() {
    std::lock_guard<std::mutex> lock(ble_setup_timer_mutex_);
    if (ble_setup_timer_ == nullptr) {
        return;  // Never armed or already cancelled — no-op.
    }
    esp_timer_stop(ble_setup_timer_);
    esp_timer_delete(ble_setup_timer_);
    ble_setup_timer_ = nullptr;
    ESP_LOGI(BLUFI_TAG, "BLE setup cancelled (provisioned)");
}

Blufi::BleState Blufi::GetBleState() const {
    if (transition_gate_.IsTransitionActive()) {
        return BleState::kOff;
    }
    if (ble_timed_out_) {
        return BleState::kTimeout;
    }
    if (!inited_ || m_deinited) {
        return BleState::kOff;
    }
    if (m_ble_is_connected) {
        return BleState::kConnected;
    }
    return BleState::kAdvertising;
}

const char* Blufi::GetBleStateString() const {
    switch (GetBleState()) {
        case BleState::kAdvertising:  return "advertising";
        case BleState::kConnected:    return "connected";
        case BleState::kTimeout:      return "timeout";
        case BleState::kOff:
        default:                      return "off";
    }
}

void Blufi::ClearProvisioningSecrets() {
    // Zeroize in-place before clearing (defense-in-depth)
    if (!bootstrap_token_.empty()) {
        std::fill(bootstrap_token_.begin(), bootstrap_token_.end(), '\0');
        bootstrap_token_.clear();
    }
    if (!provisioning_code_.empty()) {
        std::fill(provisioning_code_.begin(), provisioning_code_.end(), '\0');
        provisioning_code_.clear();
    }
    Settings websocket_settings("websocket", true);
    if (websocket_settings.GetString("claim_device_id").empty()) {
        // Legacy provisioning owns this token, so a successful provisioning-status
        // report may clear the at-rest copy. In claim flow the same token is
        // single-use auth for /claim/confirm; only the claim terminal path may
        // erase it.
        websocket_settings.EraseKey("bootstrap_token");
    } else {
        ESP_LOGI(BLUFI_TAG, "Claim flow active; preserving NVS bootstrap token for claim confirm");
    }
    ESP_LOGI(BLUFI_TAG, "Provisioning secrets cleared");
}

void Blufi::_event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    GetInstance()._handle_event(event, param);
}

void Blufi::_negotiate_data_handler_trampoline(uint8_t* data, int len, uint8_t** output_data,
                                               int* output_len, bool* need_free) {
    GetInstance()._dh_negotiate_data_handler(data, len, output_data, output_len, need_free);
}

int Blufi::_encrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_encrypt(iv8, crypt_data, crypt_len);
}

int Blufi::_decrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_decrypt(iv8, crypt_data, crypt_len);
}

uint16_t Blufi::_checksum_func_trampoline(uint8_t iv8, uint8_t* data, int len) {
    return _crc_checksum(iv8, data, len);
}
