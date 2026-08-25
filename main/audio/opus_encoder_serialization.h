#ifndef OPUS_ENCODER_SERIALIZATION_H
#define OPUS_ENCODER_SERIALIZATION_H

#include <mutex>

// ESP Opus calls share one boundary; wake encoders retain the lease for their full lifetime.
class OpusEncoderSerialization final {
public:
    using Lease = std::unique_lock<std::mutex>;

    static Lease Acquire();

private:
    static std::mutex mutex_;
};

#endif
