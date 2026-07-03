#pragma once

#include <cstdint>

inline int64_t esp_timer_get_time() {
    static int64_t now_us = 0;
    now_us += 1000;
    return now_us;
}
