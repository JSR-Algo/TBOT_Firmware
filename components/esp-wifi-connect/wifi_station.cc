#include "wifi_station.h"
#include <cstring>
#include <algorithm>
#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT 5

WifiStation::WifiStation(
        WifiScanLeaseCoordinator& scan_lease_coordinator)
    : scan_lease_coordinator_(scan_lease_coordinator) {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");

    std::lock_guard<std::recursive_mutex> callback_lock(
        session_callback_mutex_);
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
    }
    std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_);
    scans_enabled_ = false;
    ++scan_session_id_;
    esp_timer_handle_t timer_to_delete = timer_handle_;
    timer_handle_ = nullptr;
    session_operations_drained_.wait(lifecycle_lock, [this]() {
        return in_flight_session_operations_ == 0;
    });

    if (scan_lease_.has_value()) {
        const auto lease = *scan_lease_;
        scan_lease_coordinator_.BeginDrain(lease);
        esp_wifi_scan_stop();
    }

    // The process-lifetime manager retains this object and its handler until
    // callback debt is consumed or later full driver recovery proves it gone.
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }

    {
        // Lock order is callback -> scan -> data. Data is never held across a
        // driver call, callback, or later scan-mutex acquisition.
        std::lock_guard<std::mutex> data_lock(session_data_mutex_);
        was_connected_ = false;
        for (auto& record : connect_queue_) {
            std::fill(record.password.begin(), record.password.end(), '\0');
        }
        connect_queue_.clear();
        std::fill(password_.begin(), password_.end(), '\0');
        password_.clear();
        ssid_.clear();
        ip_address_.clear();
        reconnect_count_ = 0;
    }
    lifecycle_lock.unlock();
    if (timer_to_delete != nullptr) {
        esp_timer_delete(timer_to_delete);
    }

    // Clear connected bit
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);

    // Set stopped event AFTER cleanup is complete to unblock WaitForConnected
    // This ensures no race condition with subsequent WiFi operations
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void(int reason)> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive

    // Clear stopped event bit so WaitForConnected works properly
    // Clear scan done bit so Stop() can wait for scan to complete
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);

    // Create the default WiFi station interface
    station_netif_ = esp_netif_create_default_wifi_sta();

    if (instance_any_id_ == nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::WifiEventHandler,
            this, &instance_any_id_));
    }
    if (instance_got_ip_ == nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::IpEventHandler,
            this, &instance_got_ip_));
    }
    // Setup the timer to scan WiFi.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise an idle device that failed to connect would
    // never retry the scan, because esp_timer_get_next_alarm_for_wake_up
    // (components/esp_timer/src/esp_timer.c) excludes timers with this flag
    // from light-sleep wakeup sources.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<WifiStation*>(arg)->StartOwnedScan();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        scans_enabled_ = true;
        ++scan_session_id_;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }
}

