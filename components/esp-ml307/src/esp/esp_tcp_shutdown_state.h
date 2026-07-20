#ifndef ESP_TCP_SHUTDOWN_STATE_H_
#define ESP_TCP_SHUTDOWN_STATE_H_

#include <atomic>

class EspTcpShutdownState {
public:
    void TaskStarted() {
        state_.store(State::kRunning);
    }

    bool TaskWillExit() {
        State expected = State::kRunning;
        return state_.compare_exchange_strong(expected, State::kTaskExitCommitted);
    }

    void TaskExited() {
        state_.store(State::kExitedUnjoined);
    }

    void TaskJoined() {
        state_.store(State::kSafe);
    }

    bool NeedsJoin() const {
        return state_.load() != State::kSafe;
    }

    bool CanDeleteSynchronization() const {
        return state_.load() == State::kSafe;
    }

private:
    enum class State {
        kSafe,
        kRunning,
        kTaskExitCommitted,
        kExitedUnjoined,
    };

    std::atomic<State> state_{State::kSafe};
};

using EspSslShutdownState = EspTcpShutdownState;

#endif  // ESP_TCP_SHUTDOWN_STATE_H_
