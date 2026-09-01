#include "wifi_config_ui.h"
#include "wifi_credential_limits.h"
#include "blocking_wifi_scan_lease_state.h"
#include "blocking_wifi_scan_policy.h"
#include "blocking_wifi_scan_retry_state.h"
#include "blocking_wifi_scan_completion_status.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#define TAG "WifiConfigUI"

namespace {

using Coordinator = WifiScanLeaseCoordinator;
std::atomic<uint64_t> next_wifi_ui_generation{1};

class BlockingWifiScanOwner {
public:
    static BlockingWifiScanOwner& GetInstance() {
        static BlockingWifiScanOwner* owner = new BlockingWifiScanOwner;
        return *owner;
    }

    bool EnsureRegistered() {
        std::lock_guard<std::mutex> lock(registration_mutex_);
        if (completion_semaphore_ == nullptr) {
            completion_semaphore_ = xSemaphoreCreateBinary();
            if (completion_semaphore_ == nullptr) {
                return false;
            }
        }
        if (event_handler_ == nullptr &&
            esp_event_handler_instance_register(
                WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &HandleScanDone, this,
                &event_handler_) != ESP_OK) {
            return false;
        }
        if (!recovery_hooks_registered_) {
            recovery_hooks_registered_ =
                WifiManager::GetInstance().RegisterScanRecoveryOwner(
                    Coordinator::Owner::kBlockingUi,
                    WifiManager::ScanRecoveryOwnerHooks{
                        .claim = [this](const auto& lease) {
                            return ClaimRecovery(lease);
                        },
                        .has_debt = [this](const auto& lease) {
                            return state_.HasDebt(lease);
                        },
                        .restore_radio = [this](const auto& lease) {
                            return RestoreRadio(lease);
                        },
                        .complete = [this](const auto& lease,
                                          const auto& proof) {
                            auto& coordinator = WifiManager::GetInstance()
                                .ScanLeaseCoordinator();
                            if (!coordinator.CompleteRecovery(lease, proof)) {
                                return false;
                            }
                            if (!state_.FinishRecovery(lease)) {
                                return false;
                            }
                            return true;
                        },
                        .retry = [this](const auto& lease) {
                            std::lock_guard<std::mutex> lock(retry_mutex_);
                            retry_state_.Publish(
                                {lease, recovery_ui_generation_});
                        },
                    });
        }
        return event_handler_ != nullptr && recovery_hooks_registered_;
    }