bool WifiStation::StartOwnedScan() {
    int64_t retry_delay_microseconds = 0;
    {
        std::lock_guard<std::mutex> data_lock(session_data_mutex_);
        retry_delay_microseconds = scan_current_interval_microseconds_;
    }
    WifiScanLeaseCoordinator::Lease lease;
    std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_);
    if (!scans_enabled_) {
        return false;
    }
    const bool local_callback_debt = scan_lease_.has_value();
    if (local_callback_debt) {
        if (timer_handle_ != nullptr) {
            esp_timer_start_once(timer_handle_, retry_delay_microseconds);
        }
        return false;
    }
    const auto acquired = scan_lease_coordinator_.TryAcquire(WifiScanLeaseCoordinator::Owner::kStation);
    if (!acquired.acquired) {
        if (timer_handle_ != nullptr) {
            esp_timer_start_once(timer_handle_, retry_delay_microseconds);
        }
        return false;
    }
    lease = acquired.lease;
    scan_lease_ = lease;
    lease_session_id_ = scan_session_id_;

    const esp_err_t err = esp_wifi_scan_start(nullptr, false);
    const auto commit =
        scan_lease_coordinator_.CommitSubmission(lease, err == ESP_OK);
    lifecycle_lock.unlock();
    if (commit.consume_latched) {
        CompleteOwnedScan(lease);
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi station scan start skipped: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void WifiStation::CompleteOwnedScan(
        const WifiScanLeaseCoordinator::Lease& lease) {
    uint64_t expected_session = 0;
    std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_);
    if (!scan_lease_.has_value() ||
        scan_lease_->owner != lease.owner ||
        scan_lease_->lease_id != lease.lease_id ||
        scan_lease_->driver_incarnation != lease.driver_incarnation) {
        return;
    }
    expected_session = lease_session_id_;
    if (!TryBeginSessionOperationLocked(expected_session)) {
        FinishDiscardedCompletionLocked(lease);
        return;
    }
    lifecycle_lock.unlock();

    uint16_t ap_num = 0;
    std::vector<wifi_ap_record_t> ap_records;
    bool cleanup_proven = false;
    if (esp_wifi_scan_get_ap_num(&ap_num) == ESP_OK && ap_num != 0) {
        ap_records.resize(ap_num);
        if (esp_wifi_scan_get_ap_records(&ap_num, ap_records.data()) != ESP_OK) {
            ap_records.clear();
        } else {
            ap_records.resize(ap_num);
            cleanup_proven = true;
        }
    }
    if (!cleanup_proven) {
        cleanup_proven = esp_wifi_clear_ap_list() == ESP_OK;
    }

    std::optional<WifiApRecord> connect_record;
    std::optional<int64_t> retry_delay_microseconds;
    if (cleanup_proven) {
        std::lock_guard<std::mutex> data_lock(session_data_mutex_);
        connect_record = HandleScanResultLocked(
            std::move(ap_records), retry_delay_microseconds);
    }
    std::optional<std::string> connecting_ssid;
    if (connect_record.has_value()) {
        connecting_ssid = StartConnectForSession(std::move(*connect_record));
    }
    FinishSessionOperation();

    {
        std::lock_guard<std::mutex> lifecycle_lock(scan_mutex_);
        if (!cleanup_proven) {
            ESP_LOGE(TAG, "WiFi scan AP-list cleanup failed; recovery required");
            scan_recovery_needed_ = true;
            return;
        }
        if (scan_lease_coordinator_.FinishCompletion(lease)) {
            scan_lease_.reset();
        }
    }
    if (retry_delay_microseconds.has_value()) {
        ScheduleScanRetry(expected_session, *retry_delay_microseconds);
    }
    if (connecting_ssid.has_value()) {
        DispatchSessionCallback(expected_session,
            [this, connecting_ssid = std::move(*connecting_ssid)]() {
                if (on_connect_) {
                    on_connect_(connecting_ssid);
                }
            });
    }
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    // Wait for either connected or stopped event
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED,
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    // Return true only if connected (not if stopped)
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

std::optional<WifiApRecord> WifiStation::HandleScanResultLocked(
        std::vector<wifi_ap_record_t> ap_records,
        std::optional<int64_t>& retry_delay_microseconds) {
    // sort by rssi descending
    std::sort(ap_records.begin(), ap_records.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (const auto& ap_record : ap_records) {
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Matched saved AP: RSSI=%d channel=%d authmode=%d",
                     ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode,
                .bssid = {0}
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000 / 1000);
        retry_delay_microseconds = scan_current_interval_microseconds_;
        UpdateScanInterval();
        return std::nullopt;
    }

    return PrepareNextConnectLocked();
}

void WifiStation::ScheduleScanRetry(
        uint64_t expected_session, int64_t delay_microseconds) {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (scans_enabled_ && expected_session == scan_session_id_ &&
        timer_handle_ != nullptr) {
        esp_timer_start_once(timer_handle_, delay_microseconds);
    }
}

void WifiStation::StartConnect() {
    uint64_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        session_id = scan_session_id_;
        if (!TryBeginSessionOperationLocked(session_id)) {
            return;
        }
    }
    std::optional<WifiApRecord> connect_record;
    {
        std::lock_guard<std::mutex> data_lock(session_data_mutex_);
        if (!connect_queue_.empty()) {
            connect_record = PrepareNextConnectLocked();
        }
    }
    if (!connect_record.has_value()) {
        FinishSessionOperation();
        return;
    }
    const std::string connecting_ssid =
        StartConnectForSession(std::move(*connect_record));
    FinishSessionOperation();
    DispatchSessionCallback(session_id, [this, connecting_ssid]() {
        if (on_connect_) {
            on_connect_(connecting_ssid);
        }
    });
}

