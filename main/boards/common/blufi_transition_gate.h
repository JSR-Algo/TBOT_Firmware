#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

class BlufiTransitionGate {
public:
    enum class Operation {
        kInit,
        kDeinit,
    };

    class Turn {
    public:
        bool owner() const { return owner_; }
        int result() const { return result_; }

    private:
        friend class BlufiTransitionGate;
        bool owner_ = false;
        int result_ = 0;
        uint64_t epoch_ = 0;
    };

    explicit BlufiTransitionGate(int reentrant_failure)
        : reentrant_failure_(reentrant_failure) {}

    Turn Acquire(Operation operation, uintptr_t owner_token) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (active_) {
                if (active_owner_token_ == owner_token) {
                    Turn turn;
                    turn.result_ = reentrant_failure_;
                    return turn;
                }
                if (active_operation_ == operation) {
                    const uint64_t observed_epoch = active_epoch_;
                    ++same_operation_waiters_;
                    cv_.wait(lock, [&]() { return completed_epoch_ >= observed_epoch; });
                    Turn turn;
                    turn.result_ = completed_result_;
                    if (--same_operation_waiters_ == 0) {
                        cv_.notify_all();
                    }
                    return turn;
                }
                cv_.wait(lock, [this]() {
                    return !active_ && same_operation_waiters_ == 0;
                });
                continue;
            }
            if (same_operation_waiters_ != 0) {
                cv_.wait(lock, [this]() { return same_operation_waiters_ == 0; });
                continue;
            }

            active_ = true;
            active_operation_ = operation;
            active_owner_token_ = owner_token;
            active_epoch_ = ++next_epoch_;
            Turn turn;
            turn.owner_ = true;
            turn.epoch_ = active_epoch_;
            return turn;
        }
    }

    void Complete(const Turn& turn, int result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!turn.owner_ || !active_ || turn.epoch_ != active_epoch_) {
            return;
        }
        completed_epoch_ = active_epoch_;
        completed_result_ = result;
        active_ = false;
        active_owner_token_ = 0;
        cv_.notify_all();
    }

    bool IsTransitionActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

    size_t WaitingSameOperationCallers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return same_operation_waiters_;
    }

private:
    const int reentrant_failure_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool active_ = false;
    Operation active_operation_ = Operation::kInit;
    uintptr_t active_owner_token_ = 0;
    uint64_t next_epoch_ = 0;
    uint64_t active_epoch_ = 0;
    uint64_t completed_epoch_ = 0;
    int completed_result_ = 0;
    size_t same_operation_waiters_ = 0;
};
