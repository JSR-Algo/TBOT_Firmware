#include "wifi_scan_recovery_executor.h"

#include "default_event_loop_barrier.h"

#include <chrono>

#include <esp_wifi.h>

namespace {

bool IsStoppedOrNotInitialized(esp_err_t result) {
    return result == ESP_OK || result == ESP_ERR_WIFI_NOT_INIT;
}

bool IsScanStoppedOrDriverCanStopIt(esp_err_t result) {
    // ESP-IDF reports STATE while connecting; the required wifi_stop step ends it.
    return IsStoppedOrNotInitialized(result) ||
           result == ESP_ERR_WIFI_NOT_STARTED ||
           result == ESP_ERR_WIFI_STATE;
}

}  // namespace

WifiScanLeaseCoordinator::RecoveryProof WifiScanRecoveryExecutor::Execute(
        const WifiScanLeaseCoordinator::RecoveryDecision& recovery) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recovery.begun() || recovery.recovery_id() == 0) {
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }

    if (!IsScanStoppedOrDriverCanStopIt(esp_wifi_scan_stop())) {
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }
    if (!IsStoppedOrNotInitialized(esp_wifi_stop())) {
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }
    if (!IsStoppedOrNotInitialized(esp_wifi_deinit())) {
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }
    if (!DrainDefaultEventLoop(std::chrono::milliseconds(1000))) {
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }

    return WifiScanLeaseCoordinator::RecoveryProof{
        recovery.recovery_id(), true, true};
}
