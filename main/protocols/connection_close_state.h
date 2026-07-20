#ifndef CONNECTION_CLOSE_STATE_H
#define CONNECTION_CLOSE_STATE_H

#include <atomic>

class ConnectionCloseState {
public:
    void ResetForConnection() {
        deferred_epoch_.store(0, std::memory_order_release);
        notified_.store(false, std::memory_order_release);
    }

    bool MarkDeferred(uint32_t connection_epoch) {
        uint32_t expected = 0;
        return deferred_epoch_.compare_exchange_strong(
            expected, connection_epoch, std::memory_order_acq_rel);
    }

    bool TakeDeferred(uint32_t connection_epoch) {
        return deferred_epoch_.compare_exchange_strong(
            connection_epoch, 0, std::memory_order_acq_rel);
    }

    bool TakeNotification() {
        return !notified_.exchange(true, std::memory_order_acq_rel);
    }

private:
    std::atomic<uint32_t> deferred_epoch_{0};
    std::atomic<bool> notified_{false};
};

#endif  // CONNECTION_CLOSE_STATE_H
