#include "blufi.h"
#include "blufi_advertising_ledger.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
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
#include "esp_heap_caps.h"
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
static constexpr uint16_t kMaxBlufiWifiScanCandidates = 8;

enum class BleSessionPhase : uint64_t {
    kStopping = 0,
    kAccepting = 1,
    kConnected = 2,
    kDisconnected = 3,
};

static constexpr uint64_t kBleSessionPhaseMask = 0x3;

static uint64_t EncodeBleSessionState(uint32_t generation, BleSessionPhase phase) {
    return (static_cast<uint64_t>(generation) << 2) |
           static_cast<uint64_t>(phase);
}

static BleSessionPhase DecodeBleSessionPhase(uint64_t state) {
    return static_cast<BleSessionPhase>(state & kBleSessionPhaseMask);
}

static uint32_t DecodeBleSessionGeneration(uint64_t state) {
    return static_cast<uint32_t>(state >> 2);
}

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

static void LogBlufiHeapSnapshot(const char* phase) {
    ESP_LOGI(BLUFI_TAG,
             "heap phase=%s internal_free=%u internal_largest=%u "
             "internal_dma_free=%u internal_dma_largest=%u",
             phase,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
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
#ifdef CONFIG_BT_BLUEDROID_ENABLED
namespace {
using AdvertisingCallbackKind = TbotBlufiAdvertisingLedger::CallbackKind;
TbotBlufiAdvertisingLedger tbot_adv_ledger;

esp_ble_adv_params_t tbot_adv_params = {
    .adv_int_min = 0x100,
    .adv_int_max = 0x100,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

void InvalidateTbotBlufiAdvertising() {
    tbot_adv_ledger.Invalidate();
}

void ResetTbotBlufiAdvertisingAfterSuccessfulHostDeinit() {
    tbot_adv_ledger.ResetAfterSuccessfulHostDeinit();
}

bool ActivateTbotBlufiAdvertisingAfterSuccessfulHostInit() {
    return tbot_adv_ledger.ActivateAfterSuccessfulHostInit();
}

static void TbotBlufiGapEventHandler(esp_gap_ble_cb_event_t event,
                                     esp_ble_gap_cb_param_t* param) {
    if (param == nullptr) {
        return;
    }
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        tbot_adv_ledger.CompleteDefaultConfigAndSubmit(
            true, param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS,
            []() {
                return esp_ble_gap_start_advertising(&tbot_adv_params) == ESP_OK;
            });
        return;
    }

    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: {
            esp_err_t start_error = ESP_OK;
            const auto result = tbot_adv_ledger.CompleteCompactConfigAndSubmit(
                AdvertisingCallbackKind::kCompactAdvData, true,
                param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS,
                [&start_error]() {
                    start_error = esp_ble_gap_start_advertising(&tbot_adv_params);
                    return start_error == ESP_OK;
                },
                []() { esp_blufi_adv_start(); });
            if (result.fallback_started) {
                ESP_LOGW(BLUFI_TAG, "compact ADV configuration/start failed; using BluFi default");
            } else if (start_error != ESP_OK) {
                ESP_LOGW(BLUFI_TAG, "start compact advertising failed: %s",
                         esp_err_to_name(start_error));
            }
            break;
        }
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT: {
            const auto result = tbot_adv_ledger.CompleteCompactConfigAndSubmit(
                AdvertisingCallbackKind::kCompactScanResponse, true,
                param->scan_rsp_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS,
                []() { return esp_ble_gap_start_advertising(&tbot_adv_params) == ESP_OK; },
                []() { esp_blufi_adv_start(); });
            if (result.fallback_started) {
                ESP_LOGW(BLUFI_TAG, "compact scan response/start failed; using BluFi default");
            }
            break;
        }
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT: {
            const auto result = tbot_adv_ledger.CompleteStartAndMaybeFallback(
                true, param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS,
                []() { esp_blufi_adv_start(); });
            if (result.fallback_started) {
                ESP_LOGW(BLUFI_TAG, "compact advertising start failed; using BluFi default");
            } else if (result.default_failed) {
                ESP_LOGW(BLUFI_TAG, "BluFi default advertising start failed");
            } else if (result.compact_completed) {
                ESP_LOGI(BLUFI_TAG,
                         "TBOT compact ADV ready: UUID16 0xFFFF + name in primary ADV/RSP");
            }
            break;
        }
        default:
            esp_blufi_gap_event_handler(event, param);
            break;
    }
}
}  // namespace
#endif

#ifndef CONFIG_BT_BLUEDROID_ENABLED
static void InvalidateTbotBlufiAdvertising() {}
static void ResetTbotBlufiAdvertisingAfterSuccessfulHostDeinit() {}
static bool ActivateTbotBlufiAdvertisingAfterSuccessfulHostInit() { return true; }
#endif

static void StartTbotBlufiAdvertising(const char* device_name) {
#ifdef CONFIG_BT_BLUEDROID_ENABLED
    if (device_name != nullptr && device_name[0] != '\0') {
        esp_err_t name_err = esp_ble_gap_set_device_name(device_name);
        if (name_err != ESP_OK) {
            ESP_LOGW(BLUFI_TAG, "set device name failed: %s", esp_err_to_name(name_err));
        }
    }

    // Keep a shortened TBOT identity in the primary ADV because some Android
    // stacks do not merge the scan response into the discovery callback.
    uint8_t adv_raw[31] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0xFF, 0xFF,
    };
    size_t adv_len = 7;
    if (device_name != nullptr && device_name[0] != '\0') {
        const size_t short_name_len = std::min(
            std::strlen(device_name), sizeof(adv_raw) - adv_len - 2);
        adv_raw[adv_len++] = static_cast<uint8_t>(short_name_len + 1);
        adv_raw[adv_len++] = 0x08;  // Shortened Local Name
        std::memcpy(adv_raw + adv_len, device_name, short_name_len);
        adv_len += short_name_len;
    }

    uint8_t scan_rsp[31] = {};
    size_t rsp_len = 0;
    if (device_name != nullptr && device_name[0] != '\0') {
        size_t name_len = std::strlen(device_name);
        if (name_len > 29) {
            name_len = 29;
        }
        scan_rsp[0] = static_cast<uint8_t>(name_len + 1);
        scan_rsp[1] = name_len == std::strlen(device_name) ? 0x09 : 0x08;
        std::memcpy(scan_rsp + 2, device_name, name_len);
        rsp_len = name_len + 2;
    }

    const uint8_t pending = static_cast<uint8_t>(
        TbotBlufiAdvertisingLedger::kAdvDataPending |
        (rsp_len > 0 ? TbotBlufiAdvertisingLedger::kScanResponsePending : 0));
    esp_err_t adv_error = ESP_OK;
    esp_err_t scan_error = ESP_OK;
    const auto submission = tbot_adv_ledger.BeginCompactAndSubmit(
        pending,
        [&]() {
            adv_error = esp_ble_gap_config_adv_data_raw(adv_raw, adv_len);
            return adv_error == ESP_OK;
        },
        [&]() {
            scan_error = esp_ble_gap_config_scan_rsp_data_raw(scan_rsp, rsp_len);
            return scan_error == ESP_OK;
        },
        []() { esp_blufi_adv_start(); });
    if (!submission) {
        ESP_LOGW(BLUFI_TAG, "advertising callback ledger busy; waiting for host reset");
    } else if (adv_error != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "raw ADV configuration failed: %s", esp_err_to_name(adv_error));
    } else if (scan_error != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "raw scan RSP configuration failed: %s", esp_err_to_name(scan_error));
    }
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

static void SecureClearLocalString(std::string& value) {
    if (!value.empty()) {
        volatile char* bytes = &value[0];
        for (size_t i = 0; i < value.size(); ++i) {
            bytes[i] = '\0';
        }
    }
    value.clear();
}

struct BlufiCustomDataSnapshot {
    std::array<char, 64> device_id{};
    std::array<char, 64> token{};
    std::array<char, 16> code{};
    size_t device_id_len = 0;
    size_t token_len = 0;
    size_t code_len = 0;
    uint32_t expected_generation = 0;
};

static void SecureClearCustomDataSnapshot(BlufiCustomDataSnapshot& snapshot) {
    volatile char* device_id = snapshot.device_id.data();
    volatile char* token = snapshot.token.data();
    volatile char* code = snapshot.code.data();
    for (size_t i = 0; i < snapshot.device_id.size(); ++i) device_id[i] = '\0';
    for (size_t i = 0; i < snapshot.token.size(); ++i) token[i] = '\0';
    for (size_t i = 0; i < snapshot.code.size(); ++i) code[i] = '\0';
    snapshot.device_id_len = 0;
    snapshot.token_len = 0;
    snapshot.code_len = 0;
}

struct BlufiCustomDataContext {
    BlufiCustomDataSnapshot snapshot;

    void Clear() {
        SecureClearCustomDataSnapshot(snapshot);
    }

    ~BlufiCustomDataContext() {
        Clear();
    }

    void Retain() {
        references_.fetch_add(1, std::memory_order_relaxed);
    }

    void Release() {
        if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

private:
    std::atomic<size_t> references_{1};
};

// std::function may copy a scheduled closure. This handle keeps those copies
// pointer-only while the final owner securely destroys the shared context.
class BlufiCustomDataContextPtr {
public:
    explicit BlufiCustomDataContextPtr(BlufiCustomDataContext* context = nullptr)
        : context_(context) {}

    BlufiCustomDataContextPtr(const BlufiCustomDataContextPtr& other)
        : context_(other.context_) {
        Retain();
    }

    BlufiCustomDataContextPtr(BlufiCustomDataContextPtr&& other) noexcept
        : context_(other.context_) {
        other.context_ = nullptr;
    }

    BlufiCustomDataContextPtr& operator=(const BlufiCustomDataContextPtr& other) {
        if (this != &other) {
            auto* next = other.context_;
            if (next != nullptr) {
                next->Retain();
            }
            Release();
            context_ = next;
        }
        return *this;
    }