WifiApRecord WifiStation::PrepareNextConnectLocked() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    return ap_record;
}

std::string WifiStation::StartConnectForSession(WifiApRecord ap_record) {
    const std::string connecting_ssid = ap_record.ssid;
    {
        std::lock_guard<std::mutex> data_lock(session_data_mutex_);
        std::fill(password_.begin(), password_.end(), '\0');
        ssid_ = ap_record.ssid;
        password_ = ap_record.password;
        reconnect_count_ = 0;
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    const size_t ssid_len =
        std::min(ap_record.ssid.size(), sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.ssid, ap_record.ssid.data(), ssid_len);
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    wifi_config.sta.listen_interval = 10;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    bzero(&wifi_config, sizeof(wifi_config));
    std::fill(ap_record.password.begin(), ap_record.password.end(), '\0');
    ESP_ERROR_CHECK(esp_wifi_connect());
    return connecting_ssid;
}

bool WifiStation::TryBeginSessionOperationLocked(uint64_t session_id) {
    if (!scans_enabled_ || session_id == 0 || session_id != scan_session_id_) {
        return false;
    }
    ++in_flight_session_operations_;
    return true;
}

void WifiStation::FinishSessionOperation() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (in_flight_session_operations_ == 0) {
        return;
    }
    --in_flight_session_operations_;
    if (in_flight_session_operations_ == 0) {
        session_operations_drained_.notify_all();
    }
}

void WifiStation::DispatchSessionCallback(
        uint64_t expected_session, std::function<void()> callback) {
    std::lock_guard<std::recursive_mutex> callback_lock(
        session_callback_mutex_);
    {
        std::lock_guard<std::mutex> lifecycle_lock(scan_mutex_);
        if (!(scans_enabled_ && expected_session == scan_session_id_)) {
            return;
        }
    }
    if (callback) {
        callback();
    }
}

void WifiStation::FinishDiscardedCompletionLocked(
        const WifiScanLeaseCoordinator::Lease& lease) {
    if (esp_wifi_clear_ap_list() != ESP_OK) {
        ESP_LOGE(TAG, "Discarded scan AP-list cleanup failed; recovery required");
        scan_recovery_needed_ = true;
        return;
    }
    if (scan_lease_coordinator_.FinishCompletion(lease)) {
        scan_lease_.reset();
    }
}

bool WifiStation::ScanRecoveryNeeded() const {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    return scan_recovery_needed_;
}

std::string WifiStation::GetSsid() const {
    std::lock_guard<std::mutex> lock(session_data_mutex_);
    return ssid_;
}

std::string WifiStation::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(session_data_mutex_);
    return ip_address_;
}

int8_t WifiStation::GetRssi() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }

    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }

    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    std::lock_guard<std::mutex> lock(session_data_mutex_);
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}

void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;  // Maximum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: LOW_POWER (MAX_MODEM)");
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;  // Minimum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: BALANCED (MIN_MODEM)");
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;       // No power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: PERFORMANCE (NONE)");
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}

