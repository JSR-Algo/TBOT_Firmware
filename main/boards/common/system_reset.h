#ifndef _SYSTEM_RESET_H
#define _SYSTEM_RESET_H

#include <driver/gpio.h>

class SystemReset {
public:
    SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin); // 构造函数私有化
    void CheckButtons();

    // Release this robot's backend ownership (POST /v1/devices/:id/factory-reset,
    // authenticated by the stored device_secret) so a different parent phone can
    // re-claim it. Reads backend.{api_url,device_id,device_secret} from NVS and
    // performs a blocking ~5s HTTP POST, so call it OFF the priority-10 Application
    // task. Returns true if ownership was released OR there were no cloud
    // credentials to release; false if the HTTP call failed (e.g. offline) and the
    // release should be retried later. Uses no instance state (NVS + Board only),
    // so it is static and reusable by the BOOT re-pair flow.
    static bool ReleaseCloudOwnership();

private:
    gpio_num_t reset_nvs_pin_;
    gpio_num_t reset_factory_pin_;

    void ResetNvsFlash();
    void ResetToFactory();
    void RestartInSeconds(int seconds);
};


#endif
