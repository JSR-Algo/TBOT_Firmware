#ifndef WAKE_WORD_LIFECYCLE_CONTROLLER_H
#define WAKE_WORD_LIFECYCLE_CONTROLLER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

class WakeWordLifecycleController {
public:
    struct PrewarmToken {
        uint64_t generation = 0;
        bool valid() const { return generation != 0; }
    };

    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept { MoveFrom(other); }
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                Reset();
                MoveFrom(other);
            }
            return *this;
        }
        ~Lease() { Reset(); }
        explicit operator bool() const { return owner_ != nullptr; }
        uint64_t generation() const { return generation_; }

    private:
        friend class WakeWordLifecycleController;
        Lease(WakeWordLifecycleController* owner, uint64_t generation)
            : owner_(owner), generation_(generation) {}
        void Reset() {
            if (owner_ != nullptr) {
                owner_->ReleaseLease();
                owner_ = nullptr;
            }
        }
        void MoveFrom(Lease& other) {
            owner_ = other.owner_;
            generation_ = other.generation_;
            other.owner_ = nullptr;
        }
        WakeWordLifecycleController* owner_ = nullptr;
        uint64_t generation_ = 0;
    };

    PrewarmToken CapturePrewarmToken() const {
        const uint64_t state = state_.load(std::memory_order_acquire);
        return (Flags(state) & (kProvisioning | kQuiescing)) == 0
                   ? PrewarmToken{Generation(state)}
                   : PrewarmToken{};
    }

    Lease TryAcquireFeed() { return TryAcquire(kRunning, 0); }
    Lease TryAcquireAccess() { return TryAcquire(0, 0); }
    Lease TryAcquirePrewarm(PrewarmToken token) {
        return token.valid() ? TryAcquire(0, token.generation) : Lease{};
    }

    void SetRunning(bool running) {
        std::lock_guard<std::mutex> lock(transition_mutex_);
        uint64_t state = state_.load(std::memory_order_relaxed);
        uint8_t flags = Flags(state);
        if (flags & (kProvisioning | kQuiescing)) {
            return;
        }
        flags = running ? static_cast<uint8_t>(flags | kRunning)
                        : static_cast<uint8_t>(flags & ~kRunning);
        state_.store(Pack(Generation(state), flags), std::memory_order_release);
    }

    template <typename Callback>
    void BeginProvisioningAndQuiesce(Callback&& on_quiescing) {
        {
            std::lock_guard<std::mutex> transition_lock(transition_mutex_);
            const uint64_t state = state_.load(std::memory_order_relaxed);
            state_.store(Pack(Generation(state) + 1, kProvisioning | kQuiescing),
                         std::memory_order_release);
        }
        std::forward<Callback>(on_quiescing)();
        std::unique_lock<std::mutex> wait_lock(wait_mutex_);
        idle_cv_.wait(wait_lock, [this]() {
            return active_leases_.load(std::memory_order_acquire) == 0;
        });
    }

    void FinishProvisioningReset() {
        std::lock_guard<std::mutex> lock(transition_mutex_);
        const uint64_t state = state_.load(std::memory_order_relaxed);
        state_.store(Pack(Generation(state), kProvisioning), std::memory_order_release);
    }

    void EndProvisioningAndRearm() {
        std::lock_guard<std::mutex> lock(transition_mutex_);
        const uint64_t state = state_.load(std::memory_order_relaxed);
        state_.store(Pack(Generation(state) + 1, 0), std::memory_order_release);
    }

private:
    static constexpr uint8_t kProvisioning = 1u << 0;
    static constexpr uint8_t kQuiescing = 1u << 1;
    static constexpr uint8_t kRunning = 1u << 2;
    static constexpr uint64_t kFlagMask = 0xffu;

    static uint64_t Pack(uint64_t generation, uint8_t flags) {
        return (generation << 8) | flags;
    }
    static uint64_t Generation(uint64_t state) { return state >> 8; }
    static uint8_t Flags(uint64_t state) { return static_cast<uint8_t>(state & kFlagMask); }

    Lease TryAcquire(uint8_t required_flags, uint64_t required_generation) {
        active_leases_.fetch_add(1, std::memory_order_acq_rel);
        const uint64_t state = state_.load(std::memory_order_acquire);
        const uint8_t flags = Flags(state);
        const bool allowed = (flags & (kProvisioning | kQuiescing)) == 0 &&
                             (flags & required_flags) == required_flags &&
                             (required_generation == 0 ||
                              Generation(state) == required_generation);
        if (!allowed) {
            ReleaseLease();
            return {};
        }
        return Lease(this, Generation(state));
    }

    void ReleaseLease() {
        if (active_leases_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            idle_cv_.notify_all();
        }
    }

    std::atomic<uint64_t> state_{Pack(1, 0)};
    std::atomic<uint32_t> active_leases_{0};
    std::mutex transition_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable idle_cv_;
};

#endif  // WAKE_WORD_LIFECYCLE_CONTROLLER_H
