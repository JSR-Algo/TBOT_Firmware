#ifndef TRANSPORT_DEADLINE_H
#define TRANSPORT_DEADLINE_H

#include <cstdint>
#include <limits>

inline int64_t TransportDeadlineUs(int timeout_ms, int64_t now_us) {
    if (timeout_ms <= 0) {
        return now_us;
    }
    constexpr int64_t kMicrosPerMillisecond = 1000;
    const int64_t timeout_us = static_cast<int64_t>(timeout_ms) * kMicrosPerMillisecond;
    if (now_us > std::numeric_limits<int64_t>::max() - timeout_us) {
        return std::numeric_limits<int64_t>::max();
    }
    return now_us + timeout_us;
}

inline int RemainingTransportTimeoutMs(int64_t deadline_us, int64_t now_us) {
    if (deadline_us <= now_us) {
        return 0;
    }
    constexpr int64_t kMicrosPerMillisecond = 1000;
    const int64_t remaining_us = deadline_us - now_us;
    const int64_t remaining_ms =
        (remaining_us + kMicrosPerMillisecond - 1) / kMicrosPerMillisecond;
    return remaining_ms > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(remaining_ms);
}

#endif  // TRANSPORT_DEADLINE_H
