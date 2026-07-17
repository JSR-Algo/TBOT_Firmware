#include "transport_deadline.h"

#include <cassert>
#include <cstdint>

int main() {
    constexpr int64_t now_us = 1'000'000;
    const int64_t deadline_us = TransportDeadlineUs(8000, now_us);
    assert(deadline_us == 9'000'000);
    assert(RemainingTransportTimeoutMs(deadline_us, now_us) == 8000);
    assert(RemainingTransportTimeoutMs(deadline_us, 8'999'001) == 1);
    assert(RemainingTransportTimeoutMs(deadline_us, deadline_us) == 0);
    assert(RemainingTransportTimeoutMs(deadline_us, deadline_us + 1) == 0);
    return 0;
}
