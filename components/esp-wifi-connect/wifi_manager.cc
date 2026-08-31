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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "WifiManager"

WifiManager& WifiManager::GetInstance() {
    // Scanner handlers and their coordinator may be referenced by queued
    // default-event-loop callbacks for the remainder of the process.
    static WifiManager* instance = new WifiManager;
    return *instance;
}

WifiManager::WifiManager() = default;

namespace {

bool SameLease(const WifiScanLeaseCoordinator::Lease& left,
               const WifiScanLeaseCoordinator::Lease& right) {
    return left.owner == right.owner && left.lease_id == right.lease_id &&
           left.driver_incarnation == right.driver_incarnation;
}

}  // namespace

bool WifiManager::RegisterScanRecoveryOwner(
        WifiScanLeaseCoordinator::Owner owner,
        ScanRecoveryOwnerHooks hooks) {
    if ((owner != WifiScanLeaseCoordinator::Owner::kBlufi &&
         owner != WifiScanLeaseCoordinator::Owner::kBlockingUi) ||
        !hooks.claim || !hooks.has_debt || !hooks.restore_radio ||
        !hooks.complete || !hooks.retry) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (scan_recovery_active_ && scan_recovery_debt_.has_value() &&
        scan_recovery_debt_->owner == owner) {
        return false;
    }
    external_scan_recovery_hooks_[static_cast<size_t>(owner)] =
        std::move(hooks);
    return true;
}

bool WifiManager::ScheduleScanRecovery(
        const WifiScanLeaseCoordinator::Lease& lease) {
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || scan_recovery_task_ == nullptr) {
            return false;
        }
        if (scan_recovery_debt_.has_value() &&
            SameLease(*scan_recovery_debt_, lease)) {
            task = scan_recovery_task_;
        } else if (scan_recovery_active_) {
            if (scan_recovery_claim_.has_value()) {
                ESP_LOGE(TAG, "Conflicting WiFi scan recovery debt rejected");
                return false;
            }
            // The previous callback may have released its debt before this
            // worker claimed it. Retarget the queued wakeup to the new exact
            // lease; a claimed recovery can never be replaced.
            scan_recovery_debt_ = lease;
            task = scan_recovery_task_;
        } else {
            scan_recovery_debt_ = lease;
            scan_recovery_active_ = true;
            scan_recovery_retry_pending_ = false;
            task = scan_recovery_task_;
        }
    }
    return xTaskNotifyGive(task) == pdPASS;
}

void WifiManager::ScanRecoveryTask(void* context) {
    auto* self = static_cast<WifiManager*>(context);
    for (;;) {
        // A failed task notification must not strand one-shot recovery debt.
        // Periodic polling is the allocation-free, process-lifetime fallback.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        self->RunScanRecovery();
    }
}

bool WifiManager::DeferLifecycleTransitionForRecovery(
        PendingLifecycleTarget target, uint64_t transition_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_generation_ != transition_generation) {
        return true;
    }
    if (!scan_recovery_active_) {
        return false;
    }
    if (pending_lifecycle_target_ != PendingLifecycleTarget::kNone &&
        (pending_lifecycle_target_ != target ||
         pending_lifecycle_generation_ != transition_generation)) {
        ESP_LOGE(TAG, "Conflicting pending WiFi lifecycle transition rejected");
        lifecycle_transition_in_progress_ = false;
        return true;
    }
    pending_lifecycle_target_ = target;
    pending_lifecycle_generation_ = transition_generation;
    station_active_ = false;
    config_mode_active_ = false;
    lifecycle_transition_in_progress_ = false;
    return true;
}

void WifiManager::ResumePendingLifecycleTransition() {
    PendingLifecycleTarget target = PendingLifecycleTarget::kNone;
    uint64_t pending_generation = 0;
    WifiStation* station = nullptr;
    WifiConfigurationAp* config_ap = nullptr;
    WifiManagerConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scan_recovery_active_) {
            return;
        }
        target = pending_lifecycle_target_;
        pending_generation = pending_lifecycle_generation_;
        pending_lifecycle_target_ = PendingLifecycleTarget::kNone;
        pending_lifecycle_generation_ = 0;
        if (target == PendingLifecycleTarget::kNone ||
            lifecycle_generation_ != pending_generation) {
            return;
        }
        lifecycle_transition_in_progress_ = true;
        station = station_.get();
        config_ap = config_ap_.get();
        config = config_;
    }
    if (target == PendingLifecycleTarget::kStation) {
        StartStationTarget(station, config, pending_generation);
    } else if (target == PendingLifecycleTarget::kConfigAp) {
        StartConfigApTarget(config_ap, config, pending_generation);
    }
}

