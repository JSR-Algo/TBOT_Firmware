#include "wifi_radio_recovery_restorer.h"

#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <vector>

extern "C" esp_err_t esp_wifi_stop() { return ESP_FAIL; }
extern "C" esp_err_t esp_wifi_set_mode(wifi_mode_t) { return ESP_FAIL; }
extern "C" esp_err_t esp_wifi_set_config(wifi_interface_t, wifi_config_t*) {
    return ESP_FAIL;
}
extern "C" esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { return ESP_FAIL; }
extern "C" esp_err_t esp_wifi_start() { return ESP_FAIL; }
extern "C" esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t) {
    return ESP_FAIL;
}
extern "C" esp_err_t esp_wifi_set_max_tx_power(int8_t) { return ESP_FAIL; }

namespace {

class Driver : public WifiRadioRecoveryRestorer::Driver {
public:
    esp_err_t Stop() override {
        const auto result = Call("stop");
        if (result == ESP_OK || result == ESP_ERR_WIFI_NOT_STARTED) {
            started_ = false;
        }
        return result;
    }
    esp_err_t SetMode(wifi_mode_t) override {
        return started_ ? ESP_ERR_WIFI_STATE : Call("mode");
    }
    esp_err_t SetConfig(wifi_interface_t, wifi_config_t*) override {
        return started_ ? ESP_ERR_WIFI_STATE : Call("config");
    }
    esp_err_t SetPowerSave(wifi_ps_type_t) override { return Call("ps"); }
    esp_err_t Start() override {
        const auto result = Call("start");
        if (result == ESP_OK) {
            started_ = true;
        }
        return result;
    }
    esp_err_t SetBandMode(wifi_band_mode_t) override { return Call("band"); }
    esp_err_t SetMaxTxPower(int8_t) override { return Call("max_tx"); }

    void FailOnceAt(const std::string& stage) { failures_[stage].push_back(ESP_FAIL); }
    void ClearCalls() { calls.clear(); }

    std::vector<std::string> calls;

private:
    esp_err_t Call(const std::string& name) {
        calls.push_back(name);
        auto& failures = failures_[name];
        if (failures.empty()) {
            return ESP_OK;
        }
        const auto result = failures.front();
        failures.pop_front();
        return result;
    }

    std::map<std::string, std::deque<esp_err_t>> failures_;
    bool started_ = false;
};

void StationRetriesFromKnownStoppedStateAfterEveryFailure() {
    for (const std::string stage : {"stop", "mode", "start", "max_tx", "ps"}) {
        Driver driver;
        WifiRadioRecoveryRestorer restorer(driver);
        driver.FailOnceAt(stage);
        assert(!restorer.RestoreStation(WIFI_PS_MIN_MODEM, 72));
        driver.ClearCalls();
        assert(restorer.RestoreStation(WIFI_PS_MIN_MODEM, 72));
        assert((driver.calls == std::vector<std::string>{
            "stop", "mode", "start", "max_tx", "ps"}));
    }
}

void ConfigApRetriesFromKnownStoppedStateAfterEveryFailure() {
    wifi_config_t config = {};
    for (const std::string stage : {
             "stop", "mode", "config", "ps", "start", "band", "max_tx"}) {
        Driver driver;
        WifiRadioRecoveryRestorer restorer(driver);
        driver.FailOnceAt(stage);
        assert(!restorer.RestoreConfigAp(config, WIFI_BAND_MODE_2G_ONLY, 72));
        driver.ClearCalls();
        assert(restorer.RestoreConfigAp(config, WIFI_BAND_MODE_2G_ONLY, 72));
        assert((driver.calls == std::vector<std::string>{
            "stop", "mode", "config", "ps", "start", "band", "max_tx"}));
    }
}

void AlreadyStoppedIsBenignOnlyAtNormalizationStage() {
    class AlreadyStoppedDriver final : public Driver {
    public:
        esp_err_t Stop() override {
            calls.push_back("stop");
            return ESP_ERR_WIFI_NOT_STARTED;
        }
    } driver;
    WifiRadioRecoveryRestorer restorer(driver);
    assert(restorer.RestoreStation(WIFI_PS_NONE, 0));
    assert((driver.calls == std::vector<std::string>{"stop", "mode", "start", "ps"}));
}

}  // namespace

int main() {
    StationRetriesFromKnownStoppedStateAfterEveryFailure();
    ConfigApRetriesFromKnownStoppedStateAfterEveryFailure();
    AlreadyStoppedIsBenignOnlyAtNormalizationStage();
    std::cout << "wifi radio recovery restorer host tests passed\n";
    return 0;
}
