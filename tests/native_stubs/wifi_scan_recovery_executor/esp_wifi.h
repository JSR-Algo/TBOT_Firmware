#pragma once

#include <cstdint>

using esp_err_t = int32_t;

constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_WIFI_NOT_INIT = 0x3001;
constexpr esp_err_t ESP_ERR_WIFI_NOT_STARTED = 0x3002;
constexpr esp_err_t ESP_ERR_WIFI_STATE = 0x3006;

enum wifi_mode_t { WIFI_MODE_STA, WIFI_MODE_APSTA };
enum wifi_interface_t { WIFI_IF_STA, WIFI_IF_AP };
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM };
enum wifi_band_mode_t { WIFI_BAND_MODE_2G_ONLY, WIFI_BAND_MODE_AUTO };
struct wifi_config_t { uint8_t storage[128] = {}; };

struct wifi_init_config_t {
    bool nvs_enable = true;
};

#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}

extern "C" esp_err_t esp_wifi_scan_stop();
extern "C" esp_err_t esp_wifi_stop();
extern "C" esp_err_t esp_wifi_deinit();
extern "C" esp_err_t esp_wifi_init(const wifi_init_config_t* config);
extern "C" esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
extern "C" esp_err_t esp_wifi_set_config(wifi_interface_t interface,
                                           wifi_config_t* config);
extern "C" esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
extern "C" esp_err_t esp_wifi_start();
extern "C" esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t mode);
extern "C" esp_err_t esp_wifi_set_max_tx_power(int8_t power);
