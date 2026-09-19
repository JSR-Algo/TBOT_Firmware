#pragma once
#include <cstddef>
#include <cstring>
#include <string>
using esp_err_t = int;
using nvs_handle_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, NVS_READONLY = 0, NVS_READWRITE = 1;
inline std::string retained_nvs_blob;
inline std::string retained_nvs_failure;
inline int retained_nvs_open_handles = 0;
inline int nvs_open(const char*, int, nvs_handle_t* handle) {
    if (retained_nvs_failure == "open") return 2;
    ++retained_nvs_open_handles; *handle = 1; return ESP_OK;
}
inline void nvs_close(nvs_handle_t) { --retained_nvs_open_handles; }
inline int nvs_get_blob(nvs_handle_t, const char*, void* output, std::size_t* size) {
    if (retained_nvs_failure == "size" && !output) return 2;
    if (retained_nvs_failure == "read" && output) return 2;
    if (retained_nvs_blob.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (output) std::memcpy(output, retained_nvs_blob.data(), retained_nvs_blob.size());
    *size = retained_nvs_blob.size(); return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t, const char*, const void* data, std::size_t size) {
    if (retained_nvs_failure == "set") return 2;
    retained_nvs_blob.assign(static_cast<const char*>(data), size); return ESP_OK;
}
inline int nvs_commit(nvs_handle_t) { return retained_nvs_failure == "commit" ? 2 : ESP_OK; }
