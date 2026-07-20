#ifndef CONNECT_CLOSE_DEFERRAL_H
#define CONNECT_CLOSE_DEFERRAL_H

#include <atomic>

class ConnectCloseDeferral {
public:
    bool Request(bool connect_in_flight) {
        if (!connect_in_flight) {
            return true;
        }
        pending_.store(true, std::memory_order_release);
        return false;
    }

    bool TakeAfterWorker() {
        return pending_.exchange(false, std::memory_order_acq_rel);
    }

    bool Pending() const {
        return pending_.load(std::memory_order_acquire);
    }

    void Cancel() {
        pending_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> pending_{false};
};

#endif  // CONNECT_CLOSE_DEFERRAL_H
