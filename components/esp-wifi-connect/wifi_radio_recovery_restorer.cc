#include "wifi_radio_recovery_restorer.h"

namespace {

class EspWifiRecoveryDriver final : public WifiRadioRecoveryRestorer::Driver {
public:
    esp_err_t Stop() override { return esp_wifi_stop(); }
    esp_err_t SetMode(wifi_mode_t mode) override { return esp_wifi_set_mode(mode); }
    esp_err_t SetConfig(wifi_interface_t interface,
                        wifi_config_t* config) override {
        return esp_wifi_set_config(interface, config);
    }
    esp_err_t SetPowerSave(wifi_ps_type_t type) override {
        return esp_wifi_set_ps(type);
    }
    esp_err_t Start() override { return esp_wifi_start(); }
    esp_err_t SetBandMode(wifi_band_mode_t mode) override {
        return esp_wifi_set_band_mode(mode);
    }
    esp_err_t SetMaxTxPower(int8_t power) override {
        return esp_wifi_set_max_tx_power(power);
    }
};

WifiRadioRecoveryRestorer::Driver& DefaultDriver() {
    static EspWifiRecoveryDriver driver;
    return driver;
}

bool Normalized(esp_err_t result) {
    return result == ESP_OK || result == ESP_ERR_WIFI_NOT_STARTED;
}

}  // namespace

WifiRadioRecoveryRestorer::WifiRadioRecoveryRestorer()
    : driver_(DefaultDriver()) {}

WifiRadioRecoveryRestorer::WifiRadioRecoveryRestorer(Driver& driver)
    : driver_(driver) {}

bool WifiRadioRecoveryRestorer::RestoreStation(
        wifi_ps_type_t power_save, int8_t max_tx_power) {
    if (!Normalized(driver_.Stop()) ||
        driver_.SetMode(WIFI_MODE_STA) != ESP_OK ||
        driver_.Start() != ESP_OK) {
        return false;
    }
    if (max_tx_power != 0 && driver_.SetMaxTxPower(max_tx_power) != ESP_OK) {
        return false;
    }
    return driver_.SetPowerSave(power_save) == ESP_OK;
}

bool WifiRadioRecoveryRestorer::RestoreConfigAp(
        wifi_config_t config, wifi_band_mode_t band_mode,
        int8_t max_tx_power) {
    if (!Normalized(driver_.Stop()) ||
        driver_.SetMode(WIFI_MODE_APSTA) != ESP_OK ||
        driver_.SetConfig(WIFI_IF_AP, &config) != ESP_OK ||
        driver_.SetPowerSave(WIFI_PS_NONE) != ESP_OK ||
        driver_.Start() != ESP_OK ||
        driver_.SetBandMode(band_mode) != ESP_OK) {
        return false;
    }
    return max_tx_power == 0 ||
           driver_.SetMaxTxPower(max_tx_power) == ESP_OK;
}
