#pragma once

#include <cstddef>

// Codec-worker-local budget: refill playback without starving microphone encoding.
class AudioPlaybackRefillPolicy {
public:
    bool DeferEncode(bool decoded_for_playback, bool decode_pending,
                     bool encode_ready, std::size_t playback_queued,
                     std::size_t playback_capacity) {
        if (!decoded_for_playback || !decode_pending || !encode_ready ||
            playback_queued >= playback_capacity || deferred_) {
            deferred_ = false;
            return false;
        }
        deferred_ = true;
        return true;
    }

private:
    bool deferred_ = false;
};