    BlufiCustomDataContextPtr& operator=(BlufiCustomDataContextPtr&& other) noexcept {
        if (this != &other) {
            Release();
            context_ = other.context_;
            other.context_ = nullptr;
        }
        return *this;
    }

    ~BlufiCustomDataContextPtr() {
        Release();
    }

    BlufiCustomDataContext* operator->() const {
        return context_;
    }

private:
    void Retain() {
        if (context_ != nullptr) {
            context_->Retain();
        }
    }

    void Release() {
        if (context_ != nullptr) {
            context_->Release();
            context_ = nullptr;
        }
    }

    BlufiCustomDataContext* context_ = nullptr;
};

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
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    return InitWithLifecycleOwned();
}

bool Blufi::EnsureAdvertisingForSetupGeneration(
        uint32_t expected_generation, int timeout_seconds,
        ProvisioningToken* provisioning_token,
        const std::function<esp_err_t()>& prepare,
        const std::function<void()>& on_current) {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> finalization_lock(provisioning_finalization_mutex_);
        if (expected_generation != setup_generation_.load()) {
            return false;
        }
    }

    BleState state = GetBleState();
    if (state == BleState::kOff) {
        if (prepare && prepare() != ESP_OK) {
            return false;
        }
        if (InitWithLifecycleOwned() != ESP_OK) {
            if (provisioning_token != nullptr && provisioning_token->valid()) {
                CompleteSuccessfulProvisioningTeardownWithLifecycleOwned(
                    "setup_abort", *provisioning_token);
            }
            return false;
        }
        state = GetBleState();
    }
    if (state == BleState::kOff) {
        return false;
    }
    if (!StartBleSetupTimeoutWithLifecycleOwned(timeout_seconds)) {
        if (provisioning_token != nullptr && provisioning_token->valid()) {
            CompleteSuccessfulProvisioningTeardownWithLifecycleOwned(
                "setup_abort", *provisioning_token);
        }
        return false;
    }
    if (on_current) {
        on_current();
    }
    return GetBleState() == BleState::kAdvertising ||
           GetBleState() == BleState::kConnected;
}

esp_err_t Blufi::InitWithLifecycleOwned() {
    if (teardown_failed_.load()) {
        ESP_LOGE(BLUFI_TAG, "BLE teardown previously failed; refusing blind reinitialization");
        return ESP_ERR_INVALID_STATE;
    }
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
    InvalidateTbotBlufiAdvertising();
    ble_session_state_.exchange(
        EncodeBleSessionState(setup_generation_.load(), BleSessionPhase::kStopping),
        std::memory_order_acq_rel);
    if (teardown_failed_.load()) {
        ESP_LOGE(BLUFI_TAG, "BLE teardown previously failed; refusing blind reinitialization");
        return ESP_ERR_INVALID_STATE;
    }

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
    m_wifi_connect_task_started.store(false);
    m_wifi_list_dispatch_epoch_.fetch_add(1, std::memory_order_acq_rel);
    m_wifi_list_dispatch_pending_epoch_.store(0, std::memory_order_release);

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
    teardown_failed_.store(false);
    inited_ = true;
    ble_session_state_.store(
        EncodeBleSessionState(setup_generation_.load(), BleSessionPhase::kAccepting),
        std::memory_order_release);
    return ESP_OK;
}

esp_err_t Blufi::deinit() {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    return DeinitWithLifecycleOwned();
}

esp_err_t Blufi::DeinitForSetupGeneration(
        uint32_t expected_generation, const std::function<void()>& on_current) {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> finalization_lock(provisioning_finalization_mutex_);
        if (expected_generation != setup_generation_.load()) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    CancelBleSetupTimeout();
    const esp_err_t result = DeinitWithLifecycleOwned();
    if (result == ESP_OK && on_current) {
        on_current();
    }
    return result;
}

esp_err_t Blufi::DeinitWithLifecycleOwned() {
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
    InvalidateTbotBlufiAdvertising();
    ble_session_state_.exchange(
        EncodeBleSessionState(setup_generation_.load(), BleSessionPhase::kStopping),
        std::memory_order_acq_rel);
    InvalidateWifiScanSession();
    esp_err_t first_error = ESP_OK;

    if (m_deinited && !host_active_ && !controller_active_) {
        return teardown_failed_.load() ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    if (scan_event_instance_ != nullptr &&
        wifi_scan_controller_.CanUnregisterHandler()) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              scan_event_instance_);
        scan_event_instance_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> session_lock(provisioning_finalization_mutex_);
        std::vector<wifi_ap_record_t>().swap(m_ap_records);
        m_ap_records_updated_us = 0;
        m_ap_records_owner_.reset();
    }
    m_wifi_list_dispatch_epoch_.fetch_add(1, std::memory_order_acq_rel);
    m_wifi_list_dispatch_pending_epoch_.store(0, std::memory_order_release);

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
    if (first_error != ESP_OK) {
        teardown_failed_.store(true);
    } else {
        teardown_failed_.store(false);
    }
    return first_error;
}

esp_err_t Blufi::RestartForSetup() {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    if (teardown_failed_.load()) {
        ESP_LOGE(BLUFI_TAG, "BLE teardown is in a failed state; reboot required before setup");
        return ESP_ERR_INVALID_STATE;
    }

    {
        std::lock_guard<std::mutex> initial_state_lock(provisioning_finalization_mutex_);
        // Invalidate completion workers from the prior BOOT generation before
        // touching the controller/host or shared station flags.
        ble_session_state_.exchange(
            EncodeBleSessionState(setup_generation_.load(), BleSessionPhase::kStopping),
            std::memory_order_acq_rel);
        setup_generation_.fetch_add(1);
        provisioning_report_owner_generation_.reset();
        const uint32_t stale_ssid_transaction = ssid_transaction_id_.exchange(0);
        SsidManager::GetInstance().RollbackSsidTransaction(stale_ssid_transaction);
        CancelBleSetupTimeout();
        InvalidateWifiScanSession();
    }

    if (GetBleState() != BleState::kOff) {
        esp_blufi_adv_stop();
        esp_err_t teardown_error = DeinitWithLifecycleOwned();
        if (teardown_error != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Fresh BOOT BLE teardown failed: %s",
                     esp_err_to_name(teardown_error));
            return teardown_error;
        }
        // Let Bluedroid finish queue/task teardown before constructing the next
        // host instance. Immediate deinit/init loops assert in vQueueDelete.
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    {
        std::lock_guard<std::mutex> reset_state_lock(provisioning_finalization_mutex_);
        _security_deinit();
        m_ble_is_connected = false;
        m_sta_connected = false;
        m_sta_got_ip = false;
        m_sta_is_connecting.store(false);
        m_wifi_connect_task_started.store(false);
        m_provisioned = false;
        m_wifi_list_dispatch_epoch_.fetch_add(1, std::memory_order_acq_rel);
        m_wifi_list_dispatch_pending_epoch_.store(0, std::memory_order_release);
        std::vector<wifi_ap_record_t>().swap(m_ap_records);
        m_ap_records_updated_us = 0;
        m_ap_records_owner_.reset();
        memset(&m_sta_config, 0, sizeof(m_sta_config));
        m_sta_config_ssid_len_ = 0;
        memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
        memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
        m_sta_ssid_len = 0;
        m_sta_conn_info = {};
        ClearProvisioningSecrets();
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = InitWithLifecycleOwned();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(BLUFI_TAG, "Fresh BOOT BLE init failed attempt %d/3: %s",
                 attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return err;
}

bool Blufi::RunIfSetupGenerationCurrent(uint32_t expected_generation,
                                        const std::function<void()>& action) {
    std::lock_guard<std::mutex> finalization_lock(provisioning_finalization_mutex_);
    if (expected_generation != setup_generation_.load()) {
        return false;
    }
    action();
    return true;
}

bool Blufi::RunWithSetupGenerationCurrent(uint32_t expected_generation,
                                          const std::function<void()>& action) {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> finalization_lock(provisioning_finalization_mutex_);
        if (expected_generation != setup_generation_.load()) {
            return false;
        }
    }
    action();
    return true;
}

bool Blufi::BindProvisioningSession(ProvisioningToken token) {
    return provisioning_session_.Bind(token);
}

Blufi::ProvisioningReservation Blufi::TryReserveProvisioningSession() {
    return provisioning_session_.TryReserve();
}

bool Blufi::ClearProvisioningSession(ProvisioningToken token) {
    return provisioning_session_.Clear(token);
}

Blufi::ProvisioningToken Blufi::CaptureProvisioningSession() const {
    return provisioning_session_.Capture();
}

bool Blufi::IsBleStackFullyOff() const {
    return !transition_gate_.IsTransitionActive() && !inited_ && !host_active_ &&
           !controller_active_;
}

bool Blufi::AbortProvisioningSetup(ProvisioningToken token) {
    return CompleteSuccessfulProvisioningTeardown("setup_abort", token);
}

bool Blufi::CompleteSuccessfulProvisioningTeardown(
        const char* reason, ProvisioningToken provisioning_token) {
    return CompleteSuccessfulProvisioningTeardownImpl(
        reason, provisioning_token, std::nullopt, {});
}

bool Blufi::CompleteSuccessfulProvisioningTeardownForGeneration(
        const char* reason, ProvisioningToken provisioning_token,
        uint32_t expected_generation, const std::function<void()>& on_current) {
    return CompleteSuccessfulProvisioningTeardownImpl(
        reason, provisioning_token, expected_generation, on_current);
}

bool Blufi::CompleteSuccessfulProvisioningTeardownImpl(
        const char* reason, ProvisioningToken provisioning_token,
        std::optional<uint32_t> expected_generation,
        const std::function<void()>& on_current) {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    if (expected_generation.has_value()) {
        std::lock_guard<std::mutex> generation_lock(provisioning_finalization_mutex_);
        if (expected_generation.value() != setup_generation_.load()) {
            return false;
        }
    }

    return CompleteSuccessfulProvisioningTeardownWithLifecycleOwned(
        reason, provisioning_token, on_current);
}

bool Blufi::CompleteSuccessfulProvisioningTeardownWithLifecycleOwned(
        const char* reason, ProvisioningToken provisioning_token,
        const std::function<void()>& on_current) {

    constexpr uint64_t kTokenChunkBase = 1000000000ULL;
    const uint64_t token_generation = provisioning_token.generation;
    const auto token_high = static_cast<unsigned long>(
        token_generation / (kTokenChunkBase * kTokenChunkBase));
    const auto token_middle = static_cast<unsigned long>(
        (token_generation / kTokenChunkBase) % kTokenChunkBase);
    const auto token_low = static_cast<unsigned long>(token_generation % kTokenChunkBase);
    auto completion = provisioning_session_.Claim(provisioning_token);
    if (!completion) {
        ESP_LOGW(BLUFI_TAG,
                 "Ignoring stale provisioning teardown: reason=%s token=%lu%09lu%09lu",
                 reason ? reason : "unknown",
                 token_high, token_middle, token_low);
        return false;
    }
    ESP_LOGI(BLUFI_TAG, "Successful provisioning teardown requested: reason=%s",
             reason ? reason : "unknown");
    CancelBleSetupTimeout();
    if (!IsBleStackFullyOff()) {
        const esp_err_t deinit_error = DeinitWithLifecycleOwned();
        if (deinit_error != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Successful provisioning teardown failed: reason=%s error=%s",
                     reason ? reason : "unknown", esp_err_to_name(deinit_error));
            return false;
        }
    }

    const bool rearmed = Application::GetInstance().GetAudioService().EndWifiProvisioningAndRearm(
        provisioning_token);
    if (!rearmed) {
        ESP_LOGW(BLUFI_TAG,
                 "Provisioning teardown did not rearm: reason=%s token=%lu%09lu%09lu",
                 reason ? reason : "unknown",
                 token_high, token_middle, token_low);
        return false;
    }
    if (!completion.ConsumeSuccess()) {
        ESP_LOGE(BLUFI_TAG,
                 "Provisioning completion ownership lost: reason=%s token=%lu%09lu%09lu",
                 reason ? reason : "unknown",
                 token_high, token_middle, token_low);
        return false;
    }
    if (on_current) {
        on_current();
    }
    ESP_LOGI(BLUFI_TAG, "Successful provisioning teardown complete: reason=%s rearmed=%d",
             reason ? reason : "unknown", static_cast<int>(rearmed));
    return true;
}

