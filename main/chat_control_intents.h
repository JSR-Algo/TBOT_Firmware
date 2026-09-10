#pragma once
#include <array>
#include "chat_outbound_mailbox.h"
#include "protocols/connection_source.h"

// Application-owned FIFO. Accepted commands stay immutable until delivered or
// explicitly superseded; no old command is rebound to a successor socket.
class ChatControlIntents {
public:
    enum class Outcome { Pending, Sent, Failed, Superseded };
    struct Intent {
        ConnectionSource source;
        uint64_t protocol_generation = 0;
        uint64_t reservation = 0;
        uint32_t connect_generation = 0, response_generation = 0;
        ChatOutboundMailbox::Job job;
        bool admitted = false;
        bool resolve_wake = false;
        Outcome outcome = Outcome::Pending;
    };
    bool Push(const Intent& intent) {
        if (size_ == slots_.size()) return false;
        slots_[(head_ + size_) % slots_.size()] = intent;
        ++size_;
        return true;
    }
    Intent* Front() { return size_ ? &slots_[head_] : nullptr; }
    bool PrependActive(const Intent& intent) {
        if (size_ == slots_.size()) return false;
        head_ = (head_ + slots_.size() - 1) % slots_.size();
        slots_[head_] = intent;
        ++size_;
        return true;
    }
    void Pop() {
        if (!size_) return;
        slots_[head_] = {};
        head_ = (head_ + 1) % slots_.size();
        --size_;
    }
    void Supersede() {
        for (size_t n = 0; n < size_; ++n) {
            auto& intent = slots_[(head_ + n) % slots_.size()];
            if (intent.outcome == Outcome::Pending) intent.outcome = Outcome::Superseded;
        }
    }
    size_t Size() const { return size_; }
private:
    std::array<Intent, 4> slots_{};
    size_t head_ = 0, size_ = 0;
};
