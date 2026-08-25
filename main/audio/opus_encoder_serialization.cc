#include "opus_encoder_serialization.h"

std::mutex OpusEncoderSerialization::mutex_;

OpusEncoderSerialization::Lease OpusEncoderSerialization::Acquire() {
    return Lease(mutex_);
}
