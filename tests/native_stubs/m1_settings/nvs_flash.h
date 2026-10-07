#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
using nvs_handle_t = uint32_t;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int NVS_READWRITE = 1;
constexpr int NVS_READONLY = 0;
#define ESP_ERROR_CHECK(value) do { if ((value) != ESP_OK) std::abort(); } while (0)
int nvs_open(const char*, int, nvs_handle_t*);
int nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
int nvs_set_str(nvs_handle_t, const char*, const char*);
int nvs_get_i32(nvs_handle_t, const char*, int32_t*);
int nvs_set_i32(nvs_handle_t, const char*, int32_t);
int nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
int nvs_set_u8(nvs_handle_t, const char*, uint8_t);
int nvs_erase_key(nvs_handle_t, const char*);
int nvs_erase_all(nvs_handle_t);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
