#pragma once

#include <cstdint>

enum class AudioOutputDrainState { Unsupported, Busy, Pending, Drained, Failed };

struct AudioOutputDrainSnapshot {
    uint32_t epoch = 0;
    AudioOutputDrainState state = AudioOutputDrainState::Unsupported;
};