bool Blufi::WasProvisioningSuccessfullyCompleted(
        ProvisioningToken provisioning_token) const {
    return provisioning_session_.WasSuccessfullyCompleted(provisioning_token);
}

bool Blufi::ReleaseBleForStationAssociation(uint32_t expected_generation) {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> initial_state_lock(provisioning_finalization_mutex_);
        if (expected_generation != setup_generation_.load()) {
            return false;
        }
    }

    CancelBleSetupTimeout();
    esp_blufi_adv_stop();
    const esp_err_t teardown_error = DeinitWithLifecycleOwned();
    if (teardown_error != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to release BLE before WiFi association: %s",
                 esp_err_to_name(teardown_error));
        return false;
    }

    // Bluedroid releases its queues asynchronously after deinit returns.
    vTaskDelay(pdMS_TO_TICKS(300));
    std::lock_guard<std::mutex> post_teardown_lock(provisioning_finalization_mutex_);
    return expected_generation == setup_generation_.load() && IsBleStackFullyOff();
}

void Blufi::RestoreBleAfterStationFailure(uint32_t expected_generation) {
    Application::GetInstance().Schedule([this, expected_generation]() {
        std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> finalization_lock(provisioning_finalization_mutex_);
            if (expected_generation != setup_generation_.load()) {
                return;
            }

            m_wifi_connect_task_started.store(false);
            m_sta_is_connecting.store(false);
            m_sta_connected = false;
            m_sta_got_ip = false;
            m_provisioned = false;
            m_ble_is_connected = false;
            setup_generation_.fetch_add(1);
            provisioning_report_owner_generation_.reset();
        }

        WifiManager::GetInstance().StopStation();
        if (InitWithLifecycleOwned() == ESP_OK) {
            StartBleSetupTimeoutWithLifecycleOwned(CONFIG_BLE_SETUP_TIMEOUT_SEC);
            ESP_LOGI(BLUFI_TAG, "WiFi association failed; BLE setup restored automatically");
        } else {
            ESP_LOGE(BLUFI_TAG, "WiFi association failed and BLE setup restore failed");
        }
    });
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
    ESP_LOGI(BLUFI_TAG, "Bluedroid host enabled");
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
        ResetTbotBlufiAdvertisingAfterSuccessfulHostDeinit();
    }
    host_active_ = profile_active_ || host_enabled_ || host_initialized_;
    return ESP_OK;
}

