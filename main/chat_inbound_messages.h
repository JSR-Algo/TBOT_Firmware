#pragma once
#include "chat_connection_messages.h"
#include "cJSON.h"
#include <array>
#include <functional>
#include <memory>
#include <mutex>

struct ChatInboundMessage {
    ChatConnectionMessages::Owner owner;
    uint64_t received_us = 0, deadline_us = 0, lesson_epoch = 0;
    std::string session_id;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root{nullptr, cJSON_Delete};
    std::string EncodeMcpReply(const std::string& payload) const {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> envelope(cJSON_CreateObject(), cJSON_Delete);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> parsed(cJSON_Parse(payload.c_str()), cJSON_Delete);
        if (!envelope || !parsed || !cJSON_AddStringToObject(envelope.get(), "session_id", session_id.c_str()) ||
            !cJSON_AddStringToObject(envelope.get(), "type", "mcp") ||
            !cJSON_AddItemToObject(envelope.get(), "payload", parsed.get())) return {};
        parsed.release();
        std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(envelope.get()), cJSON_free);
        return encoded ? std::string(encoded.get()) : std::string{};
    }
};
using ChatRequestContext = std::shared_ptr<const ChatInboundMessage>;

// Weak permits remain occupied through asynchronous tool continuations, not
// merely until the application removes the message from the handoff queue.
class ChatInboundMessages {
public:
    enum class Admission { Accepted, Busy, Full, Invalid, NoMemory };
    enum class ReadStatus { Item, Empty, Busy };
    struct ReadResult { ReadStatus status; ChatRequestContext context; };
    struct Snapshot { bool available = false; size_t queued = 0; size_t outstanding = 0; };
    Snapshot TrySnapshot() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return {};
        Snapshot snapshot{true, size_, 0};
        for (const auto& permit : permits_) if (!permit.expired()) ++snapshot.outstanding;
        return snapshot;
    }
    Admission TryAdmit(const cJSON* root, ChatConnectionMessages::Owner owner, uint64_t received_us,
        uint64_t lesson_epoch, const std::string& session_id = {},
        const std::function<bool()>& can_publish = {}) {
        if (!root || !owner.source.Valid() || !owner.protocol_generation || !owner.connect_generation ||
            received_us > UINT64_MAX - 10000000ULL) return Admission::Invalid;
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return Admission::Busy;
        bool available = false;
        for (const auto& permit : permits_) if (permit.expired()) { available = true; break; }
        if (!available || size_ == queue_.size()) return Admission::Full;
        auto message = OwnLocked(root, owner, received_us, lesson_epoch, session_id);
        if (!message) return Admission::NoMemory;
        // Only a clock/atomic source check belongs here: no dispatch or waits.
        // Rejection destroys unpublished ownership before the lock is released.
        if (can_publish && !can_publish()) return Admission::Invalid;
        queue_[(head_ + size_) % queue_.size()] = std::move(message);
        ++size_;
        return Admission::Accepted;
    }
    bool Admit(const cJSON* root, ChatConnectionMessages::Owner owner, uint64_t received_us, uint64_t lesson_epoch,
        const std::string& session_id = {}) {
        return TryAdmit(root, owner, received_us, lesson_epoch, session_id) == Admission::Accepted;
    }
    ChatRequestContext Own(const cJSON* root, ChatConnectionMessages::Owner owner, uint64_t received_us,
        uint64_t lesson_epoch, const std::string& session_id = {}) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        return lock.owns_lock() ? OwnLocked(root, owner, received_us, lesson_epoch, session_id) : ChatRequestContext{};
    }
private:
    ChatRequestContext OwnLocked(const cJSON* root, ChatConnectionMessages::Owner owner, uint64_t received_us,
        uint64_t lesson_epoch, const std::string& session_id) {
        if (!root || received_us > UINT64_MAX - 10000000ULL) return {};
        size_t slot = permits_.size();
        for (size_t i = 0; i < permits_.size(); ++i)
            if (permits_[i].expired()) { slot = i; break; }
        if (slot == permits_.size()) return {};
        try {
            auto message = std::make_shared<ChatInboundMessage>();
            message->root.reset(cJSON_Duplicate(root, true));
            if (!message->root) return {};
            message->owner = owner;
            message->received_us = received_us;
            message->deadline_us = received_us + 10000000ULL;
            message->lesson_epoch = lesson_epoch;
            message->session_id = session_id;
            permits_[slot] = message;
            return message;
        } catch (...) { return {}; }
    }
public:
    ReadResult TryTake() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return {ReadStatus::Busy, {}};
        if (!size_) return {ReadStatus::Empty, {}};
        auto message = std::move(queue_[head_]);
        head_ = (head_ + 1) % queue_.size();
        --size_;
        return {ReadStatus::Item, std::move(message)};
    }
    ChatRequestContext Take() { return TryTake().context; }
    bool Pending() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        return !lock.owns_lock() || size_ != 0;
    }
    size_t Outstanding() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return permits_.size();
        size_t count = 0;
        for (const auto& permit : permits_) if (!permit.expired()) ++count;
        return count;
    }
private:
    std::mutex mutex_;
    std::array<std::weak_ptr<const ChatInboundMessage>, 4> permits_{};
    std::array<ChatRequestContext, 4> queue_{};
    size_t head_ = 0, size_ = 0;
};
