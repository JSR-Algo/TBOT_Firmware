#pragma once
#include <cstdint>
struct ConnectionReceipt {
    uint64_t received_us = 0;
    uint64_t admission_deadline_us = 0;
};

struct ConnectionSource {
    uint32_t source_id = 0;
    uint32_t connection_epoch = 0;
    bool Valid() const { return source_id && source_id != UINT32_MAX && connection_epoch; }
};

// Owned by the WebSocket connection-mutation gate, never a callback writer.
// Unlike the legacy gate epoch, source identity cannot wrap and be reused.
class ConnectionSourceSequence {
public:
    ConnectionSource Next(uint32_t connection_epoch) {
        if (next_ != UINT32_MAX) ++next_;
        return {next_, connection_epoch};
    }
private:
    uint32_t next_ = 0;
};
