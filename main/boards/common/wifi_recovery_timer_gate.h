#ifndef WIFI_RECOVERY_TIMER_GATE_H
#define WIFI_RECOVERY_TIMER_GATE_H

#include <mutex>
#include <utility>

class WifiRecoveryTimerGate {
public:
    template <typename Validate, typename Arm>
    bool RunArmTransaction(Validate&& validate, Arm&& arm) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!std::forward<Validate>(validate)()) {
            return false;
        }
        std::forward<Arm>(arm)();
        return true;
    }

    template <typename Invalidate>
    void RunInvalidationTransaction(Invalidate&& invalidate) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::forward<Invalidate>(invalidate)();
    }

private:
    std::mutex mutex_;
};

#endif  // WIFI_RECOVERY_TIMER_GATE_H
