#pragma once
#include <cstdint>
using esp_err_t = int32_t;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_WIFI_NOT_INIT = 0x3001;
constexpr esp_err_t ESP_ERR_WIFI_NOT_STARTED = 0x3002;
constexpr esp_err_t ESP_ERR_WIFI_STATE = 0x3006;
enum wifi_mode_t { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA };
enum wifi_interface_t { WIFI_IF_STA, WIFI_IF_AP };
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM };
enum wifi_band_mode_t { WIFI_BAND_MODE_2G_ONLY, WIFI_BAND_MODE_AUTO };
struct wifi_config_t {
    struct {
        uint8_t ssid[32] = {};
        uint8_t ssid_len = 0;
        uint8_t max_connection = 0;
        uint8_t authmode = 0;
    } ap;
};
struct wifi_init_config_t { bool nvs_enable = true; };
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
esp_err_t esp_wifi_init(const wifi_init_config_t*);
esp_err_t esp_wifi_stop();
esp_err_t esp_wifi_get_mode(wifi_mode_t*);
esp_err_t esp_wifi_get_config(wifi_interface_t, wifi_config_t*);
esp_err_t esp_wifi_get_ps(wifi_ps_type_t*);
esp_err_t esp_wifi_get_max_tx_power(int8_t*);
esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t*);
esp_err_t esp_wifi_get_inactive_time(wifi_interface_t, uint16_t*);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_set_config(wifi_interface_t, wifi_config_t*);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
esp_err_t esp_wifi_start();
esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t);
esp_err_t esp_wifi_set_max_tx_power(int8_t);
