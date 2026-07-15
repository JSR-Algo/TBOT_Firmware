#ifndef WAKE_WORD_LIFECYCLE_GATE_H
#define WAKE_WORD_LIFECYCLE_GATE_H

#include <mutex>
#include <utility>

// Provisioning owns cancellation permanently for the current AudioService instance.
// The shared lock keeps AFE construction and teardown from touching wake_word_ together.
class WakeWordLifecycleGate {
public:
    template <typename Callback>
    bool RunPrewarm(Callback&& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (prewarm_cancelled_) {
            return false;
        }
        std::forward<Callback>(callback)();
        return true;
    }

    template <typename Callback>
    void CancelPrewarmAndRunRelease(Callback&& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        prewarm_cancelled_ = true;
        std::forward<Callback>(callback)();
    }

private:
    std::mutex mutex_;
    bool prewarm_cancelled_ = false;
};

#endif  // WAKE_WORD_LIFECYCLE_GATE_H