    bool Begin(const Coordinator::Lease& lease, uint64_t ui_generation) {
        if (!EnsureRegistered()) {
            return false;
        }
        while (xSemaphoreTake(completion_semaphore_, 0) == pdTRUE) {
        }
        if (!state_.Begin(lease)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(retry_mutex_);
        active_ui_generation_ = ui_generation;
        return true;
    }

    bool RecoverPreparationFailure(const Coordinator::Lease& lease,
                                   bool radio_recovery_required) {
        auto& manager = WifiManager::GetInstance();
        auto& coordinator = manager.ScanLeaseCoordinator();
        if (!radio_recovery_required &&
            coordinator.AbandonUnsubmitted(lease)) {
            return state_.AbandonUnsubmitted(lease);
        }
        if (!state_.RetainForRecovery(lease)) {
            return false;
        }
        RememberRecoveryGeneration(lease);
        const auto commit = coordinator.CommitSubmission(lease, false);
        if (commit.consume_latched &&
            !coordinator.RetainFailedCompletion(lease)) {
            return false;
        }
        return (commit.drain_required || commit.consume_latched) &&
            manager.RequestScanRecovery(lease);
    }

    bool WaitForMatchingCompletion(const Coordinator::Lease& lease) {
        if (state_.CallbackClaimed(lease)) {
            return true;
        }
        constexpr uint32_t kConservativeWifiChannelCount = 14;
        constexpr uint32_t kMaxActiveScanMsPerChannel = 300;
        constexpr uint32_t kSchedulingMarginMs = 1000;
        constexpr uint32_t kCompletionWaitMs =
            BlockingWifiScanPolicy::CompletionWaitMs(
                kConservativeWifiChannelCount,
                kMaxActiveScanMsPerChannel, kSchedulingMarginMs);
        if (xSemaphoreTake(completion_semaphore_,
                           pdMS_TO_TICKS(kCompletionWaitMs)) ==
            pdTRUE) {
            return state_.CallbackClaimed(lease);
        }
        return state_.CallbackClaimed(lease);
    }

    bool DetachAndRecover(const Coordinator::Lease& lease) {
        auto& manager = WifiManager::GetInstance();
        auto& coordinator = manager.ScanLeaseCoordinator();
        if (state_.DetachWaiterForRecovery(lease)) {
            RememberRecoveryGeneration(lease);
            coordinator.BeginDrain(lease);
            manager.RequestScanRecovery(lease);
            return true;
        }
        return false;
    }

    bool RetainCompletionAndRecover(const Coordinator::Lease& lease) {
        auto& manager = WifiManager::GetInstance();
        auto& coordinator = manager.ScanLeaseCoordinator();
        if (!coordinator.RetainFailedCompletion(lease) ||
            !state_.RetainCompletionForRecovery(lease)) {
            return false;
        }
        RememberRecoveryGeneration(lease);
        return manager.RequestScanRecovery(lease);
    }

    bool FinishNormally(const Coordinator::Lease& lease) {
        auto& manager = WifiManager::GetInstance();
        auto& coordinator = manager.ScanLeaseCoordinator();
        if (manager.FinishExternalScanRadio(lease) !=
                WifiManager::ExternalScanRadioResult::kReady ||
            !coordinator.FinishCompletion(lease) ||
            !state_.FinishNormally(lease) ||
            !manager.ReleaseExternalScanRadioToken(lease)) {
            return false;
        }
        return true;
    }

    std::optional<BlockingWifiScanRetryState::Token> PeekRecoveryRetry(
            uint64_t ui_generation) const {
        return retry_state_.Peek(ui_generation);
    }

    bool ConsumeRecoveryRetry(
            const BlockingWifiScanRetryState::Token& token) {
        return retry_state_.ConsumeIfExact(token);
    }

    void CancelUiGeneration(uint64_t ui_generation) {
        retry_state_.CancelGeneration(ui_generation);
    }

    void PublishBusyRetry(uint64_t ui_generation) {
        retry_state_.PublishIfAbsent({Coordinator::Lease{}, ui_generation});
    }

    bool ScanSucceeded(const Coordinator::Lease& lease) const {
        return callback_status_.Succeeded(lease);
    }

private:
    BlockingWifiScanOwner() = default;

    static void HandleScanDone(void* context, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
        if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_SCAN_DONE) {
            return;
        }
        const auto* event =
            static_cast<const wifi_event_sta_scan_done_t*>(event_data);
        const uint32_t scan_status = event != nullptr ? event->status : 1;
        static_cast<BlockingWifiScanOwner*>(context)->OnScanDone(scan_status);
    }

    void OnScanDone(uint32_t scan_status) {
        const auto lease = state_.LeaseSnapshot();
        if (!lease.has_value()) {
            return;
        }
        if (state_.HasDebt(*lease)) {
            return;
        }
        const auto callback = WifiManager::GetInstance()
            .ScanLeaseCoordinator().ObserveScanDone(*lease);
        if (!callback.consume_now && !callback.deferred_until_commit) {
            return;
        }
        callback_status_.Observe(*lease, scan_status);
        const auto action = state_.OnCallback(*lease);
        if (action == BlockingWifiScanLeaseState::CallbackAction::kWakeWaiter) {
            xSemaphoreGive(completion_semaphore_);
            return;
        }
        if (action !=
            BlockingWifiScanLeaseState::CallbackAction::kCompleteWithoutWaiter) {
            return;
        }
        if (esp_wifi_clear_ap_list() == ESP_OK && FinishNormally(*lease)) {
            return;
        }
        RetainCompletionAndRecover(*lease);
    }

    std::optional<Coordinator::RecoveryDecision> ClaimRecovery(
            const Coordinator::Lease& lease) {
        if (!state_.HasDebt(lease)) {
            return std::nullopt;
        }
        auto recovery = WifiManager::GetInstance().ScanLeaseCoordinator()
            .BeginRecovery(lease);
        if (!recovery.begun()) {
            return std::nullopt;
        }
        return recovery;
    }

    bool RestoreRadio(const Coordinator::Lease& lease) {
        return state_.HasDebt(lease);
    }

    void RememberRecoveryGeneration(const Coordinator::Lease& lease) {
        if (!state_.Owns(lease)) {
            return;
        }
        std::lock_guard<std::mutex> lock(retry_mutex_);
        recovery_ui_generation_ = active_ui_generation_;
    }

