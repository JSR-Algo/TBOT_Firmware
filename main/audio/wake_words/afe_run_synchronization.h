#ifndef AFE_RUN_SYNCHRONIZATION_H
#define AFE_RUN_SYNCHRONIZATION_H

#include <atomic>
#include <cstdint>

class AfeRunSynchronization {
public:
    uint32_t BeginStart(std::atomic<uint32_t>& generation) const {
        return generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    uint32_t BeginStop(std::atomic<uint32_t>& generation) const {
        return generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    void PublishStopped(std::atomic<uint32_t>& acknowledged_generation,
                        uint32_t generation) const {
        acknowledged_generation.store(generation, std::memory_order_release);
    }

    bool IsStopAcknowledged(const std::atomic<uint32_t>& acknowledged_generation,
                            uint32_t stop_generation) const {
        return acknowledged_generation.load(std::memory_order_acquire) == stop_generation;
    }

    bool IsCurrent(const std::atomic<uint32_t>& generation,
                   uint32_t fetch_generation) const {
        return generation.load(std::memory_order_acquire) == fetch_generation;
    }
};

#endif
