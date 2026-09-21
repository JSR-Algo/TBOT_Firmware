#define Settings ProductionSettings
#include "settings.h"
#undef SETTINGS_H
#undef Settings
#define Settings StagingSettings
#include "settings.h"
#undef Settings
#include <cassert>
#include <cstring>
#include <map>

using Values = std::map<std::string, std::string>;
static std::map<std::string, Values> committed;
static std::map<nvs_handle_t, std::pair<std::string, Values>> handles;
static nvs_handle_t next_handle = 1;
static bool fail_open = false;
int nvs_open(const char* ns, int, nvs_handle_t* out) {
    if (fail_open) return ESP_ERR_NVS_NOT_FOUND;
    *out = next_handle++;
    handles[*out] = {ns, committed[ns]};
    return ESP_OK;
}
int nvs_get_str(nvs_handle_t h, const char* k, char* out, size_t* size) {
    auto& values = handles.at(h).second;
    if (!values.count(k)) return ESP_ERR_NVS_NOT_FOUND;
    auto& value = values.at(k);
    if (out) { assert(*size >= value.size() + 1); std::memcpy(out, value.c_str(), value.size() + 1); }
    *size = value.size() + 1;
    return ESP_OK;
}
int nvs_set_str(nvs_handle_t h, const char* k, const char* value) {
    if (!handles.count(h)) return ESP_ERR_NVS_NOT_FOUND;
    handles.at(h).second[k] = value; return ESP_OK;
}
int nvs_get_i32(nvs_handle_t h, const char* k, int32_t* out) {
    if (!handles.at(h).second.count(k)) return ESP_ERR_NVS_NOT_FOUND;
    *out = std::stoi(handles.at(h).second.at(k)); return ESP_OK;
}
int nvs_set_i32(nvs_handle_t h, const char* k, int32_t value) { return nvs_set_str(h, k, std::to_string(value).c_str()); }
int nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* out) { int32_t value = 0; int result = nvs_get_i32(h, k, &value); *out = value; return result; }
int nvs_set_u8(nvs_handle_t h, const char* k, uint8_t value) { return nvs_set_i32(h, k, value); }
int nvs_erase_key(nvs_handle_t h, const char* k) { return handles.at(h).second.erase(k) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
int nvs_erase_all(nvs_handle_t h) { handles.at(h).second.clear(); return ESP_OK; }
int nvs_commit(nvs_handle_t h) { committed[handles.at(h).first] = handles.at(h).second; return ESP_OK; }
void nvs_close(nvs_handle_t h) { handles.erase(h); }

int main(int argc, char**) {
    fail_open = true;
    {
        StagingSettings missing("backend", argc > 1);
        assert(missing.GetString("device_secret").empty());
        if (argc > 1) missing.SetString("device_secret", "must-not-fall-back");
    }
    fail_open = false;
    committed["wifi"] = {{"ota_url", "https://old.example/ota"}, {"provisioning_url", "https://old.example/status"}, {"sleep", "12"}};
    committed["backend"] = {{"api_url", "https://old.example/v1"}, {"device_secret", "original-secret"}};
    committed["websocket"] = {{"url", "wss://old.example/ws"}, {"token", "original-token"}};
    committed["tbot_claim"] = {{"confirmed", "1"}};
    committed["mqtt"] = {{"endpoint", "old.example"}};
    committed["board"] = {{"uuid", "physical-uuid"}};
    const auto originals = committed;
    {
        StagingSettings wifi("wifi", true);
        assert(wifi.GetString("ota_url") == "https://m0-esp.tjbot.vn/tbot/ota/");
        assert(wifi.GetString("provisioning_url") == "https://m0-api.tjbot.vn/v1/device/provisioning/status");
        assert(wifi.GetInt("sleep") == 12);
        wifi.SetString("ota_url", "https://untrusted.example/");
        wifi.EraseKey("provisioning_url");
        assert(wifi.GetString("ota_url") == "https://m0-esp.tjbot.vn/tbot/ota/");
    }
    {
        StagingSettings backend("backend", true), ws("websocket", true), claim("tbot_claim", true), mqtt("mqtt"), system("board");
        assert(backend.GetString("device_secret").empty());
        assert(ws.GetString("token").empty());
        assert(claim.GetInt("confirmed") == 0);
        assert(mqtt.GetString("endpoint").empty());
        assert(system.GetString("uuid") == "physical-uuid");
        assert(backend.GetString("api_url") == "https://m0-api.tjbot.vn/v1");
        assert(ws.GetString("url") == "wss://m0-esp.tjbot.vn/tbot/v1/");
        backend.SetString("device_secret", "staging-secret");
        ws.SetString("token", "staging-token");
        claim.SetInt("confirmed", 1);
        claim.SetBool("ready", true);
    }
    {
        StagingSettings backend("backend"), ws("websocket", true), claim("tbot_claim");
        assert(backend.GetString("device_secret") == "staging-secret");
        assert(ws.GetString("token") == "staging-token");
        assert(claim.GetInt("confirmed") == 1 && claim.GetBool("ready"));
        ws.EraseAll();
    }
    for (const auto& original : originals) assert(committed.at(original.first) == original.second);
    {
        ProductionSettings backend("backend"), ws("websocket"), claim("tbot_claim"), wifi("wifi");
        assert(backend.GetString("device_secret") == "original-secret");
        assert(ws.GetString("token") == "original-token");
        assert(claim.GetInt("confirmed") == 1);
        assert(wifi.GetString("ota_url") == "https://old.example/ota");
    }
}
