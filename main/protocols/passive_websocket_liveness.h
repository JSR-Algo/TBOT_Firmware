#ifndef PASSIVE_WEBSOCKET_LIVENESS_H
#define PASSIVE_WEBSOCKET_LIVENESS_H

#include <atomic>
#include <cstdint>

class PassiveWebsocketLiveness {
public:
    enum class Action {
        kNone,
        kSendPing,
        kTimedOut,
    };

    void OnOpened(uint32_t now_ms) {
        last_ping_ms_.store(now_ms, std::memory_order_relaxed);
        awaiting_pong_.store(false, std::memory_order_release);
    }

    void OnPong(uint32_t now_ms) {
        (void)now_ms;
        awaiting_pong_.store(false, std::memory_order_release);
    }

    Action Poll(uint32_t now_ms) {
        if (awaiting_pong_.load(std::memory_order_acquire)) {
            const uint32_t elapsed_ms = now_ms - last_ping_ms_.load(std::memory_order_relaxed);
            if (elapsed_ms < kPongTimeoutMs) {
                return Action::kNone;
            }
            bool expected = true;
            if (awaiting_pong_.compare_exchange_strong(
                    expected, false, std::memory_order_acq_rel)) {
                return Action::kTimedOut;
            }
            return Action::kNone;
        }

        const uint32_t elapsed_ms = now_ms - last_ping_ms_.load(std::memory_order_relaxed);
        if (elapsed_ms < kPingIntervalMs) {
            return Action::kNone;
        }
        last_ping_ms_.store(now_ms, std::memory_order_relaxed);
        awaiting_pong_.store(true, std::memory_order_release);
        return Action::kSendPing;
    }

private:
    static constexpr uint32_t kPingIntervalMs = 2000;
    static constexpr uint32_t kPongTimeoutMs = 4000;

    std::atomic<uint32_t> last_ping_ms_{0};
    std::atomic<bool> awaiting_pong_{false};
};

#endif  // PASSIVE_WEBSOCKET_LIVENESS_H
