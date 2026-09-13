#include "chat_outbound_worker.h"

bool ChatOutboundWorker::Publish(const Activation& activation) {
    if (state_.load(std::memory_order_acquire) != 0 || !activation.protocol ||
        !activation.protocol_generation || !activation.connection_epoch ||
        !activation.pop || !activation.current_audio || !activation.notify ||
        !mailbox_.IsCurrent(activation.generation)) return false;
    activation_ = activation;
    audio_failure_.store(0, std::memory_order_release);
    state_.store(1, std::memory_order_release);
    return true;
}

bool ChatOutboundWorker::Current() const {
    return mailbox_.IsCurrent(activation_.generation);
}

bool ChatOutboundWorker::TakeRetired() {
    if (state_.load(std::memory_order_acquire) != 2 || !mailbox_.TryIdle()) return false;
    state_.store(0, std::memory_order_release);
    return true;
}

bool ChatOutboundWorker::FinishControl() {
    completion_waiting_.store(1, std::memory_order_release);
    if (!mailbox_.TryComplete(active_, active_result_)) return false;
    active_present_ = false;
    activation_.notify(activation_.context);
    return true;
}

bool ChatOutboundWorker::RunOnce(uint64_t now_us) {
    using Result = ChatOutboundMailbox::Result;
    if (state_.load(std::memory_order_acquire) != 1) return false;
    if (!Current()) {
        packet_.reset();
        if (active_present_) {
            if (active_result_ == Result::Busy) active_result_ = Result::Stale;
            if (!FinishControl()) return true;
        }
        // Copy callback before publishing final access; after this store the
        // application may return the protocol reservation and replace it.
        const auto notify = activation_.notify;
        auto* context = activation_.context;
        state_.store(2, std::memory_order_release);
        notify(context);
        return false;
    }
    if (audio_failure_.load(std::memory_order_acquire)) return false;
    if (!active_present_ && completion_waiting_.load(std::memory_order_acquire)) return false;
    if (active_present_ && active_result_ != Result::Busy) {
        FinishControl();
        return true;
    }
    if (!active_present_) {
        active_present_ = mailbox_.TryTake(active_);
        active_result_ = Result::Busy;
    }
    if (active_present_) {
        if (!Current() || active_.protocol_generation != activation_.protocol_generation ||
            active_.connection_epoch != activation_.connection_epoch) active_result_ = Result::Stale;
        else if (active_.deadline_us && now_us >= active_.deadline_us) active_result_ = Result::Failed;
        else if (active_.kind == ChatOutboundMailbox::Kind::FullText)
            active_result_ = activation_.protocol->SendChatFullTextIfCurrent(active_, [this] {
                return (!active_.authorization || active_.authorization->load(std::memory_order_acquire)) &&
                    activation_.current_connection && activation_.current_connection(activation_.context,
                    active_.source, active_.protocol_generation, active_.connect_generation);
            });
        else active_result_ = activation_.protocol->SendChatControlIfCurrent(active_, [this] {
            return Current() && mailbox_.IsCurrent(active_.generation);
        });
        if (active_result_ != Result::Busy) FinishControl();
        return true;
    }
    // A retained completion or contended mailbox must not let audio overtake a
    // queued control. No mailbox/publication lock spans queue access or I/O.
    if (!mailbox_.TryIdle()) return true;
    if (!packet_) packet_ = activation_.pop(activation_.context);
    if (!Current()) return true;
    if (!packet_) return false;
    if (!mailbox_.TryIdle()) return true;
    if (!activation_.current_audio(activation_.context, *packet_)) {
        packet_.reset();
        return true;
    }
    const auto result = activation_.protocol->SendChatAudioIfCurrent(
        *packet_, activation_.connection_epoch, [this](const ChatCaptureTag& tag) {
            return Current() && tag == packet_->capture_tag &&
                activation_.current_audio(activation_.context, *packet_);
        });
    if (result != Result::Busy) packet_.reset();
    if (result == Result::Failed) {
        audio_failure_.store(1, std::memory_order_release);
        activation_.notify(activation_.context);
    }
    return true;
}
