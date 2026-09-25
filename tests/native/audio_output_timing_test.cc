#include "audio/audio_output_timing.h"
#include <cassert>

int main() {
    AudioOutputTiming timing;
    timing.Record(100000, 160000);
    assert(timing.frames == 1 && timing.max_gap_us == 0);
    assert(timing.max_write_us == 60000);
    timing.Record(170000, 240000);
    assert(timing.max_gap_us == 10000 && timing.max_write_us == 70000);
    timing.ClearWindow();
    assert(timing.frames == 0 && timing.max_gap_us == 0);
    timing.Record(245000, 246000);
    assert(timing.max_gap_us == 5000 && timing.max_write_us == 1000);
}
