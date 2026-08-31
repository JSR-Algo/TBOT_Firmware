/*
 * WiFi Manager Implementation
 */

#include "wifi_manager.h"
#include "wifi_station.h"
#include "wifi_configuration_ap.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <nvs_flash.h>

#define TAG "WifiManager"

WifiManager& WifiManager::GetInstance() {
    // Scanner handlers and their coordinator may be referenced by queued
    // default-event-loop callbacks for the remainder of the process.
    static WifiManager* instance = new WifiManager;
    return *instance;
}

WifiManager::WifiManager() = default;

WifiManager::~WifiManager() {
    WifiStation* station = nullptr;
    WifiConfigurationAp* config_ap = nullptr;
    bool deinitialize = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        station = station_active_ ? station_.get() : nullptr;
        config_ap = config_mode_active_ ? config_ap_.get() : nullptr;
        station_active_ = false;
        config_mode_active_ = false;
        deinitialize = initialized_;
    }
    if (station != nullptr) {
        station->Stop();
    }
    if (config_ap != nullptr) {
        if (!config_ap->Stop()) {
            ESP_LOGE(TAG, "Config AP teardown boundary failed during destruction");
        }
    }
    if (deinitialize) {
        esp_wifi_deinit();
    }
}

void WifiManager::NotifyEvent(WifiEvent event, const std::string& data) {
    // Copy callback under lock, invoke without lock to avoid deadlock
    std::function<void(WifiEvent, const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = event_callback_;
    }
    if (callback) {
        callback(event, data);
    }
}

bool WifiManager::Initialize(const WifiManagerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    config_ = config;
    ESP_LOGI(TAG, "Initializing...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize netif
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Create event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    station_ = std::make_unique<WifiStation>(scan_lease_coordinator_);
    config_ap_ = std::make_unique<WifiConfigurationAp>(scan_lease_coordinator_);

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized");
    return true;
}

bool WifiManager::StopRadio() {
    WifiStation* station = nullptr;
    WifiConfigurationAp* config_ap = nullptr;
    uint64_t transition_generation = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return true;
    }
    if (lifecycle_transition_in_progress_) {
        return false;
    }
    lifecycle_transition_in_progress_ = true;
    transition_generation = ++lifecycle_generation_;
    station = station_active_ ? station_.get() : nullptr;
    config_ap = config_mode_active_ ? config_ap_.get() : nullptr;
    station_active_ = false;
    config_mode_active_ = false;
    lock.unlock();
    if (station != nullptr) {
        station->Stop();
    }
    if (config_ap != nullptr) {
        if (!config_ap->Stop()) {
            ESP_LOGE(TAG, "Config AP teardown boundary failed; radio stop remains blocked");
            std::lock_guard<std::mutex> lock(mutex_);
            if (lifecycle_generation_ == transition_generation &&
                config_ap_.get() == config_ap) {
                config_mode_active_ = true;
                lifecycle_transition_in_progress_ = false;
            }
            return false;
        }
    }

    // Provisioning scans start the radio directly through esp_wifi_* and are
    // not reflected by station_active_/config_mode_active_. Stop the radio to
    // release its DMA buffers while keeping the initialized driver available
    // for a later BLE rescan without another large esp_wifi_init allocation.
    const esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "WiFi radio stop returned: %s", esp_err_to_name(stop_ret));
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_generation_ == transition_generation &&
            (station == nullptr || station_.get() == station) &&
            (config_ap == nullptr || config_ap_.get() == config_ap)) {
            lifecycle_transition_in_progress_ = false;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_generation_ == transition_generation &&
            (station == nullptr || station_.get() == station) &&
            (config_ap == nullptr || config_ap_.get() == config_ap)) {
            lifecycle_transition_in_progress_ = false;
        }
    }

    ESP_LOGI(TAG, "WiFi radio stopped; driver remains initialized");
    return true;
}

bool WifiManager::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

// ==================== Station Mode ====================

void WifiManager::StartStation() {
    WifiStation* station = nullptr;
    WifiConfigurationAp* config_ap_to_stop = nullptr;
    WifiManagerConfig config;
    uint64_t transition_generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            ESP_LOGE(TAG, "Not initialized");
            return;
        }
        if (station_active_ || lifecycle_transition_in_progress_) {
            return;
        }
        lifecycle_transition_in_progress_ = true;
        transition_generation = ++lifecycle_generation_;
        station = station_.get();
        config_ap_to_stop = config_mode_active_ ? config_ap_.get() : nullptr;
        config_mode_active_ = false;
        config = config_;
    }
    if (config_ap_to_stop != nullptr) {
        ESP_LOGI(TAG, "Stopping config AP before starting station");
        if (!config_ap_to_stop->Stop()) {
            ESP_LOGE(TAG, "Config AP teardown boundary failed; station remains blocked");
            std::lock_guard<std::mutex> lock(mutex_);
            if (lifecycle_generation_ == transition_generation &&
                config_ap_.get() == config_ap_to_stop) {
                config_mode_active_ = true;
                lifecycle_transition_in_progress_ = false;
            }
            return;
        }
        NotifyEvent(WifiEvent::ConfigModeExit);
    }

    ESP_LOGI(TAG, "Starting station");

    // Apply configuration
    station->SetScanIntervalRange(config.station_scan_min_interval_seconds,
                                  config.station_scan_max_interval_seconds);

    // Setup callbacks
    station->OnScanBegin([this]() {
        NotifyEvent(WifiEvent::Scanning);
    });
    station->OnConnect([this](const std::string& ssid) {
        NotifyEvent(WifiEvent::Connecting, ssid);
    });
    station->OnConnected([this](const std::string& ssid) {
        NotifyEvent(WifiEvent::Connected, ssid);
    });
    station->OnDisconnected([this](int reason) {
        NotifyEvent(WifiEvent::Disconnected, std::to_string(reason));
    });

    station->Start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_generation_ == transition_generation &&
            station_.get() == station) {
            station_active_ = true;
            lifecycle_transition_in_progress_ = false;
        }
    }
}