esp_err_t Blufi::_gap_register_callback() {
    esp_err_t rc = esp_ble_gap_register_callback(TbotBlufiGapEventHandler);
    if (rc) {
        return rc;
    }
    if (!ActivateTbotBlufiAdvertisingAfterSuccessfulHostInit()) {
        ESP_LOGE(BLUFI_TAG, "BLUFI advertising ledger still owns stale callbacks before profile init");
        InvalidateTbotBlufiAdvertising();
        return ESP_ERR_INVALID_STATE;
    }
    rc = esp_blufi_profile_init();
    if (rc != ESP_OK) {
        InvalidateTbotBlufiAdvertising();
    }
    return rc;
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
            if (m_sec->dh_param_len <= 0 || m_sec->dh_param_len > 1024) {
                ESP_LOGE(BLUFI_TAG, "Invalid DH param length: %d", m_sec->dh_param_len);
                m_sec->dh_param_len = 0;
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }

            // A phone may open one encrypted session to scan APs and a second
            // one to provision. Release MPI/AES allocations retained by the
            // previous handshake before building the next session.
            m_blufi_security_negotiated = false;
            mbedtls_dhm_free(m_sec->dhm);
            mbedtls_dhm_init(m_sec->dhm);
            mbedtls_aes_free(m_sec->aes);
            mbedtls_aes_init(m_sec->aes);
            memset(m_sec->share_key, 0, sizeof(m_sec->share_key));
            memset(m_sec->psk, 0, sizeof(m_sec->psk));
            memset(m_sec->self_public_key, 0, sizeof(m_sec->self_public_key));
            memset(m_sec->iv, 0, sizeof(m_sec->iv));
            m_sec->share_len = 0;

            if (m_sec->dh_param) {
                free(m_sec->dh_param);
                m_sec->dh_param = nullptr;
            }
            m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH malloc failed");
                m_sec->dh_param_len = 0;
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
                return;
            }
            break;
        case 0x01: {
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH param not allocated");
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            if (len < m_sec->dh_param_len + 1) {
                ESP_LOGE(BLUFI_TAG, "DH param data truncated");
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
            if (dhm_len <= 0 || dhm_len > DH_SELF_PUB_KEY_LEN) {
                ESP_LOGE(BLUFI_TAG, "Unsupported DH public key length: %d", dhm_len);
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }

            LogBlufiHeapSnapshot("dh_before_make_public");
            ret = mbedtls_dhm_make_public(m_sec->dhm, dhm_len, m_sec->self_public_key,
                                          DH_SELF_PUB_KEY_LEN,
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
            LogBlufiHeapSnapshot("dh_before_public_key_return");
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

void Blufi::InvalidateWifiScanSession() {
    wifi_scan_controller_.InvalidateSession(
        setup_generation_.load(std::memory_order_acquire),
        ble_session_state_.load(std::memory_order_acquire),
        ble_connection_epoch_.load(std::memory_order_acquire));
    m_wifi_list_dispatch_pending_epoch_.store(0, std::memory_order_release);
}

bool Blufi::IsWifiScanCacheFresh() const {
    if (m_ap_records.empty() || m_ap_records_updated_us <= 0) {
        return false;
    }
    return esp_timer_get_time() - m_ap_records_updated_us <= kWifiScanCacheMaxAgeUs;
}

void Blufi::ScheduleClaimRefreshAfterTokenHandoff() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    struct ClaimRefreshDelayContext {
        Blufi* self;
        uint32_t generation;
        Blufi::ProvisioningToken provisioning_token;
    };
    auto* ctx = new (std::nothrow) ClaimRefreshDelayContext{
        this, setup_generation_.load(), CaptureProvisioningSession()};
    if (ctx == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate claim token delay context");
        return;
    }
    BaseType_t created = xTaskCreate(
        [](void* ctx) {
            auto* task_ctx = static_cast<ClaimRefreshDelayContext*>(ctx);
            auto* self = task_ctx->self;
            const uint32_t generation = task_ctx->generation;
            const auto provisioning_token = task_ctx->provisioning_token;
            delete task_ctx;
            vTaskDelay(pdMS_TO_TICKS(kClaimRefreshAfterTokenHandoffDelayMs));
            Application::GetInstance().Schedule([self, generation, provisioning_token]() {
                bool should_teardown = false;
                const bool current = self->RunIfSetupGenerationCurrent(generation, [self, &should_teardown]() {
                    if (self->m_sta_is_connecting.load()) {
                        ESP_LOGI(BLUFI_TAG,
                                 "Deferring claim refresh: WiFi credential handoff in progress");
                        return;
                    }
                    if (!WifiManager::GetInstance().IsConnected()) {
                        ESP_LOGI(BLUFI_TAG,
                                 "Skipping connected-WiFi claim refresh: WiFi is not connected");
                        return;
                    }
                    should_teardown = true;
                });
                if (!current || !should_teardown ||
                    !self->CompleteSuccessfulProvisioningTeardownForGeneration(
                        "connected_wifi_token_handoff", provisioning_token, generation)) {
                    return;
                }
                Application::GetInstance().SchedulePendingTbotClaimRefresh(generation);
            });
            vTaskDelete(nullptr);
        },
        "claim_token_delay", 4096, ctx, 5, nullptr);
    if (created != pdPASS) {
        delete ctx;
        ESP_LOGE(BLUFI_TAG, "Failed to create claim token delay task");
    }
#endif
}

void Blufi::TryReportProvisioningAuthenticated(const char* reason,
                                               uint32_t expected_generation) {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    (void)reason;
    (void)expected_generation;
    return;
#else
#ifndef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED
    (void)reason;
    (void)expected_generation;
    return;
#else
    std::unique_lock<std::mutex> report_lock(provisioning_finalization_mutex_);
    if (expected_generation != setup_generation_.load()) {
        return;
    }

    Settings websocket_settings("websocket", false);
    const bool zero_code_claim_flow =
        !websocket_settings.GetString("claim_device_id").empty() &&
        provisioning_code_.empty();
    if (zero_code_claim_flow) {
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

    const bool report_in_flight = provisioning_report_owner_generation_.has_value();
    if (!m_provisioned || !wifi_connected || token_empty || code_empty || report_in_flight) {
        ESP_LOGI(BLUFI_TAG,
                 "Reporting provisioning authenticated skipped: reason=%s wifi_provisioned=%d wifi_connected=%d token_empty=%d code_empty=%d in_flight=%d",
                 reason ? reason : "unknown", (int)m_provisioned, (int)wifi_connected,
                 (int)token_empty, (int)code_empty, (int)report_in_flight);
        return;
    }

    if (ble_state != BleState::kOff) {
        if (!m_sta_is_connecting.load()) {
            auto* self = this;
            const std::string reason_copy = reason ? reason : "unknown";
            const auto provisioning_token = CaptureProvisioningSession();
            Application::GetInstance().Schedule(
                [self, reason_copy, expected_generation, provisioning_token]() {
                    ESP_LOGI(
                        BLUFI_TAG,
                        "Provisioning authenticated report requested while BLE active; stopping BLE first");
                    self->CompleteSuccessfulProvisioningTeardownForGeneration(
                        "authenticated_report_ble_release", provisioning_token,
                        expected_generation);
                    self->TryReportProvisioningAuthenticated(
                        reason_copy.c_str(), expected_generation);
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
        uint32_t expected_generation;
    };

    auto* ctx = new (std::nothrow) ReportTaskContext{
        this, bootstrap_token_, provisioning_code_, expected_generation};
    if (ctx == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate provisioning authenticated report context");
        return;
    }

    provisioning_report_owner_generation_ = expected_generation;
    BaseType_t created = xTaskCreate(
        [](void* raw_ctx) {
            auto* ctx = static_cast<ReportTaskContext*>(raw_ctx);
            bool ok = ProvisioningStatusReporter::Report(
                ProvisioningStatusReporter::Status::DeviceAuthenticated,
                ctx->token, ctx->code);
            auto* self = ctx->self;
            const uint32_t expected_generation = ctx->expected_generation;
            std::fill(ctx->token.begin(), ctx->token.end(), '\0');
            std::fill(ctx->code.begin(), ctx->code.end(), '\0');
            delete ctx;

            Application::GetInstance().Schedule([self, ok, expected_generation]() {
                self->RunIfSetupGenerationCurrent(expected_generation, [self, ok, expected_generation]() {
                    if (!self->provisioning_report_owner_generation_.has_value() ||
                        self->provisioning_report_owner_generation_.value() != expected_generation) {
                        return;
                    }
                    self->provisioning_report_owner_generation_.reset();
                    if (ok) {
                        ESP_LOGI(BLUFI_TAG, "Provisioning report succeeded after BLE teardown");
                        self->ClearProvisioningSecrets();
                        Application::GetInstance().SchedulePendingTbotClaimRefresh(
                            expected_generation);
                    } else {
                        ESP_LOGW(BLUFI_TAG,
                                 "Provisioning report failed after BLE teardown; secrets retained for retry");
                    }
                });
            });
            vTaskDelete(nullptr);
        },
        "prov_auth_report", 6144, ctx, 5, nullptr);
    if (created != pdPASS) {
        if (provisioning_report_owner_generation_.has_value() &&
            provisioning_report_owner_generation_.value() == expected_generation) {
            provisioning_report_owner_generation_.reset();
        }
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
#endif
}

void Blufi::StartStationConnectFromCredentials(const char* reason) {
    bool expected_not_started = false;
    if (!m_wifi_connect_task_started.compare_exchange_strong(expected_not_started, true)) {
        ESP_LOGI(BLUFI_TAG, "WiFi connect already started; ignoring duplicate trigger: %s",
                 reason ? reason : "unknown");
        return;
    }

    // Bind this connection attempt before any blocking/yielding work. A BOOT
    // re-entry can advance setup_generation_ while the station settles below;
    // the old attempt must never adopt that newer generation when it resumes.
    const uint32_t generation = setup_generation_.load();
    const auto provisioning_token = CaptureProvisioningSession();

    std::string ssid(reinterpret_cast<const char*>(m_sta_config.sta.ssid),
                     m_sta_config_ssid_len_);
    std::string password(reinterpret_cast<const char*>(m_sta_config.sta.password));
    if (ssid.empty()) {
        ESP_LOGW(BLUFI_TAG, "Ignoring WiFi connect trigger with empty SSID: %s",
                 reason ? reason : "unknown");
        SecureClearLocalString(password);
        m_wifi_connect_task_started.store(false);
        m_sta_is_connecting.store(false);
        return;
    }
    ESP_LOGI(BLUFI_TAG, "Starting WiFi connect from BluFi credentials: %s",
             reason ? reason : "unknown");
    m_sta_ssid_len = static_cast<int>(std::min(ssid.size(), sizeof(m_sta_ssid)));
    memcpy(m_sta_ssid, ssid.data(), static_cast<size_t>(m_sta_ssid_len));

    auto& ssid_manager = SsidManager::GetInstance();
    const uint32_t ssid_transaction =
        ssid_manager.BeginSsidTransaction(ssid, password);
    SecureClearLocalString(ssid);
    SecureClearLocalString(password);
    if (ssid_transaction == 0) {
        ESP_LOGE(BLUFI_TAG, "Failed to stage WiFi credentials");
        m_wifi_connect_task_started.store(false);
        m_sta_is_connecting.store(false);
        return;
    }
    ssid_transaction_id_.store(ssid_transaction);
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    m_sta_connected = false;
    m_sta_got_ip = false;
    m_sta_is_connecting.store(true);
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
        ssid_manager.RollbackSsidTransaction(ssid_transaction);
        uint32_t expected_transaction = ssid_transaction;
        ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
        m_wifi_connect_task_started.store(false);
        m_sta_is_connecting.store(false);
        SendStationConnectFailureReport();
        return;
    }

    struct WifiConnectTaskContext {
        Blufi* self;
        uint32_t generation;
        uint32_t ssid_transaction;
        ProvisioningToken provisioning_token;
        std::array<uint8_t, 32> candidate_ssid;
        size_t candidate_ssid_len;
    };
    auto* ctx = new (std::nothrow) WifiConnectTaskContext{
        this, generation, ssid_transaction, provisioning_token, {},
        static_cast<size_t>(m_sta_ssid_len)};
    if (ctx == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate BluFi WiFi completion context");
        m_wifi_connect_task_started.store(false);
        m_sta_is_connecting.store(false);
        ssid_manager.RollbackSsidTransaction(ssid_transaction);
        uint32_t expected_transaction = ssid_transaction;
        ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
        wifi_manager.StopStation();
        SendStationConnectFailureReport();
        return;
    }
    memcpy(ctx->candidate_ssid.data(), m_sta_ssid, ctx->candidate_ssid_len);

    vTaskDelay(pdMS_TO_TICKS(500));

    if (generation != setup_generation_.load()) {
        ESP_LOGI(BLUFI_TAG, "Ignoring stale BluFi WiFi connect before station start");
        delete ctx;
        ssid_manager.RollbackSsidTransaction(ssid_transaction);
        uint32_t expected_transaction = ssid_transaction;
        ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
        return;
    }

    BaseType_t created = xTaskCreate(
        [](void* ctx) {
            auto* task_ctx = static_cast<WifiConnectTaskContext*>(ctx);
            auto* self = task_ctx->self;
            const uint32_t generation = task_ctx->generation;
            const uint32_t ssid_transaction = task_ctx->ssid_transaction;
            const auto provisioning_token = task_ctx->provisioning_token;
            const auto candidate_ssid = task_ctx->candidate_ssid;
            const size_t candidate_ssid_len = task_ctx->candidate_ssid_len;
            delete task_ctx;
            auto& wifi = WifiManager::GetInstance();

            if (!self->ReleaseBleForStationAssociation(generation)) {
                ESP_LOGE(BLUFI_TAG, "Unable to release BLE before WiFi association");
                SsidManager::GetInstance().RollbackSsidTransaction(ssid_transaction);
                uint32_t expected_transaction = ssid_transaction;
                self->ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
                if (generation == self->setup_generation_.load()) {
                    self->m_wifi_connect_task_started.store(false);
                    self->m_sta_is_connecting.store(false);
                    self->RestoreBleAfterStationFailure(generation);
                }
                vTaskDelete(nullptr);
                return;
            }

            if (generation != self->setup_generation_.load()) {
                SsidManager::GetInstance().RollbackSsidTransaction(ssid_transaction);
                uint32_t expected_transaction = ssid_transaction;
                self->ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
                vTaskDelete(nullptr);
                return;
            }

            wifi.StartStation();
            constexpr int kConnectTimeoutMs = 60000;
            constexpr TickType_t kDelayTick = pdMS_TO_TICKS(200);
            int waited_ms = 0;

            while (waited_ms < kConnectTimeoutMs && !wifi.IsConnected()) {
                vTaskDelay(kDelayTick);
                waited_ms += 200;
            }

            // Lock order is always Blufi finalization -> SsidManager. RestartForSetup
            // uses the same order, making generation ownership and transaction
            // resolution indivisible through the terminal BluFi report.
            std::unique_lock<std::mutex> finalization_lock(
                self->provisioning_finalization_mutex_);

            if (generation != self->setup_generation_.load()) {
                ESP_LOGI(BLUFI_TAG, "Ignoring stale BluFi WiFi completion worker");
                SsidManager::GetInstance().RollbackSsidTransaction(ssid_transaction);
                uint32_t expected_transaction = ssid_transaction;
                self->ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
                finalization_lock.unlock();
                vTaskDelete(nullptr);
                return;
            }

            bool connected_to_candidate = false;
            bool credentials_committed = false;
            if (wifi.IsConnected()) {
                const std::string connected_ssid = wifi.GetSsid();
                connected_to_candidate =
                    connected_ssid.size() == candidate_ssid_len &&
                    memcmp(connected_ssid.data(), candidate_ssid.data(),
                           candidate_ssid_len) == 0;
                if (!connected_to_candidate) {
                    ESP_LOGW(BLUFI_TAG,
                             "Connected WiFi does not match provisioning candidate; rejecting fallback");
                }
            }

            if (connected_to_candidate) {
                // Revalidate setup ownership immediately before transaction commit.
                if (generation != self->setup_generation_.load()) {
                    ESP_LOGI(BLUFI_TAG,
                             "Ignoring stale BluFi WiFi completion before credential commit");
                    SsidManager::GetInstance().RollbackSsidTransaction(ssid_transaction);
                    uint32_t expected_transaction = ssid_transaction;
                    self->ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
                    finalization_lock.unlock();
                    vTaskDelete(nullptr);
                    return;
                }
                credentials_committed =
                    SsidManager::GetInstance().CommitSsidTransaction(ssid_transaction);
            }

            if (!credentials_committed) {
                // Revalidate setup ownership immediately before transaction rollback.
                if (generation != self->setup_generation_.load()) {
                    ESP_LOGI(BLUFI_TAG,
                             "Ignoring stale BluFi WiFi completion before credential rollback");
                    SsidManager::GetInstance().RollbackSsidTransaction(ssid_transaction);
                    uint32_t expected_transaction = ssid_transaction;
                    self->ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
                    finalization_lock.unlock();
                    vTaskDelete(nullptr);
                    return;
                }
                SsidManager::GetInstance().RollbackSsidTransaction(ssid_transaction);
            }

            uint32_t expected_transaction = ssid_transaction;
            self->ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);

            // Fence resolved transaction before shared station or report mutation.
            if (generation != self->setup_generation_.load()) {
                ESP_LOGI(BLUFI_TAG,
                         "Ignoring stale BluFi WiFi completion after credential resolution");
                finalization_lock.unlock();
                vTaskDelete(nullptr);
                return;
            }

            if (credentials_committed) {
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

                self->m_sta_is_connecting.store(false);
                ESP_LOGI(BLUFI_TAG, "connected to WiFi");
                finalization_lock.unlock();

                // BLE was released before station association. Completing the
                // provisioning session here only consumes ownership and rearms
                // normal audio; deinit is idempotent when the stack is off.
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
                Application::GetInstance().Schedule([self, generation, provisioning_token]() {
                    std::unique_lock<std::mutex> continuation_lock(
                        self->provisioning_finalization_mutex_);
                    if (generation != self->setup_generation_.load()) {
                        ESP_LOGI(BLUFI_TAG,
                                 "Ignoring stale BluFi WiFi completion continuation");
                        continuation_lock.unlock();
                        return;
                    }
                    const bool code_based_provisioning =
                        !self->provisioning_code_.empty();
                    ESP_LOGI(BLUFI_TAG, "WiFi provisioned; stopping BLE before claim refresh");
                    continuation_lock.unlock();
                    const bool teardown_completed =
                        self->CompleteSuccessfulProvisioningTeardownForGeneration(
                            "wifi_credentials_connected", provisioning_token, generation);
                    const bool completion_recorded =
                        teardown_completed ||
                        self->WasProvisioningSuccessfullyCompleted(provisioning_token);
                    if (!completion_recorded) {
                        return;
                    }
                    self->provisioning_session_.AcknowledgeSuccessfullyCompleted(
                        provisioning_token);
                    if (code_based_provisioning) {
                        self->TryReportProvisioningAuthenticated(
                            "wifi_success_after_ble_teardown", generation);
                        return;
                    }
                    Application::GetInstance().SchedulePendingTbotClaimRefresh(generation);
                });
#else
                Application::GetInstance().Schedule([self, generation, provisioning_token]() {
                    std::unique_lock<std::mutex> continuation_lock(
                        self->provisioning_finalization_mutex_);
                    if (generation != self->setup_generation_.load()) {
                        return;
                    }
                    continuation_lock.unlock();
                    const bool teardown_completed =
                        self->CompleteSuccessfulProvisioningTeardownForGeneration(
                            "course_mode_wifi_credentials_connected", provisioning_token,
                            generation);
                    const bool completion_recorded =
                        teardown_completed ||
                        self->WasProvisioningSuccessfullyCompleted(provisioning_token);
                    if (!completion_recorded) {
                        return;
                    }
                    self->provisioning_session_.AcknowledgeSuccessfullyCompleted(
                        provisioning_token);
                    Application::GetInstance()
                        .PromoteCourseModeFromWifiConfigAfterProvisioning();
                });
#endif
            } else {
                if (wifi.IsConnected()) {
                    ESP_LOGE(BLUFI_TAG, "WiFi connected but credential persistence failed");
                    wifi.StopStation();
                }
                self->m_wifi_connect_task_started.store(false);
                self->m_sta_is_connecting.store(false);
                self->m_sta_connected = false;
                self->m_sta_got_ip = false;
                self->m_provisioned = false;
#if defined(CONFIG_TBOT_PROVISIONING_REPORT_ENABLED) && !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
                std::string failure_token = self->bootstrap_token_;
                std::string failure_code = self->provisioning_code_;
#endif
                finalization_lock.unlock();
                ESP_LOGE(BLUFI_TAG, "Failed to connect to WiFi via esp-wifi-connect");
#if defined(CONFIG_TBOT_PROVISIONING_REPORT_ENABLED) && !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
                {
                    const std::string& token = failure_token;
                    const std::string& code = failure_code;
                    if (!token.empty()) {
                        Settings websocket_settings("websocket", false);
                        if (websocket_settings.GetString("claim_device_id").empty()) {
                            ESP_LOGI(BLUFI_TAG, "Reporting provisioning status: failed");
                            ProvisioningStatusReporter::Report(
                                ProvisioningStatusReporter::Status::Failed,
                                token, code, "wifi_connect_failed");
                        } else {
                            ESP_LOGI(BLUFI_TAG,
                                     "Claim flow active; skipping legacy failed-status report");
                        }
                        ESP_LOGI(BLUFI_TAG, "Provisioning secrets retained for WiFi retry");
                    }
                }
                SecureClearLocalString(failure_token);
                SecureClearLocalString(failure_code);
#endif
                self->RestoreBleAfterStationFailure(generation);
            }
            vTaskDelete(nullptr);
        },
        "blufi_wifi_conn", 4096, ctx, 5, nullptr);
    if (created != pdPASS) {
        delete ctx;
        ESP_LOGE(BLUFI_TAG, "Failed to create BluFi WiFi completion task");
        m_wifi_connect_task_started.store(false);
        m_sta_is_connecting.store(false);
        ssid_manager.RollbackSsidTransaction(ssid_transaction);
        uint32_t expected_transaction = ssid_transaction;
        ssid_transaction_id_.compare_exchange_strong(expected_transaction, 0);
        wifi_manager.StopStation();

        wifi_mode_t mode = GetWifiModeWithFallback(wifi_manager);
        esp_blufi_extra_info_t info = {};
        info.sta_ssid = m_sta_ssid;
        info.sta_ssid_len = m_sta_ssid_len;
        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                        _get_softap_conn_num(), &info);
    }
}

void Blufi::SendStationConnectFailureReport() {
    auto& wifi_manager = WifiManager::GetInstance();
    wifi_mode_t mode = GetWifiModeWithFallback(wifi_manager);
    esp_blufi_extra_info_t info = {};
    info.sta_ssid = m_sta_ssid;
    info.sta_ssid_len = m_sta_ssid_len;
    esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                    _get_softap_conn_num(), &info);
}

void Blufi::ScheduleStationConnectFallback() {
    const uint32_t generation = setup_generation_.load();
    struct StationConnectFallbackContext {
        Blufi* self;
        uint32_t generation;
    };
    auto* ctx = new (std::nothrow) StationConnectFallbackContext{this, generation};
    if (ctx == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate password fallback context");
        Application::GetInstance().Schedule([this, generation]() {
            if (generation != setup_generation_.load()) {
                return;
            }
            StartStationConnectFromCredentials("password_fallback_task_create_failed");
        });
        return;
    }

    BaseType_t created = xTaskCreate(
        [](void* ctx) {
            auto* task_ctx = static_cast<StationConnectFallbackContext*>(ctx);
            auto* self = task_ctx->self;
            const uint32_t generation = task_ctx->generation;
            delete task_ctx;
            vTaskDelay(pdMS_TO_TICKS(500));
            if (generation != self->setup_generation_.load()) {
                ESP_LOGI(BLUFI_TAG, "Ignoring stale password fallback worker");
                vTaskDelete(nullptr);
                return;
            }
            if (!self->m_sta_is_connecting.load() || self->m_wifi_connect_task_started.load() ||
                self->m_sta_config_ssid_len_ == 0) {
                vTaskDelete(nullptr);
                return;
            }
            ESP_LOGW(BLUFI_TAG,
                     "CONNECT_TO_AP not observed after password; starting WiFi fallback");
            self->StartStationConnectFromCredentials("password_fallback");
            vTaskDelete(nullptr);
        },
        "blufi_conn_fb", 3072, ctx, 5, nullptr);
    if (created != pdPASS) {
        delete ctx;
        ESP_LOGE(BLUFI_TAG, "Failed to create password fallback task");
        Application::GetInstance().Schedule([this, generation]() {
            if (generation != setup_generation_.load()) {
                return;
            }
            StartStationConnectFromCredentials("password_fallback_task_create_failed");
        });
    }
}

void Blufi::RequestWifiListScan(bool save_results, bool send_list) {
    BlufiWifiScanController::Request request{
        .setup_generation = setup_generation_.load(std::memory_order_acquire),
        .ble_session_state = ble_session_state_.load(std::memory_order_acquire),
        .ble_connection_epoch = ble_connection_epoch_.load(std::memory_order_acquire),
        .save_results = save_results,
        .send_list = send_list,
    };
    const auto decision = wifi_scan_controller_.RequestScan(request);
    if (decision.rejected_stale) {
        ESP_LOGI(BLUFI_TAG, "Ignoring stale WiFi scan request");
        return;
    }
    if (decision.start_now) {
        StartOwnedWifiScan(decision.request_id);
    }
}

bool Blufi::StartOwnedWifiScan(uint64_t request_id) {
    ESP_LOGI(BLUFI_TAG, "Starting owned WiFi scan id=%llu",
             static_cast<unsigned long long>(request_id));

    auto commit_failure = [this, request_id]() {
        const auto claim = wifi_scan_controller_.ClaimStart(request_id);
        if (!claim.claimed) {
            return false;
        }
        const auto committed = wifi_scan_controller_.CommitStart(request_id, false);
        if (committed.send_failure) {
            ScheduleWifiScanFailure(committed.owner, "scan_start_failed");
        }
        if (committed.start_pending) {
            SchedulePendingWifiScan(committed.pending_request_id, committed.pending);
        }
        return false;
    };

    if (!EnsureWifiScanEventHandlerRegistered()) {
        return commit_failure();
    }

    auto& wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
        ESP_LOGE(BLUFI_TAG, "Failed to initialize WiFi manager for scan");
        return commit_failure();
    }

    wifi_scan_config_t scan_config{};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
    scan_config.scan_time.passive = WIFI_PASSIVE_SCAN_DEFAULT_TIME;

    // A failed mode read must not strand the phone with an empty list. The
    // manager has initialized the driver above, but a concurrent radio reset can
    // still make this read fail; treat that as a station-mode recovery case.
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&current_mode);
    if (err != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "Failed to read WiFi mode before scan: %s",
                 esp_err_to_name(err));
        current_mode = WIFI_MODE_NULL;
    }

    if (current_mode == WIFI_MODE_STA || current_mode == WIFI_MODE_APSTA) {
        // The driver may remain initialized after StopRadio(). Starting an
        // already-running driver is harmless and reported as WIFI_STATE.
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi before scan: %s",
                     esp_err_to_name(err));
            return commit_failure();
        }
    } else {
        // NULL/AP/unknown modes are not station-capable. Move to STA before the
        // passive scan; config-mode list dispatch stops the idle radio later.
        ESP_LOGI(BLUFI_TAG, "Switching WiFi to STA for scan (mode=%d)", current_mode);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to set WiFi mode to STA: %s", esp_err_to_name(err));
            return commit_failure();
        }
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi after mode switch: %s",
                     esp_err_to_name(err));
            return commit_failure();
        }
    }

    const auto claim = wifi_scan_controller_.ClaimStart(request_id);
    if (!claim.claimed) {
        ESP_LOGI(BLUFI_TAG, "Skipping stale WiFi scan submission id=%llu",
                 static_cast<unsigned long long>(request_id));
        return false;
    }
    const esp_err_t scan_error = esp_wifi_scan_start(&scan_config, false);
    const auto committed = wifi_scan_controller_.CommitStart(
        request_id, scan_error == ESP_OK);
    if (scan_error != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to start WiFi scan: %s",
                 esp_err_to_name(scan_error));
    }
    if (committed.send_failure) {
        ScheduleWifiScanFailure(committed.owner, "scan_start_failed");
    }
    if (committed.start_pending) {
        SchedulePendingWifiScan(committed.pending_request_id, committed.pending);
    }

    if (committed.accepted) {
        ESP_LOGI(BLUFI_TAG, "WiFi scan started");
    }
    return committed.accepted;
}

