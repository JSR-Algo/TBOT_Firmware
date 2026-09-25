#pragma once
#include "chat_outbound_mailbox.h"
#include "protocols/connection_source.h"
#include <memory>
#include <string>

// App-owned logical requests outlive response-worker generations. Only proven
// unsent physical attempts may be rebound, after their exact reservation retires.
class ChatConnectionMessages {
public:
    struct Owner {
        ConnectionSource source;
        uint64_t protocol_generation = 0;
        uint32_t connect_generation = 0;
        bool Matches(const Owner& other) const {
            return source.source_id == other.source.source_id && source.connection_epoch == other.source.connection_epoch &&
                protocol_generation == other.protocol_generation && connect_generation == other.connect_generation;
        }
    };
    enum class Outcome { Pending, Sent, Failed, Cancelled };
    struct Record {
        Owner owner;
        uint64_t id = 0, deadline_us = 0, reservation = 0;
        std::shared_ptr<const std::string> payload;
        std::shared_ptr<std::atomic<bool>> authorization;
        ChatOutboundMailbox::Job physical;
        bool submitted = false, unsent_completion = false;
        Outcome outcome = Outcome::Pending;
    };
    uint64_t Admit(Owner owner, const std::string& text, uint64_t received_us,
                   std::shared_ptr<std::atomic<bool>> authorization = {}) {
        if (size_ == records_.size() || text.empty() || text.size() > 65535 ||
            !owner.source.Valid() || !owner.protocol_generation || !owner.connect_generation ||
            next_id_ == UINT64_MAX || received_us > UINT64_MAX - 10000000ULL) return 0;
        std::shared_ptr<const std::string> payload;
        try { payload = std::make_shared<const std::string>(text); } catch (...) { return 0; }
        auto& record = records_[(head_ + size_) % records_.size()];
        record = {};
        record.owner = owner;
        record.id = ++next_id_;
        record.deadline_us = received_us + 10000000ULL;
        record.payload = std::move(payload);
        record.authorization = std::move(authorization);
        ++size_;
        return record.id;
    }
    Record* Front() { return size_ ? &records_[head_] : nullptr; }
    size_t Size() const { return size_; }
    void Pop() {
        if (!size_ || records_[head_].submitted) return;
        records_[head_] = {};
        head_ = (head_ + 1) % records_.size();
        --size_;
    }
    bool Deliver(const ChatOutboundMailbox::Completion& completion) {
        for (auto& record : records_) {
            if (!record.id || !record.submitted || record.physical.request_id != completion.job.request_id ||
                record.physical.generation != completion.job.generation ||
                record.physical.protocol_generation != completion.job.protocol_generation ||
                record.physical.connection_epoch != completion.job.connection_epoch) continue;
            using Result = ChatOutboundMailbox::Result;
            if (completion.result == Result::Sent || completion.result == Result::Failed) {
                if (completion.result == Result::Failed && record.authorization)
                    record.authorization->store(false, std::memory_order_release);
                if (record.outcome == Outcome::Pending)
                    record.outcome = completion.result == Result::Sent ? Outcome::Sent : Outcome::Failed;
                record.submitted = false;
            } else {
                record.unsent_completion = true;
                if (!completion.stale) {
                    record.submitted = false;
                    record.physical = {};
                    record.unsent_completion = false;
                }
            }
            return true;
        }
        return false;
    }
    void ObserveRetirement(uint64_t reservation) {
        for (auto& record : records_) {
            if (!record.id || !record.submitted || record.reservation != reservation) continue;
            record.submitted = false;
            record.unsent_completion = false;
            record.physical = {};
            record.reservation = 0;
        }
    }
    void Cancel(const Owner& current) {
        for (auto& record : records_)
            if (record.id && !record.owner.Matches(current) && record.outcome == Outcome::Pending)
                record.outcome = Outcome::Cancelled;
    }
private:
    std::array<Record, 2> records_{};
    size_t head_ = 0, size_ = 0;
    uint64_t next_id_ = 0;
};
