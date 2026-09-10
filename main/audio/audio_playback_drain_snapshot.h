#pragma once

#include <cstddef>
#include <cstdint>
#include "audio_output_drain.h"

// Observation only: callers decide completion and recheck response identity.
struct PlaybackDrainSnapshot {
    uint64_t reset_epoch = 0;
    uint32_t playback_generation = 0;
    bool stopped = true;
    size_t decode_queue_size = 0;
    size_t playback_queue_size = 0;
    bool decode_in_flight = false;
    bool output_in_flight = false;
    AudioOutputDrainSnapshot codec;
};