void Blufi::SchedulePendingWifiScan(
        uint64_t request_id,
        const BlufiWifiScanController::Request& request) {
    Application::GetInstance().Schedule([this, request_id, request]() {
        const uint32_t current_generation =
            setup_generation_.load(std::memory_order_acquire);
        const uint64_t current_session =
            ble_session_state_.load(std::memory_order_acquire);
        const uint64_t current_connection =
            ble_connection_epoch_.load(std::memory_order_acquire);
        if (request.setup_generation != current_generation ||
            request.ble_session_state != current_session ||
            request.ble_connection_epoch != current_connection) {
            wifi_scan_controller_.InvalidateSession(
                current_generation, current_session, current_connection);
            return;
        }
        StartOwnedWifiScan(request_id);
    });
}

void Blufi::ScheduleWifiScanFailure(
        const BlufiWifiScanController::Request& request,
        const char* reason) {
    const std::string failure_reason = reason ? reason : "unknown";
    Application::GetInstance().Schedule([this, request, failure_reason]() {
        bool failure_owner_is_current = false;
        {
            std::lock_guard<std::mutex> session_lock(
                provisioning_finalization_mutex_);
            const uint32_t current_generation =
                setup_generation_.load(std::memory_order_acquire);
            const uint64_t current_session =
                ble_session_state_.load(std::memory_order_acquire);
            const uint64_t current_connection =
                ble_connection_epoch_.load(std::memory_order_acquire);
            failure_owner_is_current =
                request.setup_generation == current_generation &&
                request.ble_session_state == current_session &&
                request.ble_connection_epoch == current_connection &&
                m_ble_is_connected &&
                DecodeBleSessionPhase(current_session) ==
                    BleSessionPhase::kConnected;
        }
        if (!failure_owner_is_current) {
            return;
        }
        ESP_LOGW(BLUFI_TAG, "WiFi scan fail reason=%s", failure_reason.c_str());
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
    });
}

