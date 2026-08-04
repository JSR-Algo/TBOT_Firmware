#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

inline char (&HostEspRomOutput())[2048] {
    static char output[2048] = {};
    return output;
}

inline void HostEspRomOutputReset() {
    HostEspRomOutput()[0] = '\0';
}

template <typename... Args>
inline void esp_rom_printf(const char* format, Args... args) {
    auto& output = HostEspRomOutput();
    const std::size_t used = std::strlen(output);
    if (used < sizeof(output)) {
        std::snprintf(output + used, sizeof(output) - used, format, args...);
    }
}
