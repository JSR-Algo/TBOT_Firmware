#pragma once

#include <array>
#include <atomic>
#include <cstdint>

// Reservations and actions are changed only on the application task. Workers
// carry the token until their last protocol access and return it via Schedule.
class ProtocolWorkLifetime {
public:
    enum class Action : uint8_t { kNone, kClose, kReset, kReinitialize, kReboot };
    uint64_t Reserve() {
        if (Pending()) return 0;
        for (auto& slot : reservations_) {
            if (slot == 0) {
                slot = ++next_;
                return slot;
            }
        }
        return 0;
    }
    bool Release(uint64_t token) {
        if (!token) return false;
        for (auto& slot : reservations_) {
            if (slot == token) { slot = 0; return true; }
        }
        return false;
    }
    bool Busy() const {
        for (auto slot : reservations_) if (slot) return true;
        return false;
    }
    bool BusyExcept(uint64_t token) const {
        for (auto slot : reservations_) if (slot && slot != token) return true;
        return false;
    }
    bool Pending() const { return pending_.load() != Action::kNone; }
    void Request(Action action) {
        if (action > pending_.load()) pending_.store(action);
    }
    Action TakeReady() {
        if (Busy()) return Action::kNone;
        return pending_.exchange(Action::kNone);
    }
private:
    // Running work plus the two queue slots; one spare for submission unwind.
    std::array<uint64_t, 4> reservations_{};
    uint64_t next_ = 0;
    std::atomic<Action> pending_{Action::kNone};
};
