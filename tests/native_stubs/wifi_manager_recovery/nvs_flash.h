#pragma once
#include "esp_wifi.h"
constexpr esp_err_t ESP_ERR_NVS_NO_FREE_PAGES = 0x110d;
constexpr esp_err_t ESP_ERR_NVS_NEW_VERSION_FOUND = 0x1110;
esp_err_t nvs_flash_init();
esp_err_t nvs_flash_erase();
const char* esp_err_to_name(esp_err_t);
#define ESP_ERROR_CHECK(expr) do { if ((expr) != ESP_OK) __builtin_trap(); } while (0)
