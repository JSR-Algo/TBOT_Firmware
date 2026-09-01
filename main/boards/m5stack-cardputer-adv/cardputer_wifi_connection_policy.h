#pragma once

#include "wifi_station_start_result.h"

enum class CardputerWifiStartAction : uint8_t {
    kRetry,
    kCompleteReconnect,
    kMonitorCredentials,
};

inline CardputerWifiStartAction ResolveCardputerWifiStartAction(
        bool reconnect, WifiStationStartResult result) {
    if (result == WifiStationStartResult::kBusyOrFailed) {
        return CardputerWifiStartAction::kRetry;
    }
    return reconnect ? CardputerWifiStartAction::kCompleteReconnect
                     : CardputerWifiStartAction::kMonitorCredentials;
}