    std::mutex registration_mutex_;
    mutable std::mutex retry_mutex_;
    BlockingWifiScanLeaseState state_;
    SemaphoreHandle_t completion_semaphore_ = nullptr;
    esp_event_handler_instance_t event_handler_ = nullptr;
    bool recovery_hooks_registered_ = false;
    BlockingWifiScanRetryState retry_state_;
    uint64_t active_ui_generation_ = 0;
    uint64_t recovery_ui_generation_ = 0;
    BlockingWifiScanCompletionStatus callback_status_;
};

bool CleanupScanList(std::vector<wifi_ap_record_t>& records) {
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        return esp_wifi_clear_ap_list() == ESP_OK;
    }
    try {
        records.resize(ap_count);
    } catch (...) {
        return esp_wifi_clear_ap_list() == ESP_OK;
    }
    if (esp_wifi_scan_get_ap_records(&ap_count, records.data()) != ESP_OK) {
        records.clear();
        return esp_wifi_clear_ap_list() == ESP_OK;
    }
    records.resize(ap_count);
    return true;
}

}  // namespace

WifiConfigUI::WifiConfigUI(LcdDisplay* display)
    : display_(display),
      state_(WifiConfigState::Scanning),
      is_active_(false),
      selected_index_(0),
      scroll_offset_(0),
      saved_selected_index_(0),
      saved_scroll_offset_(0),
      input_focus_on_password_(false),
      ui_generation_(next_wifi_ui_generation.fetch_add(1)),
      cursor_visible_(true),
      last_cursor_toggle_(0) {
}

WifiConfigUI::~WifiConfigUI() {
    CancelPendingScan();
}

void WifiConfigUI::Start() {
    ESP_LOGI(TAG, "Starting WiFi config UI");
    is_active_ = true;
    state_ = WifiConfigState::Scanning;
    selected_index_ = 0;
    scroll_offset_ = 0;
    input_ssid_.clear();
    input_password_.clear();
    selected_ssid_.clear();

    // Load saved WiFi list
    LoadSavedWifiList();

    // Start scanning
    StartScanning();
}

void WifiConfigUI::StartWithSavedList() {
    ESP_LOGI(TAG, "Starting WiFi config UI with saved list");
    is_active_ = true;
    selected_index_ = 0;
    scroll_offset_ = 0;
    input_ssid_.clear();
    input_password_.clear();
    selected_ssid_.clear();

    // Show saved list directly (ShowSavedList will load the list)
    ShowSavedList();
}

bool WifiConfigUI::StartScanning() {
    if (scan_request_pending_) {
        return false;
    }
    state_ = WifiConfigState::Scanning;
    scan_failed_ = false;

    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);
    DrawHeader("扫描 WiFi 中...");
    DrawFooter("请稍候...");

    ++scan_revision_;
    scan_request_pending_ = true;
    if (!scan_request_callback_ ||
        !scan_request_callback_(ui_generation_, scan_revision_)) {
        scan_request_pending_ = false;
        return false;
    }
    return true;
}

WifiConfigUI::ScanWorkerResult WifiConfigUI::RunWifiScanWorker(
        uint64_t ui_generation) {
    return DoWifiScan(ui_generation);
}

void WifiConfigUI::CompleteWifiScanWorker(uint64_t revision,
                                          ScanWorkerResult result) {
    if (revision != scan_revision_ || !scan_request_pending_ ||
        state_ != WifiConfigState::Scanning) {
        return;
    }
    scan_request_pending_ = false;
    if (!result.scan_started) {
        return;
    }
    scan_failed_ = result.failed;
    scan_results_ = std::move(result.networks);
    auto& owner = BlockingWifiScanOwner::GetInstance();
    const auto retry = owner.PeekRecoveryRetry(ui_generation_);
    if (retry.has_value()) {
        owner.ConsumeRecoveryRetry(*retry);
    }

    // Show results
    lv_obj_t* canvas = lv_scr_act();
    if (scan_failed_) {
        lv_obj_clean(canvas);
        DrawHeader("WiFi 扫描失败");
        DrawFooter("W:手动输入 Esc:退出");
    } else if (scan_results_.empty()) {
        lv_obj_clean(canvas);
        DrawHeader("未找到 WiFi");
        DrawFooter("W:手动输入 Esc:退出");
    } else {
        state_ = WifiConfigState::SelectWifi;
        ShowScanResults();
    }
}

void WifiConfigUI::CancelPendingScan() {
    is_active_ = false;
    ++scan_revision_;
    scan_request_pending_ = false;
    BlockingWifiScanOwner::GetInstance().CancelUiGeneration(ui_generation_);
}

void WifiConfigUI::DismissPendingScanResult() {
    if (!scan_request_pending_) {
        return;
    }
    ++scan_revision_;
    scan_request_pending_ = false;
}

