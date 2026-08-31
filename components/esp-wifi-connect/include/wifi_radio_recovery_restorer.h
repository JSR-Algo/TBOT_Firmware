#pragma once

#include <esp_wifi.h>

class WifiRadioRecoveryRestorer {
public:
    class Driver {
    public:
        virtual ~Driver() = default;
        virtual esp_err_t Stop() = 0;
        virtual esp_err_t SetMode(wifi_mode_t mode) = 0;
        virtual esp_err_t SetConfig(wifi_interface_t interface,
                                    wifi_config_t* config) = 0;
        virtual esp_err_t SetPowerSave(wifi_ps_type_t type) = 0;
        virtual esp_err_t Start() = 0;
        virtual esp_err_t SetBandMode(wifi_band_mode_t mode) = 0;
        virtual esp_err_t SetMaxTxPower(int8_t power) = 0;
    };

    WifiRadioRecoveryRestorer();
    explicit WifiRadioRecoveryRestorer(Driver& driver);

    bool RestoreStation(wifi_ps_type_t power_save, int8_t max_tx_power);
    bool RestoreConfigAp(wifi_config_t config, wifi_band_mode_t band_mode,
                         int8_t max_tx_power);

private:
    Driver& driver_;
};
