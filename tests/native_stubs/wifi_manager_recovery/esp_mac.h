#pragma once
#include "esp_wifi.h"
constexpr int ESP_MAC_WIFI_STA = 0;
esp_err_t esp_read_mac(uint8_t*, int);