WifiConfigUI::ScanWorkerResult WifiConfigUI::DoWifiScan(
        uint64_t ui_generation) {
    ScanWorkerResult output;
    auto& manager = WifiManager::GetInstance();
    if (!manager.IsInitialized() && !manager.Initialize()) {
        ESP_LOGE(TAG, "WiFi manager initialization failed");
        output.scan_started = true;
        return output;
    }
    auto& coordinator = manager.ScanLeaseCoordinator();
    auto& owner = BlockingWifiScanOwner::GetInstance();
    const auto acquired =
        coordinator.TryAcquire(Coordinator::Owner::kBlockingUi);
    if (!acquired.acquired) {
        owner.PublishBusyRetry(ui_generation);
        ESP_LOGW(TAG, "WiFi scan is busy");
        return output;
    }
    const auto lease = acquired.lease;
    if (!owner.Begin(lease, ui_generation)) {
        coordinator.AbandonUnsubmitted(lease);
        ESP_LOGE(TAG, "WiFi scan owner initialization failed");
        output.scan_started = true;
        return output;
    }
    const auto preparation = manager.PrepareExternalScanRadio(lease);
    if (preparation != WifiManager::ExternalScanRadioResult::kReady) {
        owner.RecoverPreparationFailure(
            lease, preparation ==
                WifiManager::ExternalScanRadioResult::kRecoveryRequired);
        ESP_LOGE(TAG, "WiFi manager radio preparation failed");
        output.scan_started = true;
        return output;
    }

    // Use WifiManager's scan capability if available, otherwise do direct scan
    // Note: We need to be careful not to disrupt existing WiFi state

    // Configure scan
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    // Nonblocking scans are the ESP-IDF path that emits WIFI_EVENT_SCAN_DONE.
    const esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    const auto commit = coordinator.CommitSubmission(lease, err == ESP_OK);
    if (!commit.accepted && !commit.consume_latched) {
        owner.DetachAndRecover(lease);
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return output;
    }
    output.scan_started = true;
    if (commit.callback_won_error) {
        ESP_LOGW(TAG, "WiFi scan completion arrived before start returned");
    }

    if (!owner.WaitForMatchingCompletion(lease)) {
        if (owner.DetachAndRecover(lease)) {
            ESP_LOGE(TAG, "WiFi scan completion timed out; recovery scheduled");
            return output;
        }
        // A matching callback may win after the bounded wait but before the
        // waiter detaches. In that case this caller still owns completion.
        if (!owner.WaitForMatchingCompletion(lease)) {
            ESP_LOGE(TAG, "WiFi scan completion ownership was lost");
            return output;
        }
    }

    if (!owner.ScanSucceeded(lease)) {
        if (esp_wifi_clear_ap_list() != ESP_OK ||
            !owner.FinishNormally(lease)) {
            owner.RetainCompletionAndRecover(lease);
        }
        ESP_LOGE(TAG, "WiFi scan completed with driver error");
        return output;
    }

    // Get scan results
    std::vector<wifi_ap_record_t> ap_records;
    if (!CleanupScanList(ap_records)) {
        owner.RetainCompletionAndRecover(lease);
        ESP_LOGE(TAG, "WiFi scan result cleanup failed; recovery scheduled");
        return output;
    }

    try {
        for (size_t i = 0; i < ap_records.size() && i < 20; ++i) {
            WifiScanResult result;
            result.ssid = std::string(reinterpret_cast<char*>(ap_records[i].ssid));
            result.rssi = ap_records[i].rssi;
            result.is_encrypted = (ap_records[i].authmode != WIFI_AUTH_OPEN);

            // Skip empty SSIDs
            if (!result.ssid.empty()) {
                output.networks.push_back(result);
            }
        }
    } catch (...) {
        output.networks.clear();
    }

    if (!owner.FinishNormally(lease)) {
        owner.RetainCompletionAndRecover(lease);
        output.networks.clear();
        ESP_LOGE(TAG, "WiFi scan ownership completion failed");
        return output;
    }

    output.failed = false;
    ESP_LOGI(TAG, "Found %d WiFi networks", (int)output.networks.size());
    return output;
}

void WifiConfigUI::ShowScanResults() {
    DrawWifiList(scan_results_, selected_index_, scroll_offset_);
}

void WifiConfigUI::ShowPasswordInput() {
    // Only clear password and set state on first entry (not on redraw)
    if (state_ != WifiConfigState::InputPassword) {
        state_ = WifiConfigState::InputPassword;
        input_password_.clear();
    }

    RedrawPasswordInput();
}