void WifiManager::RunScanRecovery() {
    for (;;) {
        std::optional<WifiScanLeaseCoordinator::Lease> debt;
        std::optional<ScanRecoveryWork> work;
        bool wait_for_lifecycle = false;
        bool resume_pending_transition = false;
        ScanRecoveryOwnerHooks external_hooks;
        bool external_owner = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!scan_recovery_active_ || !scan_recovery_debt_.has_value()) {
                return;
            }
            debt = scan_recovery_debt_;
            work = scan_recovery_claim_;
            wait_for_lifecycle = lifecycle_transition_in_progress_;
            if (wait_for_lifecycle) {
                scan_recovery_retry_pending_ = true;
            }
            const size_t owner_index = static_cast<size_t>(debt->owner);
            external_owner = owner_index < external_scan_recovery_hooks_.size() &&
                external_scan_recovery_hooks_[owner_index].has_value();
            if (external_owner) {
                external_hooks = *external_scan_recovery_hooks_[owner_index];
            }
        }
        if (wait_for_lifecycle) {
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }

        if (!work.has_value()) {
            if (debt->owner == WifiScanLeaseCoordinator::Owner::kStation) {
                auto claim = station_->ClaimScanRecovery(*debt);
                if (claim.has_value()) {
                    work = ScanRecoveryWork{
                        claim->lease, claim->recovery, std::nullopt,
                        claim->scan_session_id, claim->scans_were_enabled};
                }
            } else if (debt->owner ==
                       WifiScanLeaseCoordinator::Owner::kConfigAp) {
                auto claim = config_ap_->ClaimScanRecovery(*debt);
                if (claim.has_value()) {
                    work = ScanRecoveryWork{
                        claim->lease, claim->recovery, std::nullopt,
                        claim->scan_session_id, claim->scans_were_enabled};
                }
            } else if (external_owner) {
                const auto recovery = external_hooks.claim(*debt);
                if (recovery.has_value()) {
                    work = ScanRecoveryWork{
                        *debt, *recovery, std::nullopt, 0, false};
                }
            }
            if (!work.has_value()) {
                bool debt_still_exists = false;
                if (debt->owner == WifiScanLeaseCoordinator::Owner::kStation) {
                    debt_still_exists = station_->HasScanRecoveryDebt(*debt);
                } else if (debt->owner ==
                           WifiScanLeaseCoordinator::Owner::kConfigAp) {
                    debt_still_exists = config_ap_->HasScanRecoveryDebt(*debt);
                } else if (external_owner) {
                    debt_still_exists = external_hooks.has_debt(*debt);
                }
                if (debt_still_exists) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        scan_recovery_retry_pending_ = true;
                    }
                    vTaskDelay(pdMS_TO_TICKS(250));
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (scan_recovery_debt_.has_value() &&
                        SameLease(*scan_recovery_debt_, *debt)) {
                        scan_recovery_debt_.reset();
                        scan_recovery_active_ = false;
                        scan_recovery_retry_pending_ = false;
                        resume_pending_transition = true;
                    }
                }
                if (resume_pending_transition) {
                    ResumePendingLifecycleTransition();
                }
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (!scan_recovery_debt_.has_value() ||
                !SameLease(*scan_recovery_debt_, work->lease)) {
                return;
            }
            scan_recovery_claim_ = work;
        }

        if (!work->proof.has_value()) {
            const auto proof = scan_recovery_executor_.Execute(work->recovery);
            if (!proof.Proves(work->recovery)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    scan_recovery_retry_pending_ = true;
                }
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            work->proof = proof;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                scan_recovery_claim_ = work;
            }
        }

        bool owner_ready = false;
        if (work->lease.owner == WifiScanLeaseCoordinator::Owner::kStation) {
            WifiStation::ScanRecoveryClaim claim{
                work->lease, work->recovery, work->scan_session_id,
                work->scans_were_enabled};
            owner_ready = station_->RestoreRadioAfterRecovery(claim);
        } else if (work->lease.owner ==
                   WifiScanLeaseCoordinator::Owner::kConfigAp) {
            WifiConfigurationAp::ScanRecoveryClaim claim{
                work->lease, work->recovery, work->scan_session_id,
                work->scans_were_enabled};
            owner_ready = config_ap_->RestoreRadioAfterRecovery(claim);
        } else if (external_owner) {
            owner_ready = external_hooks.restore_radio(work->lease);
        }
        if (!owner_ready) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                scan_recovery_retry_pending_ = true;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        bool completed = false;
        if (work->lease.owner == WifiScanLeaseCoordinator::Owner::kStation) {
            WifiStation::ScanRecoveryClaim claim{
                work->lease, work->recovery, work->scan_session_id,
                work->scans_were_enabled};
            completed = station_->CompleteScanRecovery(claim, *work->proof);
        } else if (work->lease.owner ==
                   WifiScanLeaseCoordinator::Owner::kConfigAp) {
            WifiConfigurationAp::ScanRecoveryClaim claim{
                work->lease, work->recovery, work->scan_session_id,
                work->scans_were_enabled};
            completed = config_ap_->CompleteScanRecovery(claim, *work->proof);
        } else if (external_owner) {
            completed = external_hooks.complete(work->lease, *work->proof);
        }
        if (!completed) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                scan_recovery_retry_pending_ = true;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        const auto owner = work->lease.owner;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scan_recovery_claim_.reset();
            scan_recovery_debt_.reset();
            scan_recovery_active_ = false;
            scan_recovery_retry_pending_ = false;
        }
        if (owner == WifiScanLeaseCoordinator::Owner::kStation) {
            station_->RetryScanAfterRecovery();
        } else if (owner == WifiScanLeaseCoordinator::Owner::kConfigAp) {
            config_ap_->RetryScanAfterRecovery();
        } else if (external_owner) {
            external_hooks.retry(work->lease);
        }
        ResumePendingLifecycleTransition();
        return;
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

    if (!wifi_runtime_ready_) {
        // Initialize NVS
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
            ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "Erasing NVS...");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
            return false;
        }

        ret = esp_netif_init();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
            return false;
        }

        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
            return false;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = false;
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
            return false;
        }

        station_ = std::make_unique<WifiStation>(scan_lease_coordinator_);
        config_ap_ =
            std::make_unique<WifiConfigurationAp>(scan_lease_coordinator_);
        station_->OnScanRecoveryNeeded([this](const auto& lease) {
            ScheduleScanRecovery(lease);
        });
        config_ap_->OnScanRecoveryNeeded([this](const auto& lease) {
            ScheduleScanRecovery(lease);
        });
        wifi_runtime_ready_ = true;
    }
    if (scan_recovery_task_ == nullptr &&
        xTaskCreate(&WifiManager::ScanRecoveryTask, "wifi_scan_recover", 4096,
                    this, 5, &scan_recovery_task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi scan recovery task");
        return false;
    }

    initialized_ = true;
    wifi_teardown_faulted_ = false;
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
    if (wifi_teardown_faulted_) {
        return false;
    }
    if (lifecycle_transition_in_progress_ || scan_recovery_active_ ||
        pending_lifecycle_target_ != PendingLifecycleTarget::kNone) {
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
                wifi_teardown_faulted_ = true;
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

bool WifiManager::HasTeardownFault() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wifi_teardown_faulted_;
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
        if (station_active_ || lifecycle_transition_in_progress_ ||
            scan_recovery_active_ ||
            pending_lifecycle_target_ != PendingLifecycleTarget::kNone ||
            wifi_teardown_faulted_) {
            return;
        }
        lifecycle_transition_in_progress_ = true;
        transition_generation = ++lifecycle_generation_;
        station = station_.get();
        config_ap_to_stop = config_mode_active_ ? config_ap_.get() : nullptr;
        config = config_;
    }
    if (config_ap_to_stop != nullptr) {
        ESP_LOGI(TAG, "Stopping config AP before starting station");
        if (!config_ap_to_stop->Stop()) {
            ESP_LOGE(TAG, "Config AP teardown boundary failed; station remains blocked");
            std::lock_guard<std::mutex> lock(mutex_);
            if (lifecycle_generation_ == transition_generation &&
                config_ap_.get() == config_ap_to_stop) {
                wifi_teardown_faulted_ = true;
                lifecycle_transition_in_progress_ = false;
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lifecycle_generation_ != transition_generation) {
                return;
            }
            config_mode_active_ = false;
        }
        NotifyEvent(WifiEvent::ConfigModeExit);
    }
    if (DeferLifecycleTransitionForRecovery(
            PendingLifecycleTarget::kStation, transition_generation)) {
        return;
    }

    StartStationTarget(station, config, transition_generation);
}