void Blufi::_send_wifi_list(std::vector<wifi_ap_record_t> m_ap_records) {
    if (m_ap_records.empty()) {
        return;
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %u scanned APs", static_cast<unsigned>(m_ap_records.size()));

    std::array<esp_blufi_ap_record_t, kMaxBlufiWifiListApRecords> blufi_ap_list{};
    size_t blufi_ap_count = 0;

    // Select the strongest unique SSIDs in bounded passes. Avoid copying and
    // sorting the complete scan result: those transient heap allocations leave
    // too little contiguous internal SRAM for BluFi's BTC/GATT send buffers.
    while (blufi_ap_count < blufi_ap_list.size()) {
        const wifi_ap_record_t* strongest = nullptr;
        for (const auto& ap : m_ap_records) {
            size_t ssid_len = 0;
            while (ssid_len < sizeof(ap.ssid) && ap.ssid[ssid_len] != 0) {
                ++ssid_len;
            }
            if (ssid_len == 0) {
                continue;
            }

            bool already_selected = false;
            for (size_t i = 0; i < blufi_ap_count; ++i) {
                const auto& selected = blufi_ap_list[i];
                size_t selected_len = 0;
                while (selected_len < sizeof(selected.ssid) && selected.ssid[selected_len] != 0) {
                    ++selected_len;
                }
                if (selected_len == ssid_len &&
                    memcmp(selected.ssid, ap.ssid, ssid_len) == 0) {
                    already_selected = true;
                    break;
                }
            }
            if (!already_selected && (strongest == nullptr || ap.rssi > strongest->rssi)) {
                strongest = &ap;
            }
        }

        if (strongest == nullptr) {
            break;
        }
        auto& selected = blufi_ap_list[blufi_ap_count++];
        memcpy(selected.ssid, strongest->ssid,
               std::min(sizeof(selected.ssid), sizeof(strongest->ssid)));
        selected.rssi = strongest->rssi;
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %u APs after cap/dedupe",
             static_cast<unsigned>(blufi_ap_count));

    // esp_blufi_send_wifi_list() performs two more internal allocations. Free
    // the driver-result cache first so those allocations see a contiguous heap.
    std::vector<wifi_ap_record_t>().swap(m_ap_records);

    // Config-mode scanning temporarily starts the Wi-Fi radio. Stop it before
    // BluFi allocates its BTC/GATT notification buffers; otherwise the list
    // response can exhaust the small internal DMA heap shared by Wi-Fi and
    // Bluetooth. Keep the driver initialized so a later BLE rescan only needs
    // esp_wifi_start(), not another large allocation while GATT is active.
    if (!WifiManager::GetInstance().IsConnected()) {
        if (!WifiManager::GetInstance().StopRadio()) {
            ESP_LOGW(BLUFI_TAG, "Failed to stop idle WiFi radio before list dispatch");
        }
    }
    LogBlufiHeapSnapshot("wifi_list_before_dispatch");
    esp_err_t err = esp_blufi_send_wifi_list(blufi_ap_count, blufi_ap_list.data());
    LogBlufiHeapSnapshot("wifi_list_after_dispatch");
    if (err != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to dispatch WiFi list: %s", esp_err_to_name(err));
    }
}

void Blufi::ScheduleWifiListSend(uint32_t expected_generation,
                                 uint64_t expected_ble_session_state,
                                 uint64_t expected_ble_connection_epoch,
                                 uint64_t expected_wifi_list_dispatch_epoch,
                                 std::vector<wifi_ap_record_t> ap_records) {
    Application::GetInstance().Schedule(
        [this, expected_generation, expected_ble_session_state,
         expected_ble_connection_epoch, expected_wifi_list_dispatch_epoch,
         ap_records = std::move(ap_records)]() mutable {
            RunIfSetupGenerationCurrent(
                expected_generation,
                [this, expected_ble_session_state, expected_ble_connection_epoch,
                 expected_wifi_list_dispatch_epoch, &ap_records]() {
                    const uint64_t current_ble_session_state =
                        ble_session_state_.load(std::memory_order_acquire);
                    const uint64_t current_ble_connection_epoch =
                        ble_connection_epoch_.load(std::memory_order_acquire);
                    const uint64_t current_wifi_list_dispatch_epoch =
                        m_wifi_list_dispatch_pending_epoch_.load(std::memory_order_acquire);
                    if (!m_ble_is_connected ||
                        current_ble_session_state != expected_ble_session_state ||
                        DecodeBleSessionPhase(expected_ble_session_state) !=
                            BleSessionPhase::kConnected ||
                        DecodeBleSessionPhase(current_ble_session_state) !=
                            BleSessionPhase::kConnected ||
                        current_ble_connection_epoch != expected_ble_connection_epoch ||
                        current_wifi_list_dispatch_epoch != expected_wifi_list_dispatch_epoch) {
                        return;
                    }
                    _send_wifi_list(std::move(ap_records));
                });
            uint64_t owned_dispatch_epoch = expected_wifi_list_dispatch_epoch;
            m_wifi_list_dispatch_pending_epoch_.compare_exchange_strong(
                owned_dispatch_epoch, 0,
                std::memory_order_acq_rel, std::memory_order_acquire);
        });
}

void Blufi::_wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                     void* event_data) {
    Blufi* self = static_cast<Blufi*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        const auto completion = self->wifi_scan_controller_.BeginCompletion(
            self->setup_generation_.load(std::memory_order_acquire),
            self->ble_session_state_.load(std::memory_order_acquire),
            self->ble_connection_epoch_.load(std::memory_order_acquire));
        if (!completion.owned_callback) {
            ESP_LOGI(BLUFI_TAG, "Ignoring WiFi scan done event not owned by BluFi");
            return;
        }

        ESP_LOGI(BLUFI_TAG, "WiFi scan done");

        uint16_t ap_num = 0;
        bool cache_scan_results = false;
        int64_t scan_results_updated_us = 0;
        std::vector<wifi_ap_record_t> scanned_ap_records;
        if (completion.discard_results) {
            esp_wifi_clear_ap_list();
        } else {
            esp_wifi_scan_get_ap_num(&ap_num);

            if (ap_num == 0) {
                ESP_LOGW(BLUFI_TAG, "No APs found");
                esp_wifi_clear_ap_list();
                cache_scan_results = completion.save_results;
            } else if (completion.save_results) {
                ap_num = std::min<uint16_t>(ap_num, kMaxBlufiWifiScanCandidates);
                scanned_ap_records.resize(ap_num);
                esp_err_t err = esp_wifi_scan_get_ap_records(
                    &ap_num, scanned_ap_records.data());
                if (err != ESP_OK) {
                    ESP_LOGE(BLUFI_TAG, "Failed to read WiFi scan records: %s", esp_err_to_name(err));
                    esp_wifi_clear_ap_list();
                    scanned_ap_records.clear();
                } else {
                    esp_wifi_clear_ap_list();
                    scanned_ap_records.resize(ap_num);
                    scan_results_updated_us = esp_timer_get_time();

                    ESP_LOGI(BLUFI_TAG, "Found %d APs", ap_num);
                }
                cache_scan_results = true;
            } else {
                esp_wifi_clear_ap_list();
            }
        }

        uint64_t expected_wifi_list_dispatch_epoch = 0;
        bool send_owned_wifi_list = false;
        bool send_owned_wifi_failure = false;
        std::vector<wifi_ap_record_t> owned_ap_records;
        if (cache_scan_results || completion.send_list) {
            std::lock_guard<std::mutex> session_lock(
                self->provisioning_finalization_mutex_);
            const bool completion_owner_is_current =
                completion.owner.setup_generation ==
                    self->setup_generation_.load(std::memory_order_acquire) &&
                completion.owner.ble_session_state ==
                    self->ble_session_state_.load(std::memory_order_acquire) &&
                completion.owner.ble_connection_epoch ==
                    self->ble_connection_epoch_.load(std::memory_order_acquire);
            if (cache_scan_results && completion_owner_is_current) {
                self->m_ap_records.swap(scanned_ap_records);
                self->m_ap_records_updated_us = scan_results_updated_us;
                self->m_ap_records_owner_ = completion.owner;
            }
            if (completion.send_list && completion_owner_is_current) {
                owned_ap_records.swap(self->m_ap_records);
                self->m_ap_records_updated_us = 0;
                self->m_ap_records_owner_.reset();
                if (owned_ap_records.empty()) {
                    send_owned_wifi_failure = true;
                } else {
                    expected_wifi_list_dispatch_epoch =
                        self->m_wifi_list_dispatch_epoch_.fetch_add(
                            1, std::memory_order_acq_rel) + 1;
                    self->m_wifi_list_dispatch_pending_epoch_.store(
                        expected_wifi_list_dispatch_epoch, std::memory_order_release);
                    send_owned_wifi_list = true;
                }
            }
        }

        const auto finished =
            self->wifi_scan_controller_.FinishCompletion(completion.request_id);
        if (send_owned_wifi_failure) {
            self->ScheduleWifiScanFailure(completion.owner,
                                          "scan_completed_without_ap_records");
        } else if (send_owned_wifi_list) {
            self->ScheduleWifiListSend(completion.owner.setup_generation,
                                       completion.owner.ble_session_state,
                                       completion.owner.ble_connection_epoch,
                                       expected_wifi_list_dispatch_epoch,
                                       std::move(owned_ap_records));
        }
        if (finished.start_pending) {
            self->SchedulePendingWifiScan(finished.request_id, finished.pending);
        }
    }
}

