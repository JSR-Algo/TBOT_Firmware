#include "ssid_manager.h"

#include <algorithm>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "SsidManager"
#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10

SsidManager::SsidManager() {
    LoadFromNvs();
}

SsidManager::~SsidManager() {
    for (auto& item : ssid_list_) {
        SecureClearString(item.ssid);
        SecureClearString(item.password);
    }
    ClearTransactionBackup();
}

SsidMutationResult SsidManager::Clear() {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (active_transaction_id_ != 0) {
        return SsidMutationResult::kBusy;
    }
    return ClearLocked();
}

SsidMutationResult SsidManager::ForceClearAndCancelTransaction() {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (active_transaction_id_ != 0) {
        RestoreActiveTransaction();
    }
    ClearTransactionBackup();
    active_transaction_id_ = 0;
    return ClearLocked();
}

SsidMutationResult SsidManager::ClearLocked() {
    auto previous_list = ssid_list_;
    for (auto& item : ssid_list_) {
        SecureClearString(item.ssid);
        SecureClearString(item.password);
    }
    ssid_list_.clear();
    return PersistMutationOrRestore(previous_list);
}

std::vector<SsidItem> SsidManager::GetSsidList() const {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    return ssid_list_;
}

void SsidManager::LoadFromNvs() {
    ssid_list_.clear();

    // Load ssid and password from NVS from namespace "wifi"
    // ssid, ssid1, ssid2, ... ssid9
    // password, password1, password2, ... password9
    nvs_handle_t nvs_handle;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        // The namespace doesn't exist, just return
        ESP_LOGW(TAG, "NVS namespace %s doesn't exist", NVS_NAMESPACE);
        return;
    }
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }

        char ssid[33];
        char password[65];
        size_t length = sizeof(ssid);
        if (nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &length) != ESP_OK) {
            continue;
        }
        length = sizeof(password);
        if (nvs_get_str(nvs_handle, password_key.c_str(), password, &length) != ESP_OK) {
            continue;
        }
        if (ssid[0] == '\0') {
            ESP_LOGW(TAG, "Ignore empty SSID in NVS key %s", ssid_key.c_str());
            continue;
        }
        ssid_list_.push_back({ssid, password});
    }
    nvs_close(nvs_handle);
}

bool SsidManager::SaveToNvs() {
    nvs_handle_t nvs_handle;
    esp_err_t first_error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (first_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WiFi NVS for write: %s", esp_err_to_name(first_error));
        return false;
    }
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }

        if (i < ssid_list_.size()) {
            esp_err_t err = nvs_set_str(nvs_handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str());
            if (first_error == ESP_OK && err != ESP_OK) {
                first_error = err;
            }
            err = nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str());
            if (first_error == ESP_OK && err != ESP_OK) {
                first_error = err;
            }
        } else {
            esp_err_t err = nvs_erase_key(nvs_handle, ssid_key.c_str());
            if (first_error == ESP_OK && err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                first_error = err;
            }
            err = nvs_erase_key(nvs_handle, password_key.c_str());
            if (first_error == ESP_OK && err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                first_error = err;
            }
        }
    }
    if (first_error == ESP_OK) {
        first_error = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (first_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist WiFi credentials: %s", esp_err_to_name(first_error));
        return false;
    }
    return true;
}

SsidMutationResult SsidManager::PersistMutationOrRestore(
        std::vector<SsidItem>& previous_list) {
    if (SaveToNvs()) {
        for (auto& item : previous_list) {
            SecureClearString(item.ssid);
            SecureClearString(item.password);
        }
        return SsidMutationResult::kApplied;
    }

    for (auto& item : ssid_list_) {
        SecureClearString(item.ssid);
        SecureClearString(item.password);
    }
    ssid_list_.swap(previous_list);
    if (!SaveToNvs()) {
        ESP_LOGE(TAG, "Compensating WiFi credential restore failed");
    }
    return SsidMutationResult::kPersistenceFailed;
}

SsidMutationResult SsidManager::AddSsid(const std::string& ssid,
                                        const std::string& password) {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (active_transaction_id_ != 0) {
        return SsidMutationResult::kBusy;
    }
    if (ssid.empty()) {
        ESP_LOGW(TAG, "Ignore empty SSID");
        return SsidMutationResult::kInvalid;
    }
    auto previous_list = ssid_list_;
    UpsertSsid(ssid, password);
    return PersistMutationOrRestore(previous_list);
}

