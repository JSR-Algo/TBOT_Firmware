#pragma once

#include <cstdint>

inline std::uint32_t& HostEspRandomValue() {
    static std::uint32_t value = 0x1234abcdU;
    return value;
}

inline std::uint32_t& HostEspRandomCalls() {
    static std::uint32_t calls = 0;
    return calls;
}

inline std::uint32_t esp_random() {
    ++HostEspRandomCalls();
    return HostEspRandomValue();
}