void WifiConfigUI::RedrawPasswordInput() {
    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);

    DrawHeader("输入密码");

    // Show selected SSID
    lv_obj_t* label = lv_label_create(canvas);
    lv_label_set_text_fmt(label, "连接: %s", selected_ssid_.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 5, 5);

    lv_obj_t* pwd_label = lv_label_create(canvas);
    lv_label_set_text(pwd_label, "请输入密码:");
    lv_obj_set_style_text_color(pwd_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(pwd_label, LV_ALIGN_TOP_LEFT, 5, 30);

    lv_obj_t* input_label = lv_label_create(canvas);
    std::string display_pwd(input_password_.length(), '*');
    display_pwd += cursor_visible_ ? "_" : " ";
    lv_label_set_text_fmt(input_label, ">>> %s", display_pwd.c_str());
    lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(input_label, LV_ALIGN_TOP_LEFT, 5, 55);

    DrawFooter("Enter:确认 Esc:返回");
}

void WifiConfigUI::ShowManualInput() {
    // Only clear inputs and set state on first entry (not on redraw)
    if (state_ != WifiConfigState::InputSsid && state_ != WifiConfigState::InputManualPwd) {
        state_ = WifiConfigState::InputSsid;
        input_ssid_.clear();
        input_password_.clear();
        input_focus_on_password_ = false;
    }

    RedrawManualInput();
}

void WifiConfigUI::RedrawManualInput() {
    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);

    DrawHeader("手动设置 WiFi");

    lv_obj_t* ssid_label = lv_label_create(canvas);
    lv_label_set_text(ssid_label, "SSID:");
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 5, 25);

    lv_obj_t* ssid_input = lv_label_create(canvas);
    std::string ssid_display = ">>> " + input_ssid_;
    if (!input_focus_on_password_) {
        ssid_display += cursor_visible_ ? "_" : " ";
    }
    lv_label_set_text(ssid_input, ssid_display.c_str());
    lv_obj_set_style_text_color(ssid_input, input_focus_on_password_ ? lv_color_hex(0x888888) : lv_color_hex(0xFFFF00), 0);
    lv_obj_align(ssid_input, LV_ALIGN_TOP_LEFT, 5, 45);

    lv_obj_t* pwd_label = lv_label_create(canvas);
    lv_label_set_text(pwd_label, "密码:");
    lv_obj_set_style_text_color(pwd_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(pwd_label, LV_ALIGN_TOP_LEFT, 5, 70);

    lv_obj_t* pwd_input = lv_label_create(canvas);
    std::string pwd_display = ">>> " + std::string(input_password_.length(), '*');
    if (input_focus_on_password_) {
        pwd_display += cursor_visible_ ? "_" : " ";
    }
    lv_label_set_text(pwd_input, pwd_display.c_str());
    lv_obj_set_style_text_color(pwd_input, input_focus_on_password_ ? lv_color_hex(0xFFFF00) : lv_color_hex(0x888888), 0);
    lv_obj_align(pwd_input, LV_ALIGN_TOP_LEFT, 5, 90);

    DrawFooter("Tab:切换 Enter:确认 Esc:返回");
}

void WifiConfigUI::ShowSavedList() {
    state_ = WifiConfigState::SavedList;
    saved_selected_index_ = 0;
    saved_scroll_offset_ = 0;

    LoadSavedWifiList();
    DrawSavedWifiList();
}

void WifiConfigUI::DrawSavedWifiList() {
    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);

    char title[48];
    snprintf(title, sizeof(title), "已保存的 WiFi (%d/10)", (int)saved_wifi_list_.size());
    DrawHeader(title);

    if (saved_wifi_list_.empty()) {
        lv_obj_t* empty_label = lv_label_create(canvas);
        lv_label_set_text(empty_label, "没有已保存的 WiFi");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x888888), 0);
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
        DrawFooter("Esc:返回");
        return;
    }

    int y_offset = 25;
    int visible_count = std::min((int)saved_wifi_list_.size() - saved_scroll_offset_, MAX_VISIBLE_ITEMS);

    for (int i = 0; i < visible_count; i++) {
        int idx = saved_scroll_offset_ + i;
        bool is_selected = (idx == saved_selected_index_);

        lv_obj_t* item_label = lv_label_create(canvas);
        char item_text[48];
        snprintf(item_text, sizeof(item_text), "%s %d. %s",
                 is_selected ? ">" : " ",
                 idx + 1,
                 saved_wifi_list_[idx].first.c_str());
        lv_label_set_text(item_label, item_text);
        lv_obj_set_style_text_color(item_label, is_selected ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(item_label, LV_ALIGN_TOP_LEFT, 5, y_offset);
        y_offset += 20;
    }

    DrawFooter("↑↓:选择 Enter:连接 Del:删除 Esc:返回");
}

