#pragma once

#include <cstddef>
#include <cstring>
#include <string>

inline constexpr size_t kMaxWifiSsidBytes = 32;
inline constexpr size_t kMaxWifiPasswordBytes = 63;

inline bool IsValidWifiCredentials(const std::string& ssid,
                                   const std::string& password) {
    return !ssid.empty() && ssid.size() <= kMaxWifiSsidBytes &&
           password.size() <= kMaxWifiPasswordBytes;
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
