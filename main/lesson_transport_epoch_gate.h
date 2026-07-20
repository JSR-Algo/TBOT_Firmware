#pragma once

#include <atomic>
#include <cstdint>

class LessonTransportEpochGate {
public:
    std::uint64_t PublishedEpoch() const {
        return published_epoch_.load(std::memory_order_acquire);
    }

    std::uint64_t PublishTerminalEpoch() {
        return published_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    bool WorkerApplyTerminal(std::uint64_t epoch) {
        if (epoch <= worker_epoch_) return false;
        worker_epoch_ = epoch;
        return true;
    }

    bool WorkerAcceptFrame(std::uint64_t epoch) const {
        return epoch == worker_epoch_;
    }

private:
    std::atomic<std::uint64_t> published_epoch_{1};
    std::uint64_t worker_epoch_ = 1;
};
