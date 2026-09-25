#ifndef BACKEND_RECOVERY_WINDOW_H
#define BACKEND_RECOVERY_WINDOW_H

#include <cstdint>

class BackendRecoveryWindow {
public:
    explicit BackendRecoveryWindow(uint64_t timeout_ms) : timeout_ms_(timeout_ms) {}

    bool ShouldEnterWifiConfig(uint64_t now_ms) {
        if (!active_) {
            active_ = true;
            started_ms_ = now_ms;
            return false;
        }
        if (fired_ || now_ms - started_ms_ < timeout_ms_) {
            return false;
        }
        fired_ = true;
        return true;
    }

    void Reset() {
        active_ = false;
        fired_ = false;
        started_ms_ = 0;
    }

private:
    const uint64_t timeout_ms_;
    uint64_t started_ms_ = 0;
    bool active_ = false;
    bool fired_ = false;
};

#endif  // BACKEND_RECOVERY_WINDOW_H
