#include "audio/codecs/es8311_write_timing.h"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    Es8311WriteTiming timing;
    assert(timing.frames == 0 && !timing.WindowReady());
    assert(timing.max_prepare_us == 0 && timing.max_driver_us == 0);
    assert(timing.max_log_us == 0 && timing.logged_frames == 0 && timing.errors == 0);

    timing.Record(10, 700000, 50000, true, false);
    timing.Record(40, 60000, 2, false, true);
    assert(timing.frames == 2 && timing.logged_frames == 1 && timing.errors == 1);
    assert(timing.max_prepare_us == 40 && timing.max_driver_us == 700000);
    assert(timing.max_log_us == 50000);
    for (int i = 2; i < 100; ++i) timing.Record(1, 2, 3, false, false);
    assert(timing.WindowReady() && timing.frames == 100);
    timing.Record(999999, 999999, 999999, true, true);
    assert(timing.frames == 100 && timing.errors == 1);

    timing.ClearWindow();
    assert(timing.frames == 0 && !timing.WindowReady());
    assert(timing.max_prepare_us == 0 && timing.max_driver_us == 0);
    assert(timing.max_log_us == 0 && timing.logged_frames == 0 && timing.errors == 0);
    // Caller passes durations, so silence between turns cannot enter a stage maximum.
    timing.Record(5, 6, 7, false, false);
    assert(timing.frames == 1 && timing.max_prepare_us == 5);
    assert(timing.max_driver_us == 6 && timing.max_log_us == 7);
    timing.Record(-1, std::numeric_limits<int64_t>::max(), 0, false, false);
    assert(timing.max_prepare_us == 5 && timing.max_driver_us == UINT32_MAX);
}
