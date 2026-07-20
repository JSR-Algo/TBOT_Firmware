#pragma once

#include "device_state.h"

class WifiConfigEntryPolicy {
public:
    static bool CanPrepare(DeviceState state, bool lesson_active,
                           bool connect_in_flight, bool reset_pending) {
        if (lesson_active || connect_in_flight || reset_pending) {
            return false;
        }
        return state == kDeviceStateStarting ||
               state == kDeviceStateWifiConfiguring ||
               state == kDeviceStateIdle ||
               state == kDeviceStateConnecting ||
               state == kDeviceStateListening ||
               state == kDeviceStateSpeaking ||
               state == kDeviceStateActivating;
    }
};
