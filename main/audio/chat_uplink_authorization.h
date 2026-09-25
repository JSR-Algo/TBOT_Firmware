#pragma once

#include <atomic>
#include <cstdint>

struct ChatCaptureTag {
    uint32_t generation = 0;
    bool chat_scope = false;
    bool operator==(const ChatCaptureTag& other) const {
        return generation == other.generation && chat_scope == other.chat_scope;
    }
    bool operator!=(const ChatCaptureTag& other) const { return !(*this == other); }
};

// Revoke/Arm have one writer (Application) and never acquire audio locks. Zero
// is the initial legacy era. Low bits encode legacy(0), revoked(1), chat(2).
// Target qualification is disassembly-based, not is_always_lock_free on Xtensa.
class ChatUplinkAuthorization {
public:
    uint32_t Revoke() {
        const auto old = state_.load(std::memory_order_acquire);
        const auto next = old >= UINT32_MAX - 7 ? UINT32_MAX :
            ((old & ~3U) + 4U) | 1U;
        state_.store(next, std::memory_order_release);
        return next;
    }
    bool Arm(uint32_t revoked, bool chat_scope = true) {
        if (revoked == 0 || revoked == UINT32_MAX || !(revoked & 1U) ||
            state_.load(std::memory_order_acquire) != revoked ||
            prepared_.load(std::memory_order_acquire) != PreparedTag(revoked, chat_scope).generation) return false;
        state_.store(PreparedTag(revoked, chat_scope).generation, std::memory_order_release);
        return true;
    }
    // Only the serialized audio preparation worker publishes this after all
    // owned buffers are cleared. A stale acknowledgement cannot arm a new era.
    void AcknowledgePrepared(uint32_t revoked, bool chat_scope) {
        prepared_.store(PreparedTag(revoked, chat_scope).generation, std::memory_order_release);
    }
    static ChatCaptureTag PreparedTag(uint32_t revoked, bool chat_scope) {
        return {(revoked & ~3U) | (chat_scope ? 2U : 0U), chat_scope};
    }
    bool IsRevoked(uint32_t expected) const {
        return expected != UINT32_MAX && (expected & 3U) == 1U &&
            state_.load(std::memory_order_acquire) == expected;
    }
    ChatCaptureTag Capture() const {
        const auto state = state_.load(std::memory_order_acquire);
        return {state, (state & 3U) == 2U};
    }
    bool Accepts(ChatCaptureTag tag) const {
        const auto state = state_.load(std::memory_order_acquire);
        return !(state & 1U) && state == tag.generation &&
            tag.chat_scope == ((state & 3U) == 2U);
    }
    bool IsCurrentChat(ChatCaptureTag tag) const {
        return tag.chat_scope && Accepts(tag);
    }

private:
    std::atomic<uint32_t> state_{0};
    std::atomic<uint32_t> prepared_{0};
};