void WifiConfigUI::ShowConnecting() {
    state_ = WifiConfigState::Connecting;

    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);

    DrawHeader("连接中...");

    lv_obj_t* ssid_label = lv_label_create(canvas);
    lv_label_set_text_fmt(ssid_label, "正在连接: %s", selected_ssid_.c_str());
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, 0);

    DrawFooter("请稍候...");
}

void WifiConfigUI::ShowSuccess() {
    state_ = WifiConfigState::Success;

    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);

    DrawHeader("连接成功!");

    lv_obj_t* ssid_label = lv_label_create(canvas);
    lv_label_set_text_fmt(ssid_label, "已连接: %s", selected_ssid_.c_str());
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0x00FF00), 0);
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t* saved_label = lv_label_create(canvas);
    lv_label_set_text(saved_label, "WiFi 配置已保存");
    lv_obj_set_style_text_color(saved_label, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(saved_label, LV_ALIGN_CENTER, 0, 15);

    DrawFooter("Enter:继续");
}

void WifiConfigUI::ShowFailed() {
    state_ = WifiConfigState::Failed;

    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);

    DrawHeader("连接失败");

    lv_obj_t* ssid_label = lv_label_create(canvas);
    lv_label_set_text_fmt(ssid_label, "无法连接: %s", selected_ssid_.c_str());
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFF0000), 0);
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, 0);

    DrawFooter("Enter:重试 Esc:返回");
}

void WifiConfigUI::DrawHeader(const char* title) {
    lv_obj_t* canvas = lv_scr_act();

    lv_obj_t* header = lv_label_create(canvas);
    lv_label_set_text(header, title);
    lv_obj_set_style_text_color(header, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 5, 2);
}

void WifiConfigUI::DrawFooter(const char* hint) {
    lv_obj_t* canvas = lv_scr_act();

    lv_obj_t* footer = lv_label_create(canvas);
    lv_label_set_text(footer, hint);
    lv_obj_set_style_text_color(footer, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 5, -2);
}

