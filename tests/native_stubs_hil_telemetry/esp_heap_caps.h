#pragma once

#include <cstddef>

using esp_err_t = int;

constexpr esp_err_t ESP_OK = 0;
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

inline std::size_t& HostHilLocalInternalHeapMinimum() {
    static std::size_t value = 60000;
    return value;
}

inline std::size_t& HostHilLocalPsramHeapMinimum() {
    static std::size_t value = 4100000;
    return value;
}

inline bool& HostHilHeapMonitorActive() {
    static bool value = false;
    return value;
}

inline int& HostHilHeapMonitorStartCalls() {
    static int value = 0;
    return value;
}

inline int& HostHilHeapMonitorStopCalls() {
    static int value = 0;
    return value;
}

inline std::size_t heap_caps_get_minimum_free_size(int caps) {
    if (caps & MALLOC_CAP_SPIRAM) {
        return HostHilHeapMonitorActive() ? HostHilLocalPsramHeapMinimum()
                                          : HostHilLifetimePsramHeapMinimum();
    }
    return HostHilHeapMonitorActive() ? HostHilLocalInternalHeapMinimum()
                                      : HostHilLifetimeInternalHeapMinimum();
}

inline esp_err_t heap_caps_monitor_local_minimum_free_size_start() {
    HostHilLocalInternalHeapMinimum() = HostHilCurrentInternalHeapFree();
    HostHilLocalPsramHeapMinimum() = HostHilCurrentPsramHeapFree();
    HostHilHeapMonitorActive() = true;
    ++HostHilHeapMonitorStartCalls();
    return ESP_OK;
}

inline esp_err_t heap_caps_monitor_local_minimum_free_size_stop() {
    HostHilHeapMonitorActive() = false;
    ++HostHilHeapMonitorStopCalls();
    return ESP_OK;
}
