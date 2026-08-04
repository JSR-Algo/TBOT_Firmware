#pragma once

#include <cstddef>

constexpr int MALLOC_CAP_INTERNAL = 1 << 0;
constexpr int MALLOC_CAP_SPIRAM = 1 << 1;

inline std::size_t& HostHilLifetimeInternalHeapMinimum() {
    static std::size_t value = 12000;
    return value;
}

inline std::size_t& HostHilLifetimePsramHeapMinimum() {
    static std::size_t value = 4200000;
    return value;
}

inline std::size_t& HostHilCurrentInternalHeapFree() {
    static std::size_t value = 60000;
    return value;
}

inline std::size_t& HostHilCurrentPsramHeapFree() {
    static std::size_t value = 4100000;
    return value;
}

inline std::size_t heap_caps_get_minimum_free_size(int caps) {
    return caps & MALLOC_CAP_SPIRAM ? HostHilLifetimePsramHeapMinimum()
                                    : HostHilLifetimeInternalHeapMinimum();
}

inline std::size_t heap_caps_get_free_size(int caps) {
    return caps & MALLOC_CAP_SPIRAM ? HostHilCurrentPsramHeapFree()
                                    : HostHilCurrentInternalHeapFree();
}
