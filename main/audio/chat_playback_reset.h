#pragma once
#include <atomic>
#include <cstdint>

// Application is the sole requester; serialized audio cleanup acknowledges.
// Zero is the unchanged legacy path; saturation permanently fences chat decode.
class ChatPlaybackReset {
public:
    uint32_t Request() {
        const auto value = requested_.load(std::memory_order_relaxed);
        const auto next = value == UINT32_MAX ? value : value + 1;
        requested_.store(next, std::memory_order_release);
        return next;
    }
    uint32_t Requested() const { return requested_.load(std::memory_order_acquire); }
    bool Current(uint32_t token) const { return token && token != UINT32_MAX && Requested() == token; }
    bool Pending() const {
        const auto token = Requested();
        return token == UINT32_MAX || token != completed_.load(std::memory_order_acquire);
    }
    bool AllowsDecode(uint32_t token) const {
        return token == 0 || (Current(token) && completed_.load(std::memory_order_acquire) == token);
    }
    bool Complete(uint32_t token) {
        if (!Current(token)) return false;
        completed_.store(token, std::memory_order_release);
        return Current(token);
    }
private:
    std::atomic<uint32_t> requested_{0}, completed_{0};
};
