#pragma once

#include <atomic>

template <typename Handle>
class ProcessLifetimeWorkerHandle {
public:
    bool Publish(Handle handle) {
        if (handle == Handle{}) {
            return false;
        }
        Handle expected{};
        return handle_.compare_exchange_strong(
            expected, handle, std::memory_order_release,
            std::memory_order_acquire);
    }

    Handle Load() const {
        return handle_.load(std::memory_order_acquire);
    }

private:
    std::atomic<Handle> handle_{};
};
