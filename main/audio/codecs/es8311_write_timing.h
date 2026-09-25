#pragma once

#include <algorithm>
#include <cstdint>

// Output-worker owned. These are elapsed wall times, including scheduling waits.
struct Es8311WriteTiming {
    uint32_t frames = 0;
    uint32_t logged_frames = 0;
    uint32_t errors = 0;
    uint32_t max_prepare_us = 0;
    uint32_t max_driver_us = 0;
    uint32_t max_log_us = 0;

    bool WindowReady() const { return frames >= 100; }

    void Record(int64_t prepare_us, int64_t driver_us, int64_t log_us,
                bool logged, bool error) {
        if (WindowReady()) return;
        ++frames;
        logged_frames += logged;
        errors += error;
        max_prepare_us = std::max(max_prepare_us, Saturate(prepare_us));
        max_driver_us = std::max(max_driver_us, Saturate(driver_us));
        max_log_us = std::max(max_log_us, Saturate(log_us));
    }

    void ClearWindow() { *this = {}; }

private:
    static uint32_t Saturate(int64_t duration_us) {
        return static_cast<uint32_t>(std::clamp<int64_t>(duration_us, 0, UINT32_MAX));
    }
};
