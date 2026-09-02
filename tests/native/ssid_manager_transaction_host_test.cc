#include "ssid_manager.h"
#include "../../main/boards/m5stack-cardputer-adv/cardputer_wifi_deferred_intent_state.h"

#include <nvs_flash.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

std::map<std::string, std::string> durable_nvs;
int nvs_commit_calls = 0;
bool fail_next_commit = false;

std::string IndexedKey(const char* prefix, int index) {
    return index == 0 ? prefix : std::string(prefix) + std::to_string(index);
}

void SeedTenNetworks() {
    for (int index = 0; index < 10; ++index) {
        durable_nvs[IndexedKey("ssid", index)] =
            "saved-" + std::to_string(index);
        durable_nvs[IndexedKey("password", index)] =
            "password-" + std::to_string(index);
    }
}

void AssertOriginalList(const std::vector<SsidItem>& list) {
    assert(list.size() == 10);
    for (int index = 0; index < 10; ++index) {
        assert(list[index].ssid == "saved-" + std::to_string(index));
        assert(list[index].password ==
               "password-" + std::to_string(index));
    }
}

void AssertSameList(const std::vector<SsidItem>& left,
                    const std::vector<SsidItem>& right) {
    assert(left.size() == right.size());
    for (size_t index = 0; index < left.size(); ++index) {
        assert(left[index].ssid == right[index].ssid);
        assert(left[index].password == right[index].password);
    }
}

}  // namespace

esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* handle) {
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* value,
                      size_t* length) {
    const auto found = durable_nvs.find(key);
    if (found == durable_nvs.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const size_t required = found->second.size() + 1;
    if (*length < required) {
        *length = required;
        return ESP_FAIL;
    }
    std::memcpy(value, found->second.c_str(), required);
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value) {
    durable_nvs[key] = value;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t, const char* key) {
    return durable_nvs.erase(key) == 0 ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t) {
    ++nvs_commit_calls;
    if (fail_next_commit) {
        fail_next_commit = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t) {}

const char* esp_err_to_name(esp_err_t) {
    return "host-test";
}

int main() {
    SeedTenNetworks();
    auto& manager = SsidManager::GetInstance();
    AssertOriginalList(manager.GetSsidList());

    const uint32_t failed =
        manager.BeginSsidTransaction("candidate", "wrong-password");
    assert(failed != 0);
    assert(nvs_commit_calls == 0);
    const auto provisional = manager.GetSsidList();
    assert(provisional.size() == 10);
    assert(provisional.front().ssid == "candidate");
    assert(provisional.back().ssid == "saved-8");
    assert(manager.RollbackSsidTransaction(failed));
    assert(nvs_commit_calls == 0);
    AssertOriginalList(manager.GetSsidList());

    const uint32_t owner =
        manager.BeginSsidTransaction("owner", "owner-password");
    assert(owner != 0);

    CardputerWifiDeferredIntentState cardputer;
    assert(cardputer.PublishCredentials(1, "cardputer", "cardputer-password"));
    cardputer.ObserveWorkerCreation(true);
    assert(cardputer.ArmNotification());
    const auto cardputer_intent = cardputer.TakeNotified();
    assert(cardputer_intent.has_value());
    const uint32_t cardputer_transaction = manager.BeginSsidTransaction(
        cardputer_intent->ssid, cardputer_intent->password);
    assert(cardputer_transaction == 0);
    assert(cardputer.StoreConnectionResult(*cardputer_intent, false));
    assert(!cardputer.RetryInFlight(*cardputer_intent));
    const auto cardputer_result = cardputer.ClaimResultForDelivery();
    assert(cardputer_result.has_value() && !cardputer_result->connected);
    assert(cardputer.CompleteResult(*cardputer_result));
    const auto owner_still_active = manager.GetSsidList();
    assert(owner_still_active.front().ssid == "owner");
    assert(owner_still_active.front().password == "owner-password");

    const uint32_t blocked =
        manager.BeginSsidTransaction("blocked", "blocked-password");
    assert(blocked == 0);
    assert(manager.CommitSsidTransaction(owner));
    assert(nvs_commit_calls == 1);
    const auto committed = manager.GetSsidList();
    assert(committed.size() == 10);
    assert(committed.front().ssid == "owner");
    assert(committed.front().password == "owner-password");
    for (const auto& item : committed) {
        assert(item.ssid != "blocked");
    }

    const uint32_t cancelled =
        manager.BeginSsidTransaction("cancelled", "cancelled-password");
    assert(cancelled != 0);
    assert(manager.RollbackSsidTransaction(cancelled));
    const uint32_t after_cancel =
        manager.BeginSsidTransaction("after-cancel", "new-password");
    assert(after_cancel != 0);
    assert(manager.RollbackSsidTransaction(after_cancel));

    uint32_t first = 0;
    uint32_t second = 0;
    std::thread first_owner([&]() {
        first = manager.BeginSsidTransaction("concurrent-a", "password-a");
    });
    std::thread second_owner([&]() {
        second = manager.BeginSsidTransaction("concurrent-b", "password-b");
    });
    first_owner.join();
    second_owner.join();
    assert((first == 0) != (second == 0));
    const uint32_t concurrent_owner = first != 0 ? first : second;
    assert(manager.RollbackSsidTransaction(concurrent_owner));

    const uint32_t after_success =
        manager.BeginSsidTransaction("after-success", "final-password");
    assert(after_success != 0);
    assert(manager.RollbackSsidTransaction(after_success));

    const std::string max_ssid(32, 's');
    const std::string max_password(63, 'p');
    assert(manager.AddSsid(max_ssid, max_password) ==
           SsidMutationResult::kApplied);
    const auto after_max_credentials = manager.GetSsidList();
    assert(after_max_credentials.front().ssid == max_ssid);
    assert(after_max_credentials.front().password == max_password);
    const auto durable_after_max_credentials = durable_nvs;
    assert(manager.AddSsid(std::string(33, 's'), "password") ==
           SsidMutationResult::kInvalid);
    assert(manager.AddSsid("ssid", std::string(64, 'p')) ==
           SsidMutationResult::kInvalid);
    assert(manager.BeginSsidTransaction(std::string(33, 's'), "password") == 0);
    assert(manager.BeginSsidTransaction("ssid", std::string(64, 'p')) == 0);
    AssertSameList(manager.GetSsidList(), after_max_credentials);
    assert(durable_nvs == durable_after_max_credentials);

    const auto before_failed_add = manager.GetSsidList();
    const auto durable_before_failed_add = durable_nvs;
    fail_next_commit = true;
    assert(manager.AddSsid("failed-add", "failed-password") ==
           SsidMutationResult::kPersistenceFailed);
    AssertSameList(manager.GetSsidList(), before_failed_add);
    assert(durable_nvs == durable_before_failed_add);

    const auto before_failed_remove = manager.GetSsidList();
    const auto durable_before_failed_remove = durable_nvs;
    fail_next_commit = true;
    assert(manager.RemoveSsid(4) == SsidMutationResult::kPersistenceFailed);
    AssertSameList(manager.GetSsidList(), before_failed_remove);
    assert(durable_nvs == durable_before_failed_remove);

    const auto before_failed_default = manager.GetSsidList();
    const auto durable_before_failed_default = durable_nvs;
    fail_next_commit = true;
    assert(manager.SetDefaultSsid(6) ==
           SsidMutationResult::kPersistenceFailed);
    AssertSameList(manager.GetSsidList(), before_failed_default);
    assert(durable_nvs == durable_before_failed_default);

    const auto before_failed_clear = manager.GetSsidList();
    const auto durable_before_failed_clear = durable_nvs;
    fail_next_commit = true;
    assert(manager.Clear() == SsidMutationResult::kPersistenceFailed);
    AssertSameList(manager.GetSsidList(), before_failed_clear);
    assert(durable_nvs == durable_before_failed_clear);

    const uint32_t existing_owner = manager.BeginSsidTransaction(
        "saved-4", "provisional-existing-password");
    assert(existing_owner != 0);
    const auto existing_provisional = manager.GetSsidList();
    const auto durable_before_busy_mutations = durable_nvs;
    assert(manager.AddSsid("legacy-add", "legacy-password") ==
           SsidMutationResult::kBusy);
    assert(manager.RemoveSsid(2) == SsidMutationResult::kBusy);
    assert(manager.SetDefaultSsid(6) == SsidMutationResult::kBusy);
    assert(manager.Clear() == SsidMutationResult::kBusy);
    AssertSameList(manager.GetSsidList(), existing_provisional);
    assert(durable_nvs == durable_before_busy_mutations);
    assert(manager.RollbackSsidTransaction(existing_owner));

    const uint32_t mutation_owner = manager.BeginSsidTransaction(
        "saved-4", "concurrent-provisional-password");
    assert(mutation_owner != 0);
    SsidMutationResult concurrent_add = SsidMutationResult::kApplied;
    SsidMutationResult concurrent_remove = SsidMutationResult::kApplied;
    std::thread add_mutation([&]() {
        concurrent_add = manager.AddSsid("concurrent-add", "password");
    });
    std::thread remove_mutation([&]() {
        concurrent_remove = manager.RemoveSsid(3);
    });
    add_mutation.join();
    remove_mutation.join();
    assert(concurrent_add == SsidMutationResult::kBusy);
    assert(concurrent_remove == SsidMutationResult::kBusy);
    assert(manager.CommitSsidTransaction(mutation_owner));
    const auto committed_existing = manager.GetSsidList();
    const auto committed_match = std::find_if(
        committed_existing.begin(), committed_existing.end(),
        [](const SsidItem& item) { return item.ssid == "saved-4"; });
    assert(committed_match != committed_existing.end());
    assert(committed_match != committed_existing.begin());
    assert(committed_match->password == "concurrent-provisional-password");

    const auto before_failed_force_clear = manager.GetSsidList();
    const auto durable_before_failed_force_clear = durable_nvs;
    const uint32_t failed_force_cancel = manager.BeginSsidTransaction(
        "failed-force-clear", "failed-force-password");
    assert(failed_force_cancel != 0);
    fail_next_commit = true;
    assert(manager.ForceClearAndCancelTransaction() ==
           SsidMutationResult::kPersistenceFailed);
    AssertSameList(manager.GetSsidList(), before_failed_force_clear);
    assert(durable_nvs == durable_before_failed_force_clear);
    assert(!manager.RollbackSsidTransaction(failed_force_cancel));

    const uint32_t force_cancelled = manager.BeginSsidTransaction(
        "force-cleared", "force-clear-password");
    assert(force_cancelled != 0);
    assert(manager.ForceClearAndCancelTransaction() ==
           SsidMutationResult::kApplied);
    assert(manager.GetSsidList().empty());
    assert(!manager.RollbackSsidTransaction(force_cancelled));
    assert(durable_nvs.empty());

    std::cout << "ssid manager transaction host tests: PASS\n";
    return 0;
}