void Blufi::_handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI init finish");
            LogBlufiHeapSnapshot("blufi_init_finish");
            {
                static const std::string device_name = GetBlufiDeviceName();
                ESP_LOGI(BLUFI_TAG, "BLUFI advertising started");
                StartTbotBlufiAdvertising(device_name.c_str());
            }
            break;
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI deinit finish");
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT: {
            {
                std::lock_guard<std::mutex> session_lock(provisioning_finalization_mutex_);
                uint64_t expected_state = ble_session_state_.load(std::memory_order_acquire);
                if (DecodeBleSessionPhase(expected_state) != BleSessionPhase::kAccepting) {
                    ESP_LOGW(BLUFI_TAG, "Ignoring BLE connect while host is stopping");
                    break;
                }
                const uint32_t connection_generation =
                    DecodeBleSessionGeneration(expected_state);
                const uint64_t connected_state = EncodeBleSessionState(
                    connection_generation, BleSessionPhase::kConnected);
                if (!ble_session_state_.compare_exchange_strong(
                        expected_state, connected_state,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    ESP_LOGW(BLUFI_TAG, "Ignoring BLE connect invalidated by host transition");
                    break;
                }
                ble_connection_epoch_.fetch_add(1, std::memory_order_acq_rel);
                m_ble_is_connected = true;
            }
            ESP_LOGI(BLUFI_TAG, "BLUFI ble connect");
            LogBlufiHeapSnapshot("ble_connect");
            // A successful client connect proves re-advertising still works, so
            // clear the re-advertise cap. This makes the cap count CONSECUTIVE
            // failed auto-readvertises (a flapping peer that never connects),
            // not legitimate disconnect+reconnect sessions in one setup window —
            // otherwise a user whose phone reconnects a few times trips the cap
            // and is left with BLE down until the next explicit BOOT entry.
            ble_readvertise_count_ = 0;
            esp_blufi_adv_stop();
            // Claim credentials belong to one BLE connection. A later
            // credential-only Wi-Fi change sends no custom-data TLV, so retaining
            // a failed prior code would misclassify it as code-based provisioning
            // and spend an expired token on the legacy report endpoint.
            ClearProvisioningSecrets(false);
            _security_init();
            break;
        }
        case ESP_BLUFI_EVENT_BLE_DISCONNECT: {
            bool owns_session = false;
            uint32_t disconnected_generation = 0;
            {
                std::lock_guard<std::mutex> session_lock(provisioning_finalization_mutex_);
                uint64_t connected_state =
                    ble_session_state_.load(std::memory_order_acquire);
                if (DecodeBleSessionPhase(connected_state) == BleSessionPhase::kConnected) {
                    disconnected_generation = DecodeBleSessionGeneration(connected_state);
                    const uint64_t disconnected_state = EncodeBleSessionState(
                        disconnected_generation, BleSessionPhase::kDisconnected);
                    owns_session = ble_session_state_.compare_exchange_strong(
                        connected_state, disconnected_state,
                        std::memory_order_acq_rel, std::memory_order_acquire);
                }
                m_ble_is_connected = false;
                std::vector<wifi_ap_record_t>().swap(m_ap_records);
                m_ap_records_updated_us = 0;
                m_ap_records_owner_.reset();
                m_wifi_list_dispatch_epoch_.fetch_add(1, std::memory_order_acq_rel);
                m_wifi_list_dispatch_pending_epoch_.store(0, std::memory_order_release);
            }
            InvalidateWifiScanSession();
            ESP_LOGI(BLUFI_TAG, "BLUFI ble disconnect");
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
                    uint64_t expected_disconnected = EncodeBleSessionState(
                        disconnected_generation, BleSessionPhase::kDisconnected);
                    const uint64_t accepting_state = EncodeBleSessionState(
                        disconnected_generation, BleSessionPhase::kAccepting);
                    if (owns_session && ble_session_state_.compare_exchange_strong(
                            expected_disconnected, accepting_state,
                            std::memory_order_acq_rel, std::memory_order_acquire)) {
                        ++ble_readvertise_count_;
                        ESP_LOGI(BLUFI_TAG, "BLE re-advertise %d/%d after disconnect",
                                 ble_readvertise_count_, kMaxBleReadvertiseAttempts);
                        StartTbotBlufiAdvertising(GetBlufiDeviceName().c_str());
                    } else {
                        ESP_LOGI(BLUFI_TAG,
                                 "Skipping stale BLE disconnect re-advertise");
                    }
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
        }
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
            m_sta_is_connecting.store(false);
            m_wifi_connect_task_started.store(false);
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
            } else if (m_sta_is_connecting.load()) {
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
            // SSIDs are length-delimited and may legally occupy all 32 bytes.
            size_t ssid_n = std::min<size_t>(param->sta_ssid.ssid_len,
                                             sizeof(m_sta_config.sta.ssid));
            memset(m_sta_config.sta.ssid, 0, sizeof(m_sta_config.sta.ssid));
            memcpy(m_sta_config.sta.ssid, param->sta_ssid.ssid, ssid_n);
            m_sta_config_ssid_len_ = ssid_n;
            m_sta_is_connecting.store(true);
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
            if (m_wifi_list_dispatch_pending_epoch_.load(std::memory_order_acquire) != 0) {
                ESP_LOGI(BLUFI_TAG, "WiFi list dispatch already pending");
                break;
            }
            std::vector<wifi_ap_record_t> cached_ap_records;
            {
                std::lock_guard<std::mutex> session_lock(
                    provisioning_finalization_mutex_);
                const uint32_t current_generation =
                    setup_generation_.load(std::memory_order_acquire);
                const uint64_t current_session =
                    ble_session_state_.load(std::memory_order_acquire);
                const uint64_t current_connection =
                    ble_connection_epoch_.load(std::memory_order_acquire);
                const bool cache_owner_is_current =
                    m_ap_records_owner_.has_value() &&
                    m_ap_records_owner_->setup_generation == current_generation &&
                    m_ap_records_owner_->ble_session_state == current_session &&
                    m_ap_records_owner_->ble_connection_epoch == current_connection;
                if (cache_owner_is_current && !m_ap_records.empty() &&
                    IsWifiScanCacheFresh()) {
                    cached_ap_records.swap(m_ap_records);
                } else {
                    m_ap_records.clear();
                }
                m_ap_records_updated_us = 0;
                m_ap_records_owner_.reset();
            }
            // Sending can stop the Wi-Fi radio and allocate BLE buffers, so it
            // must stay outside the cache/session critical section.
            if (!cached_ap_records.empty()) {
                _send_wifi_list(std::move(cached_ap_records));
                break;
            }
            // No fresh cache: coalesce this logical request with any owned
            // physical scan and dispatch only from its completion callback.
            RequestWifiListScan(true, true);
            break;
        }
        case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA: {
            if (!_require_secure_session_for_credentials()) {
                break;
            }
            const uint64_t session_state =
                ble_session_state_.load(std::memory_order_acquire);
            if (DecodeBleSessionPhase(session_state) != BleSessionPhase::kConnected) {
                ESP_LOGW(BLUFI_TAG, "Ignoring custom data without an active BLE session");
                break;
            }
            const uint32_t session_generation =
                DecodeBleSessionGeneration(session_state);
            const uint8_t* data = param->custom_data.data;
            int data_len = static_cast<int>(param->custom_data.data_len);
            ESP_LOGI(BLUFI_TAG, "BLUFI recv custom data, len=%d", data_len);
            BlufiCustomDataSnapshot snapshot;
            snapshot.expected_generation = session_generation;
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
                    snapshot.token_len = std::min<size_t>(len, snapshot.token.size());
                    memcpy(snapshot.token.data(), value, snapshot.token_len);
                } else if (tag == 0x02) {
                    snapshot.code_len = std::min<size_t>(len, snapshot.code.size());
                    memcpy(snapshot.code.data(), value, snapshot.code_len);
                } else if (tag == 0x03) {
                    snapshot.device_id_len =
                        std::min<size_t>(len, snapshot.device_id.size());
                    memcpy(snapshot.device_id.data(), value, snapshot.device_id_len);
                } else {
                    ESP_LOGD(BLUFI_TAG, "Unknown TLV tag=0x%02x, len=%u, skipping", tag, len);
                }
            }

