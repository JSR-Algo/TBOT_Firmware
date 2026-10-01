#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>

// UART boundary shared by firmware and native tests. BufferSpace is the IDF
// ring's conservative free-space query; 256 covers event/item alignment costs.
// `buffer` is caller-owned scratch (PSRAM .bss on firmware) and is only
// written while `mutex` is held.
template <typename Uart, typename Owns>
bool TrySpeakingArmWrite(std::recursive_mutex& mutex, bool left,
                         int percent, Uart& uart, Owns&& owns,
                         char* buffer, size_t capacity) {
    std::unique_lock<std::recursive_mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock() || !uart.Ready() || !owns()) return false;
    const int left_angle = (std::clamp(percent, 0, 100) * 60 + 50) / 100;
    const int angle = left ? left_angle : 60 - left_angle;
    const int written = std::snprintf(buffer, capacity,
        "{\"cmd\":\"servo\",\"part\":\"%s\",\"action\":\"set_percent\","
        "\"from\":%d,\"to\":%d,\"step\":2,\"delay_ms\":20}\n",
        left ? "left_arm" : "right_arm", left ? 0 : 60, angle);
    if (written <= 0 || static_cast<size_t>(written) >= capacity) return false;
    const size_t length = static_cast<size_t>(written);
    if (!uart.Idle() || uart.BufferSpace() < length + 256 ||
        !uart.SelectProfile() || !owns()) return false;
    return uart.Write(buffer, length);
}
