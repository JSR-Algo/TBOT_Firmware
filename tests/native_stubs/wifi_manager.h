#pragma once

class WifiManager {
public:
    bool stop_config_ap_called = false;

    void StopConfigAp() { stop_config_ap_called = true; }
};