uint32_t SsidManager::BeginSsidTransaction(const std::string& ssid,
                                           const std::string& password) {
    if (ssid.empty()) {
        ESP_LOGW(TAG, "Ignore empty SSID transaction");
        return 0;
    }

    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (active_transaction_id_ != 0) {
        // The active transaction owns the provisional list until its exact
        // commit or rollback releases ownership.
        return 0;
    }

    ClearTransactionBackup();
    uint32_t transaction_id = next_transaction_id_.fetch_add(1) + 1;
    if (transaction_id == 0) {
        transaction_id = next_transaction_id_.fetch_add(1) + 1;
    }
    active_transaction_id_ = transaction_id;

    for (size_t index = 0; index < ssid_list_.size(); ++index) {
        if (ssid_list_[index].ssid == ssid) {
            transaction_matched_existing_ = true;
            transaction_match_index_ = index;
            transaction_old_password_ = ssid_list_[index].password;
            SecureClearString(ssid_list_[index].password);
            ssid_list_[index].password = password;
            return transaction_id;
        }
    }

    transaction_inserted_ = true;
    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
        transaction_evicted_tail_ = true;
        transaction_evicted_item_ = std::move(ssid_list_.back());
        ssid_list_.pop_back();
    }
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
    return transaction_id;
}

bool SsidManager::CommitSsidTransaction(uint32_t transaction_id) {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (transaction_id == 0 || active_transaction_id_ != transaction_id) {
        return false;
    }

    if (!SaveToNvs()) {
        RestoreActiveTransaction();
        if (!SaveToNvs()) {
            ESP_LOGE(TAG, "Compensating WiFi credential restore failed");
        }
        ClearTransactionBackup();
        active_transaction_id_ = 0;
        return false;
    }
    ClearTransactionBackup();
    active_transaction_id_ = 0;
    return true;
}

bool SsidManager::RollbackSsidTransaction(uint32_t transaction_id) {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (transaction_id == 0 || active_transaction_id_ != transaction_id) {
        return false;
    }

    RestoreActiveTransaction();
    ClearTransactionBackup();
    active_transaction_id_ = 0;
    return true;
}

void SsidManager::UpsertSsid(const std::string& ssid, const std::string& password) {

    for (auto& item : ssid_list_) {
        ESP_LOGI(TAG, "Comparing stored SSID len=%u with incoming len=%u",
                 static_cast<unsigned>(item.ssid.size()),
                 static_cast<unsigned>(ssid.size()));
        if (item.ssid == ssid) {
            ESP_LOGW(TAG, "Existing SSID matched; overwriting credentials");
            SecureClearString(item.password);
            item.password = password;
            return;
        }
    }

    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
        ESP_LOGW(TAG, "SSID list is full, pop one");
        SecureClearString(ssid_list_.back().ssid);
        SecureClearString(ssid_list_.back().password);
        ssid_list_.pop_back();
    }
    // Add the new ssid to the front of the list
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
}

void SsidManager::RestoreActiveTransaction() {
    if (transaction_matched_existing_) {
        if (transaction_match_index_ < ssid_list_.size()) {
            SecureClearString(ssid_list_[transaction_match_index_].password);
            ssid_list_[transaction_match_index_].password = transaction_old_password_;
        }
        return;
    }

    if (transaction_inserted_ && !ssid_list_.empty()) {
        SecureClearString(ssid_list_.front().ssid);
        SecureClearString(ssid_list_.front().password);
        ssid_list_.erase(ssid_list_.begin());
        if (transaction_evicted_tail_) {
            ssid_list_.push_back(transaction_evicted_item_);
        }
    }
}

void SsidManager::ClearTransactionBackup() {
    SecureClearString(transaction_old_password_);
    SecureClearString(transaction_evicted_item_.ssid);
    SecureClearString(transaction_evicted_item_.password);
    transaction_matched_existing_ = false;
    transaction_match_index_ = 0;
    transaction_inserted_ = false;
    transaction_evicted_tail_ = false;
}

void SsidManager::SecureClearString(std::string& value) {
    if (!value.empty()) {
        volatile char* bytes = &value[0];
        for (size_t i = 0; i < value.size(); ++i) {
            bytes[i] = '\0';
        }
    }
    value.clear();
}

SsidMutationResult SsidManager::RemoveSsid(int index) {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (active_transaction_id_ != 0) {
        return SsidMutationResult::kBusy;
    }
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return SsidMutationResult::kInvalid;
    }
    auto previous_list = ssid_list_;
    SecureClearString(ssid_list_[index].ssid);
    SecureClearString(ssid_list_[index].password);
    ssid_list_.erase(ssid_list_.begin() + index);
    return PersistMutationOrRestore(previous_list);
}

SsidMutationResult SsidManager::SetDefaultSsid(int index) {
    std::lock_guard<std::mutex> lock(transaction_mutex_);
    if (active_transaction_id_ != 0) {
        return SsidMutationResult::kBusy;
    }
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return SsidMutationResult::kInvalid;
    }
    auto previous_list = ssid_list_;
    // Move the ssid at index to the front of the list
    auto item = std::move(ssid_list_[index]);
    ssid_list_.erase(ssid_list_.begin() + index);
    ssid_list_.insert(ssid_list_.begin(), item);
    return PersistMutationOrRestore(previous_list);
}
