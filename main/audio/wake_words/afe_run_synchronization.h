#ifndef AFE_RUN_SYNCHRONIZATION_H
#define AFE_RUN_SYNCHRONIZATION_H

#include <atomic>
#include <cstdint>
#include <mutex>

class AfeRunSynchronization {
public:
    std::unique_lock<std::recursive_mutex> AcquireTransition() const {
        return std::unique_lock<std::recursive_mutex>(transition_mutex_);
    }

    std::unique_lock<std::recursive_mutex> TryAcquireTransition() const {
        return std::unique_lock<std::recursive_mutex>(transition_mutex_, std::try_to_lock);
    }

    uint32_t BeginStart(std::atomic<uint32_t>& generation) const {
        return generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    uint32_t BeginStop(std::atomic<uint32_t>& generation) const {
        return generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    template <typename ClearAcknowledgement, typename ClearRunning>
    uint32_t BeginStopAndClear(std::atomic<uint32_t>& generation,
                               ClearAcknowledgement clear_acknowledgement,
                               ClearRunning clear_running) const {
        const uint32_t stop_generation = BeginStop(generation);
        clear_acknowledgement();
        clear_running();
        return stop_generation;
    }

    void PublishStopped(std::atomic<uint32_t>& acknowledged_generation,
                        uint32_t generation) const {
        acknowledged_generation.store(generation, std::memory_order_release);
    }

    bool IsStopAcknowledged(const std::atomic<uint32_t>& acknowledged_generation,
                            uint32_t stop_generation) const {
        return acknowledged_generation.load(std::memory_order_acquire) == stop_generation;
    }

    template <typename ElapsedMs, typename Wait>
    bool WaitForStopAcknowledgement(
        const std::atomic<uint32_t>& acknowledged_generation,
        uint32_t stop_generation, uint32_t timeout_ms,
        ElapsedMs elapsed_ms, Wait wait) const {
        while (!IsStopAcknowledged(acknowledged_generation, stop_generation)) {
            const uint32_t elapsed = elapsed_ms();
            if (elapsed >= timeout_ms) {
                return false;
            }
            wait(timeout_ms - elapsed);
        }
        return true;
    }

    bool IsCurrent(const std::atomic<uint32_t>& generation,
                   uint32_t fetch_generation) const {
        return generation.load(std::memory_order_acquire) == fetch_generation;
    }

private:
    mutable std::recursive_mutex transition_mutex_;
};

#endif
