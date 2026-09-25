#ifndef CONNECTION_INBOUND_GATE_H
#define CONNECTION_INBOUND_GATE_H

#include <atomic>
#include <cstdint>
#include <mutex>

class ConnectionInboundGate {
public:
    enum class LeaseStatus { Allowed, Busy, Stale };
    class Mutation {
    public:
        Mutation(Mutation&&) = default;
        Mutation& operator=(Mutation&&) = default;
        uint32_t epoch() const { return epoch_; }
        bool Matched() const { return matched_; }

    private:
        friend class ConnectionInboundGate;
        Mutation(std::unique_lock<std::recursive_mutex>&& lock,
                 uint32_t epoch,
                 bool matched)
            : lock_(std::move(lock)), epoch_(epoch), matched_(matched) {}

        std::unique_lock<std::recursive_mutex> lock_;
        uint32_t epoch_;
        bool matched_;
    };

    class Lease {
    public:
        Lease(Lease&& other) noexcept
            : lock_(std::move(other.lock_)),
              allowed_(other.allowed_),
              current_epoch_(other.current_epoch_),
              status_(other.status_),
              tracks_thread_(other.tracks_thread_) {
            other.tracks_thread_ = false;
        }
        Lease& operator=(Lease&&) = delete;
        ~Lease() {
            if (tracks_thread_) {
                --current_thread_lease_depth_;
            }
        }
        explicit operator bool() const { return allowed_; }
        bool IsCurrentEpoch() const { return current_epoch_; }
        LeaseStatus status() const { return status_; }

    private:
        friend class ConnectionInboundGate;
        Lease(std::unique_lock<std::recursive_mutex>&& lock,
              bool allowed,
              bool current_epoch,
              LeaseStatus status)
            : lock_(std::move(lock)),
              allowed_(allowed),
              current_epoch_(current_epoch),
              status_(status),
              tracks_thread_(lock_.owns_lock()) {
            if (tracks_thread_) ++current_thread_lease_depth_;
        }

        std::unique_lock<std::recursive_mutex> lock_;
        bool allowed_;
        bool current_epoch_;
        LeaseStatus status_;
        bool tracks_thread_;
    };

    Mutation BeginConnectionMutation() {
        std::unique_lock<std::recursive_mutex> lock(mutex_);
        AdvanceEpoch();
        healthy_ = true;
        healthy_epoch_.store(epoch_, std::memory_order_release);
        return Mutation(std::move(lock), epoch_, true);
    }

    Mutation BeginFailureMutation() {
        std::unique_lock<std::recursive_mutex> lock(mutex_);
        AdvanceEpoch();
        healthy_ = false;
        healthy_epoch_.store(0, std::memory_order_release);
        return Mutation(std::move(lock), epoch_, true);
    }

    Mutation BeginFailureMutationIfCurrent(uint32_t expected_epoch) {
        std::unique_lock<std::recursive_mutex> lock(mutex_);
        if (epoch_ != expected_epoch) {
            return Mutation(std::move(lock), epoch_, false);
        }
        AdvanceEpoch();
        healthy_ = false;
        healthy_epoch_.store(0, std::memory_order_release);
        return Mutation(std::move(lock), epoch_, true);
    }

    uint32_t BeginConnection() {
        auto mutation = BeginConnectionMutation();
        return mutation.epoch();
    }

    Lease Acquire(uint32_t epoch) {
        std::unique_lock<std::recursive_mutex> lock(mutex_);
        const bool current_epoch = epoch == epoch_;
        const bool allowed = healthy_ && current_epoch;
        return Lease(std::move(lock), allowed, current_epoch,
                     allowed ? LeaseStatus::Allowed : LeaseStatus::Stale);
    }

    Lease TryAcquire(uint32_t expected_epoch) {
        std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return Lease(std::move(lock), false, false, LeaseStatus::Busy);
        }
        const bool current_epoch = expected_epoch == epoch_;
        const bool allowed = healthy_ && current_epoch;
        return Lease(std::move(lock), allowed, current_epoch,
                     allowed ? LeaseStatus::Allowed : LeaseStatus::Stale);
    }

    // Snapshot only: sending must still hold a matching lease.
    uint32_t HealthyEpoch() const {
        return healthy_epoch_.load(std::memory_order_acquire);
    }

    void FailCurrent() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        healthy_ = false;
        healthy_epoch_.store(0, std::memory_order_release);
    }

    bool CurrentThreadHasLease() const {
        return current_thread_lease_depth_ != 0;
    }

    uint32_t CurrentEpoch() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return epoch_;
    }

private:
    void AdvanceEpoch() {
        ++epoch_;
        if (epoch_ == 0) {
            ++epoch_;
        }
    }

    inline static thread_local uint32_t current_thread_lease_depth_ = 0;
    mutable std::recursive_mutex mutex_;
    uint32_t epoch_ = 0;
    bool healthy_ = false;
    // Load/store only: ESP32's SPIRAM workaround can make atomic RMW blocking.
    std::atomic<uint32_t> healthy_epoch_{0};
};

#endif  // CONNECTION_INBOUND_GATE_H
