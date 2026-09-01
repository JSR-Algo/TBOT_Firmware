#include "ssid_manager.h"

#include <nvs_flash.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

std::map<std::string, std::string> durable_nvs;
int nvs_commit_calls = 0;

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

    const uint32_t stale =
        manager.BeginSsidTransaction("stale", "stale-password");
    assert(stale != 0);
    const uint32_t winner =
        manager.BeginSsidTransaction("winner", "winner-password");
    assert(winner != 0 && winner != stale);
    assert(!manager.CommitSsidTransaction(stale));
    assert(manager.CommitSsidTransaction(winner));
    assert(nvs_commit_calls == 1);
    const auto committed = manager.GetSsidList();
    assert(committed.size() == 10);
    assert(committed.front().ssid == "winner");
    assert(committed.front().password == "winner-password");
    for (const auto& item : committed) {
        assert(item.ssid != "stale");
    }

    std::cout << "ssid manager transaction host tests: PASS\n";
    return 0;
}
