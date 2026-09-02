#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

inline constexpr size_t kMaxWifiSsidBytes = 32;
inline constexpr size_t kMaxWifiPasswordBytes = 63;

inline bool IsValidWifiCredentials(const std::string& ssid,
                                   const std::string& password) {
    return !ssid.empty() && ssid.size() <= kMaxWifiSsidBytes &&
           password.size() <= kMaxWifiPasswordBytes;
}

inline bool CopyWifiCredentialsToBuffers(
        const std::string& ssid, const std::string& password,
        uint8_t* ssid_destination, size_t ssid_capacity,
        uint8_t* password_destination, size_t password_capacity) {
    if (!IsValidWifiCredentials(ssid, password) ||
        ssid_destination == nullptr || password_destination == nullptr ||
        ssid_capacity < kMaxWifiSsidBytes ||
        password_capacity <= kMaxWifiPasswordBytes) {
        return false;
    }
    std::memset(ssid_destination, 0, ssid_capacity);
    std::memset(password_destination, 0, password_capacity);
    std::memcpy(ssid_destination, ssid.data(), ssid.size());
    std::memcpy(password_destination, password.data(), password.size());
    return true;
}

inline bool AppendWifiFieldIfFits(std::string& field, const char* input,
                                  size_t max_bytes) {
    if (input == nullptr) {
        return false;
    }
    const size_t input_bytes = std::strlen(input);
    if (input_bytes == 0 || field.size() >= max_bytes ||
        input_bytes > max_bytes - field.size()) {
        return false;
    }
    field.append(input, input_bytes);
    return true;
}