void WifiStation::UpdateScanInterval() {
    // Apply exponential backoff: double the interval, up to max
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        if (this_->StartOwnedScan()) {
            uint64_t expected_session = 0;
            bool permitted = false;
            {
                std::lock_guard<std::mutex> lock(this_->scan_mutex_);
                expected_session = this_->scan_session_id_;
                permitted = this_->TryBeginSessionOperationLocked(
                    expected_session);
            }
            if (permitted) {
                this_->FinishSessionOperation();
                this_->DispatchSessionCallback(expected_session, [this_]() {
                    if (this_->on_scan_begin_) {
                        this_->on_scan_begin_();
                    }
                });
            }
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        std::optional<WifiScanLeaseCoordinator::Lease> lease_snapshot;
        {
            std::lock_guard<std::mutex> lock(this_->scan_mutex_);
            lease_snapshot = this_->scan_lease_;
        }
        if (!lease_snapshot.has_value()) {
            ESP_LOGI(TAG, "Ignoring WiFi scan done event not owned by WifiStation");
            return;
        }
        const auto lease = *lease_snapshot;
        const auto callback =
            this_->scan_lease_coordinator_.ObserveScanDone(lease);
        if (!callback.consume_now) {
            return;
        }
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
        this_->CompleteOwnedScan(lease);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        uint64_t session_id = 0;
        {
            std::lock_guard<std::mutex> lock(this_->scan_mutex_);
            session_id = this_->scan_session_id_;
            if (!this_->TryBeginSessionOperationLocked(session_id)) {
                return;
            }
        }
        wifi_event_sta_disconnected_t* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        const int reason = event->reason;
        bool notify_disconnected = false;
        bool reconnect = false;
        int reconnect_count = 0;
        std::optional<WifiApRecord> connect_record;
        std::optional<std::string> connecting_ssid;
        std::optional<int64_t> retry_delay_microseconds;
        {
            std::lock_guard<std::mutex> data_lock(
                this_->session_data_mutex_);
            xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
            notify_disconnected = this_->was_connected_;
            this_->was_connected_ = false;
            if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
                ++this_->reconnect_count_;
                reconnect_count = this_->reconnect_count_;
                reconnect = true;
            } else if (!this_->connect_queue_.empty()) {
                connect_record = this_->PrepareNextConnectLocked();
            } else {
                retry_delay_microseconds =
                    this_->scan_current_interval_microseconds_;
                this_->UpdateScanInterval();
            }
        }
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", reason);
        if (reconnect) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "Reconnecting WiFi (attempt %d / %d)",
                     reconnect_count, MAX_RECONNECT_COUNT);
        } else if (connect_record.has_value()) {
            connecting_ssid = this_->StartConnectForSession(
                std::move(*connect_record));
        } else {
            ESP_LOGI(TAG, "No more AP to connect, next scan in %d seconds",
                     static_cast<int>(*retry_delay_microseconds / 1000 / 1000));
        }
        this_->FinishSessionOperation();
        if (retry_delay_microseconds.has_value()) {
            this_->ScheduleScanRetry(session_id, *retry_delay_microseconds);
        }
        if (connecting_ssid.has_value()) {
            this_->DispatchSessionCallback(session_id,
                [this_, connecting_ssid = std::move(*connecting_ssid)]() {
                    if (this_->on_connect_) {
                        this_->on_connect_(connecting_ssid);
                    }
                });
        }
        if (notify_disconnected) {
            this_->DispatchSessionCallback(session_id, [this_, reason]() {
                if (this_->on_disconnected_) {
                    this_->on_disconnected_(reason);
                }
            });
        }
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    uint64_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(this_->scan_mutex_);
        session_id = this_->scan_session_id_;
        if (!this_->TryBeginSessionOperationLocked(session_id)) {
            return;
        }
    }
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    std::string connected_ssid;
    {
        std::lock_guard<std::mutex> data_lock(
            this_->session_data_mutex_);
        this_->ip_address_ = ip_address;
        connected_ssid = this_->ssid_;
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        this_->was_connected_ = true;
        for (auto& record : this_->connect_queue_) {
            std::fill(record.password.begin(), record.password.end(), '\0');
        }
        this_->connect_queue_.clear();
        this_->reconnect_count_ = 0;
        this_->scan_current_interval_microseconds_ =
            this_->scan_min_interval_microseconds_;
    }
    ESP_LOGI(TAG, "Got IP: %s", ip_address);
    this_->FinishSessionOperation();
    this_->DispatchSessionCallback(session_id, [this_, connected_ssid]() {
        if (this_->on_connected_) {
            this_->on_connected_(connected_ssid);
        }
    });
}
