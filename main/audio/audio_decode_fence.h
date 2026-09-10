#pragma once

#include <cstdint>

// All operations require the audio queue mutex; only the codec worker completes.
class AudioDecodeFence {
public:
    uint64_t Begin() {
        decode_in_flight_ = true;
        return reset_epoch_;
    }

    bool Complete(uint64_t epoch, uint32_t packet_generation,
                  uint32_t playback_generation) {
        decode_in_flight_ = false;
        return epoch == reset_epoch_ && packet_generation == playback_generation;
    }

    void Reset() { ++reset_epoch_; }
    uint64_t ResetEpoch() const { return reset_epoch_; }
    bool InFlight() const { return decode_in_flight_; }

private:
    uint64_t reset_epoch_ = 0;
    bool decode_in_flight_ = false;
};