void WifiConfigUI::DrawWifiList(const std::vector<WifiScanResult>& list, int selected, int scroll) {
    lv_obj_t* canvas = lv_scr_act();
    lv_obj_clean(canvas);

    DrawHeader("选择 WiFi");

    int y_offset = 25;
    int visible_count = std::min((int)list.size() - scroll, MAX_VISIBLE_ITEMS);

    for (int i = 0; i < visible_count; i++) {
        int idx = scroll + i;
        bool is_selected = (idx == selected);
        const WifiScanResult& wifi = list[idx];

        lv_obj_t* item_label = lv_label_create(canvas);
        std::string signal = GetSignalBars(wifi.rssi);
        char item_text[64];
        snprintf(item_text, sizeof(item_text), "%s%d.%-12s %4ddBm %s",
                 is_selected ? ">" : " ",
                 idx + 1,
                 wifi.ssid.substr(0, 12).c_str(),
                 wifi.rssi,
                 signal.c_str());
        lv_label_set_text(item_label, item_text);
        lv_obj_set_style_text_color(item_label, is_selected ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(item_label, LV_ALIGN_TOP_LEFT, 2, y_offset);
        y_offset += 20;
    }

    DrawFooter("↑↓:选择 Enter:连接 W:手动 S:已保存");
}

std::string WifiConfigUI::GetSignalBars(int8_t rssi) {
    if (rssi >= -50) return "████";
    if (rssi >= -60) return "███░";
    if (rssi >= -70) return "██░░";
    if (rssi >= -80) return "█░░░";
    return "░░░░";
}

void WifiConfigUI::LoadSavedWifiList() {
    saved_wifi_list_.clear();
    auto& ssid_manager = SsidManager::GetInstance();
    const auto& ssid_list = ssid_manager.GetSsidList();

    for (const auto& item : ssid_list) {
        saved_wifi_list_.push_back({item.ssid, item.password});
    }
}

void WifiConfigUI::DeleteSavedWifi(int index) {
    if (index >= 0 && index < (int)saved_wifi_list_.size()) {
        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.RemoveSsid(index);
        ESP_LOGI(TAG, "Deleted saved WiFi at index: %d", index);
        LoadSavedWifiList();
    }
}

void WifiConfigUI::AttemptConnection() {
    ShowConnecting();

    if (connect_callback_) {
        connect_callback_(selected_ssid_, input_password_);
    }
}

void WifiConfigUI::OnConnectResult(bool success) {
    if (success) {
        ShowSuccess();
    } else {
        ShowFailed();
    }
}

WifiConfigResult WifiConfigUI::HandleKeyEvent(const KeyEvent& event) {
    // Only handle key press events, skip modifiers
    if (!event.pressed || event.is_modifier) {
        return WifiConfigResult::None;
    }

    // Check for ESC to cancel from Scanning or SelectWifi states
    // (other states handle ESC in their own handlers to navigate back)
    if (event.key_code == KC_ESC) {
        if (state_ == WifiConfigState::Scanning ||
            state_ == WifiConfigState::SelectWifi) {
            is_active_ = false;
            return WifiConfigResult::Cancelled;
        }
    }

    // Check if not active (was cancelled in a handler)
    if (!is_active_) {
        return WifiConfigResult::Cancelled;
    }

    switch (state_) {
        case WifiConfigState::Scanning:
            HandleScanningKey(event);
            break;
        case WifiConfigState::SelectWifi:
            HandleSelectWifiKey(event);
            break;
        case WifiConfigState::InputPassword:
            HandlePasswordInputKey(event);
            break;
        case WifiConfigState::InputSsid:
        case WifiConfigState::InputManualPwd:
            HandleManualInputKey(event);
            break;
        case WifiConfigState::SavedList:
            HandleSavedListKey(event);
            break;
        case WifiConfigState::Connecting:
            HandleConnectingKey(event);
            break;
        case WifiConfigState::Success:
            HandleResultKey(event);
            if (event.key_code == KC_ENTER) {
                is_active_ = false;
                return WifiConfigResult::Connected;
            }
            break;
        case WifiConfigState::Failed:
            HandleResultKey(event);
            break;
    }

    // Check if cancelled by a handler
    if (!is_active_) {
        return WifiConfigResult::Cancelled;
    }

    return WifiConfigResult::None;
}

void WifiConfigUI::HandleScanningKey(const KeyEvent& event) {
    if (event.key_code == KC_W) {
        DismissPendingScanResult();
        ShowManualInput();
    } else if (event.key_code == KC_S) {
        DismissPendingScanResult();
        ShowSavedList();
    }
    // ESC is handled in HandleKeyEvent
}

void WifiConfigUI::HandleSelectWifiKey(const KeyEvent& event) {
    switch (event.key_code) {
        case KC_UP:
        case KC_SEMICOLON:  // ; key as UP
            if (selected_index_ > 0) {
                selected_index_--;
                if (selected_index_ < scroll_offset_) {
                    scroll_offset_ = selected_index_;
                }
                ShowScanResults();
            }
            break;

        case KC_DOWN:
        case KC_DOT:  // . key as DOWN
            if (selected_index_ < (int)scan_results_.size() - 1) {
                selected_index_++;
                if (selected_index_ >= scroll_offset_ + MAX_VISIBLE_ITEMS) {
                    scroll_offset_ = selected_index_ - MAX_VISIBLE_ITEMS + 1;
                }
                ShowScanResults();
            }
            break;

        case KC_ENTER:
            if (!scan_results_.empty()) {
                selected_ssid_ = scan_results_[selected_index_].ssid;
                ShowPasswordInput();
            }
            break;

        case KC_W:
            ShowManualInput();
            break;

        case KC_S:
            ShowSavedList();
            break;

        default:
            break;
    }
    // ESC is handled in HandleKeyEvent
}

void WifiConfigUI::HandlePasswordInputKey(const KeyEvent& event) {
    switch (event.key_code) {
        case KC_ENTER:
            if (!input_password_.empty()) {
                AttemptConnection();
            }
            break;

        case KC_ESC:
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
            break;

        case KC_BACKSPACE:
            if (!input_password_.empty()) {
                input_password_.pop_back();
                RedrawPasswordInput();
            }
            break;

        case KC_SPACE:
            if (AppendWifiFieldIfFits(input_password_, " ",
                                      kMaxWifiPasswordBytes)) {
                RedrawPasswordInput();
            }
            break;

        default:
            // Add character if it's a printable key
            if (AppendWifiFieldIfFits(input_password_, event.key_char,
                                      kMaxWifiPasswordBytes)) {
                RedrawPasswordInput();
            }
            break;
    }
}

void WifiConfigUI::HandleManualInputKey(const KeyEvent& event) {
    std::string* current_input = input_focus_on_password_ ? &input_password_ : &input_ssid_;
    const size_t max_input_bytes = input_focus_on_password_
        ? kMaxWifiPasswordBytes
        : kMaxWifiSsidBytes;

    switch (event.key_code) {
        case KC_TAB:
            input_focus_on_password_ = !input_focus_on_password_;
            if (input_focus_on_password_) {
                state_ = WifiConfigState::InputManualPwd;
            } else {
                state_ = WifiConfigState::InputSsid;
            }
            RedrawManualInput();
            break;

        case KC_ENTER:
            if (!input_ssid_.empty()) {
                selected_ssid_ = input_ssid_;
                AttemptConnection();
            }
            break;

        case KC_ESC:
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
            break;

        case KC_BACKSPACE:
            if (!current_input->empty()) {
                current_input->pop_back();
                RedrawManualInput();
            }
            break;

        case KC_SPACE:
            if (AppendWifiFieldIfFits(*current_input, " ", max_input_bytes)) {
                RedrawManualInput();
            }
            break;

        default:
            // Add character if it's a printable key
            if (AppendWifiFieldIfFits(*current_input, event.key_char,
                                      max_input_bytes)) {
                RedrawManualInput();
            }
            break;
    }
}

void WifiConfigUI::HandleSavedListKey(const KeyEvent& event) {
    switch (event.key_code) {
        case KC_UP:
        case KC_SEMICOLON:
            if (saved_selected_index_ > 0) {
                saved_selected_index_--;
                if (saved_selected_index_ < saved_scroll_offset_) {
                    saved_scroll_offset_ = saved_selected_index_;
                }
                DrawSavedWifiList();
            }
            break;

        case KC_DOWN:
        case KC_DOT:
            if (saved_selected_index_ < (int)saved_wifi_list_.size() - 1) {
                saved_selected_index_++;
                if (saved_selected_index_ >= saved_scroll_offset_ + MAX_VISIBLE_ITEMS) {
                    saved_scroll_offset_ = saved_selected_index_ - MAX_VISIBLE_ITEMS + 1;
                }
                DrawSavedWifiList();
            }
            break;

        case KC_ENTER:
            if (!saved_wifi_list_.empty()) {
                selected_ssid_ = saved_wifi_list_[saved_selected_index_].first;
                input_password_ = saved_wifi_list_[saved_selected_index_].second;
                AttemptConnection();
            }
            break;

        case KC_BACKSPACE:  // Del key for delete
            if (!saved_wifi_list_.empty()) {
                DeleteSavedWifi(saved_selected_index_);
                if (saved_selected_index_ >= (int)saved_wifi_list_.size() && saved_selected_index_ > 0) {
                    saved_selected_index_--;
                }
                DrawSavedWifiList();
            }
            break;

        case KC_ESC:
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
            break;

        default:
            break;
    }
}

void WifiConfigUI::HandleConnectingKey(const KeyEvent& event) {
    // No key handling during connection
    (void)event;
}

void WifiConfigUI::HandleResultKey(const KeyEvent& event) {
    if (state_ == WifiConfigState::Success) {
        if (event.key_code == KC_ENTER) {
            // Will be handled in HandleKeyEvent to return Connected
        }
    } else if (state_ == WifiConfigState::Failed) {
        if (event.key_code == KC_ENTER) {
            // Retry - go back to password input (keep password for retry)
            state_ = WifiConfigState::InputPassword;
            RedrawPasswordInput();
        } else if (event.key_code == KC_ESC) {
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
        }
    }
}

void WifiConfigUI::Poll() {
    auto& owner = BlockingWifiScanOwner::GetInstance();
    const auto retry = owner.PeekRecoveryRetry(ui_generation_);
    if (is_active_ && state_ == WifiConfigState::Scanning &&
        retry.has_value()) {
        StartScanning();
        return;
    }
    uint32_t now = esp_log_timestamp();
    if (now - last_cursor_toggle_ >= CURSOR_BLINK_MS) {
        cursor_visible_ = !cursor_visible_;
        last_cursor_toggle_ = now;

        // Refresh display for input states (use Redraw functions to avoid clearing input)
        if (state_ == WifiConfigState::InputPassword) {
            RedrawPasswordInput();
        } else if (state_ == WifiConfigState::InputSsid || state_ == WifiConfigState::InputManualPwd) {
            RedrawManualInput();
        }
    }
}