#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
            SecureClearCustomDataSnapshot(snapshot);
            if (!bootstrap_token_.empty()) {
                std::fill(bootstrap_token_.begin(), bootstrap_token_.end(), '\0');
                bootstrap_token_.clear();
            }
            if (!provisioning_code_.empty()) {
                std::fill(provisioning_code_.begin(), provisioning_code_.end(), '\0');
                provisioning_code_.clear();
            }
            ESP_LOGW(BLUFI_TAG, "Ignoring claim custom data in course-mode local endpoint");
            break;
#else
            auto* raw_context = new (std::nothrow) BlufiCustomDataContext;
            if (raw_context == nullptr) {
                ESP_LOGE(BLUFI_TAG, "Failed to allocate secure custom-data context");
                SecureClearCustomDataSnapshot(snapshot);
                break;
            }
            raw_context->snapshot = snapshot;
            SecureClearCustomDataSnapshot(snapshot);
            BlufiCustomDataContextPtr secure_context(raw_context);

            auto* self = this;
            Application::GetInstance().Schedule([self, secure_context]() mutable {
                auto& snapshot = secure_context->snapshot;
                const uint32_t generation = snapshot.expected_generation;
                const bool has_token = snapshot.token_len > 0;
                const bool has_code = snapshot.code_len > 0;
                const bool applied = self->RunIfSetupGenerationCurrent(
                    generation, [self, &snapshot, has_token, has_code]() {
                        Settings websocket_settings("websocket", true);
                        // Device identity must be visible before token/code side effects.
                        if (snapshot.device_id_len > 0) {
                            websocket_settings.SetString(
                                "claim_device_id",
                                std::string(snapshot.device_id.data(), snapshot.device_id_len));
                        }
                        if (has_code) {
                            self->provisioning_code_.assign(
                                snapshot.code.data(), snapshot.code_len);
                        }
                        if (has_token) {
                            self->bootstrap_token_.assign(
                                snapshot.token.data(), snapshot.token_len);
                            websocket_settings.SetString(
                                "bootstrap_token", self->bootstrap_token_);
                            if (WifiManager::GetInstance().IsConnected() &&
                                !self->m_sta_is_connecting.load() &&
                                self->provisioning_code_.empty()) {
                                self->ScheduleClaimRefreshAfterTokenHandoff();
                            }
                        }
                    });
                if (applied && snapshot.device_id_len > 0) {
                    ESP_LOGI(BLUFI_TAG, "Received claim device_id (%u bytes)",
                             static_cast<unsigned>(snapshot.device_id_len));
                }
                if (applied && has_token) {
                    ESP_LOGI(BLUFI_TAG, "Received bootstrap token (%u bytes)",
                             static_cast<unsigned>(snapshot.token_len));
                    self->TryReportProvisioningAuthenticated(
                        "custom_data_token", generation);
                }
                if (applied && has_code) {
                    ESP_LOGI(BLUFI_TAG, "Received provisioning code (%u bytes)",
                             static_cast<unsigned>(snapshot.code_len));
                    self->TryReportProvisioningAuthenticated(
                        "custom_data_code", generation);
                }
                secure_context->Clear();
            });
#endif
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
    Blufi* self = &Blufi::GetInstance();
    const uint32_t generation =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    ESP_LOGW(BLUFI_TAG, "BLE setup TIMEOUT -> teardown posted to Application task");

    Application::GetInstance().Schedule([self, generation]() {
        if (generation != self->ble_timeout_generation_.load()) {
            ESP_LOGI(BLUFI_TAG, "Ignoring stale BLE setup timeout");
            return;
        }
        if (self->m_ble_is_connected || self->m_sta_is_connecting.load()) {
            ESP_LOGW(BLUFI_TAG,
                     "BLE setup TIMEOUT deferred while phone session active: connected=%d sta_connecting=%d",
                     (int)self->m_ble_is_connected, (int)self->m_sta_is_connecting.load());
            self->StartBleSetupTimeout(30);
            return;
        }

        self->ble_timed_out_ = true;
        ESP_LOGW(BLUFI_TAG, "BLE setup TIMEOUT teardown executing on Application task");
        // Stop advertising before tearing down to minimise the window where
        // the radio is on but we are about to pull the stack.
        esp_blufi_adv_stop();
        self->CancelBleSetupTimeout();
        self->deinit();
    });
}

bool Blufi::StartBleSetupTimeoutWithLifecycleOwned(int seconds) {
    const uint32_t generation = ble_timeout_generation_.fetch_add(1) + 1;
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
        .arg = reinterpret_cast<void*>(static_cast<uintptr_t>(generation)),
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_setup_timer",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &ble_setup_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to create BLE setup timer: %s", esp_err_to_name(err));
        ble_setup_timer_ = nullptr;
        return false;
    }
    err = esp_timer_start_once(ble_setup_timer_, static_cast<uint64_t>(seconds) * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to start BLE setup timer: %s", esp_err_to_name(err));
        esp_timer_delete(ble_setup_timer_);
        ble_setup_timer_ = nullptr;
        return false;
    }
    ESP_LOGI(BLUFI_TAG, "BLE setup timer armed %ds", seconds);
    return true;
}

void Blufi::StartBleSetupTimeout(int seconds) {
    std::lock_guard<std::mutex> lifecycle_lock(ble_lifecycle_mutex_);
    StartBleSetupTimeoutWithLifecycleOwned(seconds);
}

void Blufi::CancelBleSetupTimeout() {
    // Also invalidates a callback that already fired and queued its Application
    // continuation before the esp_timer handle was cancelled.
    ble_timeout_generation_.fetch_add(1);
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

void Blufi::ClearProvisioningSecrets(bool preserve_claim_token) {
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    Settings websocket_settings("websocket", true);
    const bool zero_code_claim_flow =
        preserve_claim_token &&
        !websocket_settings.GetString("claim_device_id").empty() &&
        provisioning_code_.empty();
#endif
    // Zeroize in-place before clearing (defense-in-depth)
    if (!bootstrap_token_.empty()) {
        std::fill(bootstrap_token_.begin(), bootstrap_token_.end(), '\0');
        bootstrap_token_.clear();
    }
    if (!provisioning_code_.empty()) {
        std::fill(provisioning_code_.begin(), provisioning_code_.end(), '\0');
        provisioning_code_.clear();
    }
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    ESP_LOGI(BLUFI_TAG, "Course-mode provisioning secrets cleared from RAM");
    return;
#else
    if (!zero_code_claim_flow) {
        // Legacy provisioning owns this token, so a successful provisioning-status
        // report may clear the at-rest copy. In claim flow the same token is
        // single-use auth for /claim/confirm; only the claim terminal path may
        // erase it.
        websocket_settings.EraseKey("bootstrap_token");
    } else {
        ESP_LOGI(BLUFI_TAG, "Claim flow active; preserving NVS bootstrap token for claim confirm");
    }
    ESP_LOGI(BLUFI_TAG, "Provisioning secrets cleared");
#endif
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
