#pragma once

#include "chat_playout_intake.h"
#include <cstring>
#include <mutex>

// One latest sentence; neither receiver nor UI waits for the other task.
class ChatCaptionMailbox {
public:
    static constexpr size_t kTextCapacity = 768;
    struct Message {
        ChatPlayoutIntake::Response owner;
        bool assistant = false;
        char text[kTextCapacity]{};
    };

    bool Publish(const ChatPlayoutIntake::Response& owner, bool assistant, const char* text) {
        if (!text) return false;
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        size_t size = strnlen(text, kTextCapacity);
        if (size == kTextCapacity) {
            size = kTextCapacity - 1;
            // Never split a Vietnamese UTF-8 character at the fixed limit.
            while (size && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80) --size;
        }
        message_.owner = owner;
        message_.assistant = assistant;
        memcpy(message_.text, text, size);
        message_.text[size] = '\0';
        pending_ = true;
        return true;
    }

    bool TryTake(Message& message) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !pending_) return false;
        message = message_;
        pending_ = false;
        return true;
    }

private:
    std::mutex mutex_;
    Message message_;
    bool pending_ = false;
};