void WifiManager::StopStation() {
    WifiStation* station = nullptr;
    uint64_t transition_generation = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!station_active_ || lifecycle_transition_in_progress_) {
            return;
        }
        lifecycle_transition_in_progress_ = true;
        transition_generation = ++lifecycle_generation_;
        station = station_.get();
        station_active_ = false;
        lock.unlock();
    }

    ESP_LOGI(TAG, "Stopping station");
    station->Stop();
    ESP_LOGI(TAG, "Station stopped");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_generation_ == transition_generation &&
            station_.get() == station) {
            lifecycle_transition_in_progress_ = false;
        }
    }
    NotifyEvent(WifiEvent::Disconnected);
}

bool WifiManager::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_active_ && station_ && station_->IsConnected();
}

std::string WifiManager::GetSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) return "";
    return station_->GetSsid();
}

std::string WifiManager::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) return "";
    return station_->GetIpAddress();
}

int WifiManager::GetRssi() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected()) return 0;
    return station_->GetRssi();
}

int WifiManager::GetChannel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected()) return 0;
    return station_->GetChannel();
}

std::string WifiManager::GetMacAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mac_address_.empty()) {
        return mac_address_;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        mac_address_ = buf;
    }
    return mac_address_;
}

// ==================== Config AP Mode ====================

void WifiManager::StartConfigAp() {
    WifiStation* station_to_stop = nullptr;
    WifiConfigurationAp* config_ap = nullptr;
    WifiManagerConfig config;
    uint64_t transition_generation = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!initialized_) {
            ESP_LOGE(TAG, "Not initialized");
            return;
        }
        if (config_mode_active_ || lifecycle_transition_in_progress_) {
            return;
        }
        lifecycle_transition_in_progress_ = true;
        transition_generation = ++lifecycle_generation_;
        station_to_stop = station_active_ ? station_.get() : nullptr;
        station_active_ = false;
        config_ap = config_ap_.get();
        config = config_;
        lock.unlock();
    }
    if (station_to_stop != nullptr) {
        ESP_LOGI(TAG, "Stopping station before starting config AP");
        station_to_stop->Stop();
        NotifyEvent(WifiEvent::Disconnected);
    }

    ESP_LOGI(TAG, "Starting config AP");

    config_ap->SetSsidPrefix(config.ssid_prefix);
    config_ap->SetLanguage(config.language);

    // Web handler calls this when user submits config
    config_ap->OnExitRequested([this]() {
        ESP_LOGI(TAG, "Config exit requested from web");
        StopConfigAp();
    });

    config_ap->Start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_generation_ == transition_generation &&
            config_ap_.get() == config_ap) {
            config_mode_active_ = true;
            lifecycle_transition_in_progress_ = false;
        }
    }
    NotifyEvent(WifiEvent::ConfigModeEnter);
}

void WifiManager::StopConfigAp() {
    WifiConfigurationAp* config_ap = nullptr;
    uint64_t transition_generation = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!config_mode_active_ || lifecycle_transition_in_progress_) {
            return;
        }
        lifecycle_transition_in_progress_ = true;
        transition_generation = ++lifecycle_generation_;
        config_ap = config_ap_.get();
        config_mode_active_ = false;
        lock.unlock();
    }

    ESP_LOGI(TAG, "Stopping config AP");
    if (!config_ap->Stop()) {
        ESP_LOGE(TAG, "Config AP teardown boundary failed; mode remains blocked");
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_generation_ == transition_generation &&
            config_ap_.get() == config_ap) {
            config_mode_active_ = true;
            lifecycle_transition_in_progress_ = false;
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_generation_ == transition_generation &&
            config_ap_.get() == config_ap) {
            lifecycle_transition_in_progress_ = false;
        }
    }
    NotifyEvent(WifiEvent::ConfigModeExit);
}

bool WifiManager::IsConfigMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_mode_active_;
}

std::string WifiManager::GetApSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_) return "";
    return config_ap_->GetSsid();
}

std::string WifiManager::GetApWebUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_) return "";
    return config_ap_->GetWebServerUrl();
}

// ==================== Power ====================

void WifiManager::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) {
        return;
    }
    station_->SetPowerSaveLevel(level);
}

// ==================== Event ====================

void WifiManager::SetEventCallback(std::function<void(WifiEvent, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}