void WifiManager::StartStationTarget(
        WifiStation* station, const WifiManagerConfig& config,
        uint64_t transition_generation) {
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
        if (!station_active_ || lifecycle_transition_in_progress_ ||
            scan_recovery_active_ ||
            pending_lifecycle_target_ != PendingLifecycleTarget::kNone) {
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
        if (config_mode_active_ || lifecycle_transition_in_progress_ ||
            scan_recovery_active_ ||
            pending_lifecycle_target_ != PendingLifecycleTarget::kNone ||
            wifi_teardown_faulted_) {
            return;
        }
        lifecycle_transition_in_progress_ = true;
        transition_generation = ++lifecycle_generation_;
        station_to_stop = station_active_ ? station_.get() : nullptr;
        config_ap = config_ap_.get();
        config = config_;
        lock.unlock();
    }
    if (station_to_stop != nullptr) {
        ESP_LOGI(TAG, "Stopping station before starting config AP");
        station_to_stop->Stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lifecycle_generation_ != transition_generation) {
                return;
            }
            station_active_ = false;
        }
        NotifyEvent(WifiEvent::Disconnected);
    }
    if (DeferLifecycleTransitionForRecovery(
            PendingLifecycleTarget::kConfigAp, transition_generation)) {
        return;
    }

    StartConfigApTarget(config_ap, config, transition_generation);
}

void WifiManager::StartConfigApTarget(
        WifiConfigurationAp* config_ap, const WifiManagerConfig& config,
        uint64_t transition_generation) {
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
        if (!config_mode_active_ || lifecycle_transition_in_progress_ ||
            scan_recovery_active_ ||
            pending_lifecycle_target_ != PendingLifecycleTarget::kNone ||
            wifi_teardown_faulted_) {
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
            wifi_teardown_faulted_ = true;
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
