#pragma once

#include "protocols/protocol.h"
#include "chat_outbound_mailbox.h"
#include <atomic>
#include <memory>

// One application publisher and one persistent worker. The application reserves
// protocol lifetime before Publish and releases it only after TakeRetired.
class ChatOutboundWorker {
public:
    struct Activation {
        Protocol* protocol = nullptr;
        uint64_t protocol_generation = 0;
        uint32_t connection_epoch = 0;
        uint32_t generation = 0;
        void* context = nullptr;
        std::unique_ptr<AudioStreamPacket> (*pop)(void*) = nullptr;
        bool (*current_audio)(void*, const AudioStreamPacket&) = nullptr;
        void (*notify)(void*) = nullptr;
        bool (*current_connection)(void*, ConnectionSource, uint64_t, uint32_t) = nullptr;
    };
    uint32_t AdvanceGeneration() { return mailbox_.AdvanceGeneration(); }
    bool IsCurrent(uint32_t generation) const { return mailbox_.IsCurrent(generation); }
    bool Publish(const Activation& activation);
    bool Submit(const ChatOutboundMailbox::Job& job) { return mailbox_.TrySubmit(job); }
    bool Collect(ChatOutboundMailbox::Completion& completion) {
        if (!mailbox_.TryCollect(completion)) return false;
        completion_waiting_.store(0, std::memory_order_release);
        return true;
    }
    bool TakeRetired();
    // true requests a bounded retry tick; false waits for a coalesced notification.
    bool RunOnce(uint64_t now_us);
    bool TakeAudioFailure() const { return audio_failure_.load(std::memory_order_acquire) != 0; }

private:
    bool Current() const;
    bool FinishControl();
    ChatOutboundMailbox mailbox_;
    Activation activation_{};
    // 0: unpublished; 1: worker owns publication; 2: final access acknowledged.
    std::atomic<uint32_t> state_{0};
    std::atomic<uint32_t> audio_failure_{0};
    std::atomic<uint32_t> completion_waiting_{0};
    std::unique_ptr<AudioStreamPacket> packet_;
    ChatOutboundMailbox::Job active_{};
    ChatOutboundMailbox::Result active_result_ = ChatOutboundMailbox::Result::Busy;
    bool active_present_ = false;
};
