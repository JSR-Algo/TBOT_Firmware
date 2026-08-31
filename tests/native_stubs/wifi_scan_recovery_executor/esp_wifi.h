#pragma once

#include <cstdint>

using esp_err_t = int32_t;

constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_WIFI_NOT_INIT = 0x3001;

struct wifi_init_config_t {
    bool nvs_enable = true;
};

#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}

extern "C" esp_err_t esp_wifi_scan_stop();
extern "C" esp_err_t esp_wifi_stop();
extern "C" esp_err_t esp_wifi_deinit();
extern "C" esp_err_t esp_wifi_init(const wifi_init_config_t* config);
