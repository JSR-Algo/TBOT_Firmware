#pragma once

#include "wifi_scan_lease_coordinator.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

// Process-lifetime one-shot timer state for an exact BluFi scan lease. Timer
// callbacks never take control_mutex_, so create/start/stop cannot deadlock
// against a callback already dispatched by esp_timer.
class BlufiWifiScanLeaseTimer {
public:
    struct ExactTuple {
        uint64_t request_id = 0;
        WifiScanLeaseCoordinator::Lease lease;
    };

    struct Ops {
        void* context = nullptr;
        bool (*create)(void*, void (*)(void*), void*, void**) noexcept = nullptr;
        void (*stop)(void*, void*) noexcept = nullptr;
        bool (*start_once)(void*, void*, int64_t) noexcept = nullptr;
        int64_t (*now_us)(void*) noexcept = nullptr;
        void (*before_claim)(void*) noexcept = nullptr;
    };

    using Signal = void (*)(void*, ExactTuple) noexcept;

    BlufiWifiScanLeaseTimer(Ops ops, void* signal_context, Signal signal)
        : ops_(ops), signal_context_(signal_context), signal_(signal) {}

    bool Arm(ExactTuple exact, int64_t timeout_us) noexcept {
        std::lock_guard<std::mutex> lock(control_mutex_);
        const uint64_t generation = ++last_generation_;
        armed_generation_.store(0, std::memory_order_release);
        failed_generation_.store(0, std::memory_order_release);
        Publish(exact);
        deadline_us_.store(
            (ops_.now_us ? ops_.now_us(ops_.context) : 0) + timeout_us,
            std::memory_order_release);
        armed_generation_.store(generation, std::memory_order_release);

        if (timer_ == nullptr &&
            (!ops_.create ||
             !ops_.create(ops_.context, &TimerCallback, this, &timer_))) {
            MarkFailed(generation);
            return false;
        }
        if (ops_.stop) {
            ops_.stop(ops_.context, timer_);
        }
        if (!ops_.start_once ||
            !ops_.start_once(ops_.context, timer_, timeout_us)) {
            MarkFailed(generation);
            return false;
        }
        return true;
    }

    bool Disarm(const ExactTuple& exact) noexcept {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if ((armed_generation_.load(std::memory_order_acquire) == 0 &&
             failed_generation_.load(std::memory_order_acquire) == 0) ||
            !SameTuple(exact, Snapshot())) {
            return false;
        }
        armed_generation_.store(0, std::memory_order_release);
        failed_generation_.store(0, std::memory_order_release);
        if (timer_ != nullptr && ops_.stop) {
            ops_.stop(ops_.context, timer_);
        }
        return true;
    }

    std::optional<ExactTuple> CurrentExactTuple() const noexcept {
        if (armed_generation_.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        return Snapshot();
    }

    std::optional<ExactTuple> FailedExactTuple() const noexcept {
        if (failed_generation_.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        return Snapshot();
    }

private:
    static void TimerCallback(void* context) noexcept {
        static_cast<BlufiWifiScanLeaseTimer*>(context)->OnTimer();
    }

    void OnTimer() noexcept {
        uint64_t generation =
            armed_generation_.load(std::memory_order_acquire);
        if (generation == 0) {
            return;
        }
        const int64_t now = ops_.now_us ? ops_.now_us(ops_.context) : 0;
        if (now < deadline_us_.load(std::memory_order_acquire)) {
            return;
        }
        if (ops_.before_claim) {
            ops_.before_claim(ops_.context);
        }
        const ExactTuple exact = Snapshot();
        if (!armed_generation_.compare_exchange_strong(
                generation, 0, std::memory_order_acq_rel,
                std::memory_order_acquire) ||
            !signal_) {
            return;
        }
        failed_generation_.store(0, std::memory_order_release);
        signal_(signal_context_, exact);
    }

    void Publish(const ExactTuple& exact) noexcept {
        tuple_sequence_.fetch_add(1, std::memory_order_acq_rel);
        request_id_.store(exact.request_id, std::memory_order_relaxed);
        owner_.store(static_cast<uint8_t>(exact.lease.owner),
                     std::memory_order_relaxed);
        lease_id_.store(exact.lease.lease_id, std::memory_order_relaxed);
        driver_incarnation_.store(exact.lease.driver_incarnation,
                                  std::memory_order_relaxed);
        tuple_sequence_.fetch_add(1, std::memory_order_release);
    }

    ExactTuple Snapshot() const noexcept {
        for (;;) {
            const uint64_t before =
                tuple_sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0) {
                continue;
            }
            const ExactTuple result{
                request_id_.load(std::memory_order_relaxed),
                WifiScanLeaseCoordinator::Lease{
                    static_cast<WifiScanLeaseCoordinator::Owner>(
                        owner_.load(std::memory_order_relaxed)),
                    lease_id_.load(std::memory_order_relaxed),
                    driver_incarnation_.load(std::memory_order_relaxed)}};
            if (before == tuple_sequence_.load(std::memory_order_acquire)) {
                return result;
            }
        }
    }

    void MarkFailed(uint64_t generation) noexcept {
        uint64_t expected = generation;
        armed_generation_.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
        failed_generation_.store(generation, std::memory_order_release);
    }

    static bool SameTuple(const ExactTuple& left,
                          const ExactTuple& right) noexcept {
        return left.request_id == right.request_id &&
            left.lease.owner == right.lease.owner &&
            left.lease.lease_id == right.lease.lease_id &&
            left.lease.driver_incarnation == right.lease.driver_incarnation;
    }

    Ops ops_;
    void* signal_context_ = nullptr;
    Signal signal_ = nullptr;
    mutable std::mutex control_mutex_;
    void* timer_ = nullptr;
    uint64_t last_generation_ = 0;
    std::atomic<uint64_t> armed_generation_{0};
    std::atomic<uint64_t> failed_generation_{0};
    std::atomic<uint64_t> request_id_{0};
    std::atomic<uint8_t> owner_{0};
    std::atomic<uint64_t> lease_id_{0};
    std::atomic<uint32_t> driver_incarnation_{0};
    std::atomic<int64_t> deadline_us_{0};
    std::atomic<uint64_t> tuple_sequence_{0};
};
