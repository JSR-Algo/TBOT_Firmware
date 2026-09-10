#pragma once

#include <algorithm>
#include <mutex>
#include <string>

// UART boundary shared by firmware and native tests. BufferSpace is the IDF
// ring's conservative free-space query; 256 covers event/item alignment costs.
template <typename Uart, typename Owns>
bool TrySpeakingArmWrite(std::recursive_mutex& mutex, bool left,
                         int percent, Uart& uart, Owns&& owns) {
    std::unique_lock<std::recursive_mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock() || !uart.Ready() || !owns()) return false;
    const int left_angle = (std::clamp(percent, 0, 100) * 60 + 50) / 100;
    const int angle = left ? left_angle : 60 - left_angle;
    const std::string payload = std::string("{\"cmd\":\"servo\",\"part\":\"") +
        (left ? "left_arm" : "right_arm") + "\",\"action\":\"set_percent\",\"from\":" +
        (left ? "0" : "60") + ",\"to\":" + std::to_string(angle) +
        ",\"step\":2,\"delay_ms\":20}\n";
    if (!uart.Idle() || uart.BufferSpace() < payload.size() + 256 ||
        !uart.SelectProfile() || !owns()) return false;
    return uart.Write(payload);
}
