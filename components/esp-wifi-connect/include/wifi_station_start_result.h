#pragma once

#include <cstdint>

enum class WifiStationStartResult : uint8_t {
    kStartedNow,
    kAlreadyActive,
    kBusyOrFailed,
    kInvalidCredentials,
};

inline bool ShouldArmWifiConnectTimeout(WifiStationStartResult result) {
    return result == WifiStationStartResult::kStartedNow;
}
