#pragma once

#include <algorithm>
#include <cstdint>

// Owned only by the output worker. Gaps include normal silence between turns.
struct AudioOutputTiming {
    uint32_t frames = 0;
    int64_t max_gap_us = 0;
    int64_t max_write_us = 0;

    void Record(int64_t start_us, int64_t end_us) {
        if (previous_end_us_ != 0) {
            max_gap_us = std::max(max_gap_us, start_us - previous_end_us_);
        }
        max_write_us = std::max(max_write_us, end_us - start_us);
        previous_end_us_ = end_us;
        ++frames;
    }

    void ClearWindow() {
        frames = 0;
        max_gap_us = max_write_us = 0;
    }

private:
    int64_t previous_end_us_ = 0;
};
