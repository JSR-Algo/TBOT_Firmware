#ifndef ASYNC_LOOKUP_LIFECYCLE_H
#define ASYNC_LOOKUP_LIFECYCLE_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

enum class AsyncLookupStatus {
    kResolved,
    kFailed,
    kTimedOut,
};

struct AsyncLookupResult {
    AsyncLookupStatus status = AsyncLookupStatus::kFailed;
    uint32_t value = 0;
};

class AsyncLookupLifecycle {
public:
    uint32_t TryAcquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kIdle) {
            return 0;
        }
        generation_ = generation_ >= kMaxGeneration ? 1 : generation_ + 1;
        state_ = State::kWaiting;
        success_ = false;
        value_ = 0;
        return generation_;
    }

    template <typename Rep, typename Period>
    AsyncLookupResult WaitFor(uint32_t generation,
                              const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (generation == 0 || generation != generation_) {
            return {AsyncLookupStatus::kFailed, 0};
        }
        const bool completed = completed_cv_.wait_for(lock, timeout, [this]() {
            return state_ == State::kCompleted;
        });
        if (!completed) {
            if (generation == generation_ && state_ == State::kWaiting) {
                state_ = State::kIdle;
            }
            return {AsyncLookupStatus::kTimedOut, 0};
        }

        const AsyncLookupResult result = {
            success_ ? AsyncLookupStatus::kResolved : AsyncLookupStatus::kFailed,
            value_,
        };
        state_ = State::kIdle;
        return result;
    }

    void Complete(uint32_t generation, bool success, uint32_t value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0 || generation != generation_ ||
            state_ != State::kWaiting) {
            return;
        }
        success_ = success;
        value_ = value;
        state_ = State::kCompleted;
        completed_cv_.notify_one();
    }

    bool IsWaiting(uint32_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation != 0 && generation == generation_ &&
               state_ == State::kWaiting;
    }

    bool IsIdle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == State::kIdle;
    }

private:
    static constexpr uint32_t kMaxGeneration = 0x0fffffffu;

    enum class State {
        kIdle,
        kWaiting,
        kCompleted,
    };

    mutable std::mutex mutex_;
    std::condition_variable completed_cv_;
    State state_ = State::kIdle;
    uint32_t generation_ = 0;
    bool success_ = false;
    uint32_t value_ = 0;
};

#endif  // ASYNC_LOOKUP_LIFECYCLE_H
