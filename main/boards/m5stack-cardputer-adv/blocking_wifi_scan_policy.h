#pragma once

#include <cstdint>

class BlockingWifiScanPolicy {
public:
    static constexpr uint32_t CompletionWaitMs(
            uint32_t channel_count, uint32_t max_active_ms_per_channel,
            uint32_t scheduling_margin_ms) {
        return channel_count * max_active_ms_per_channel +
               scheduling_margin_ms;
    }

    static constexpr bool CallbackArrivedBeforeDeadline(
            uint32_t elapsed_ms, uint32_t deadline_ms) {
        return elapsed_ms < deadline_ms;
    }
};
