#pragma once

#include <atomic>
#include "speaking_arm_gesture.h"

// Output has one publisher; Poll/controller state has one worker owner.
// The mailbox is bounded and uses only 32-bit atomic loads/stores.
class SpeakingArmDispatch {
public:
    // BeginResponse has one producer (the protocol intake task).
    void BeginResponse(uint32_t response) {
        if (last_response_ == response) return;
        last_response_ = response;
        owner_.store(response);
    }
    void Cancel() { owner_.store(0); }

    // Evidence means OutputData returned, not proof of audible/physical output.
    void PublishOutput(uint32_t response, bool conversation, uint32_t now_ms) {
        sequence_.store(++publisher_sequence_);
        output_response_.store(conversation ? response : 0);
        output_ms_.store(now_ms);
        sequence_.store(++publisher_sequence_);
    }

    template <typename Transport>
    void Poll(uint64_t now_ms, bool eligible, Transport&& transport) {
        const auto response = owner_.load();
        if (!response) return;
        if (!eligible) { Cancel(); return; }
        const auto before = sequence_.load();
        const auto output_response = output_response_.load();
        const auto output_ms = output_ms_.load();
        if ((before & 1) || before != sequence_.load() ||
            output_response != response || static_cast<uint32_t>(now_ms) - output_ms > 150 ||
            owner_.load() != response) return;
        controller_.BeginResponse(response);
        const auto target = controller_.Observe(response, true, true, now_ms);
        if (target) transport(*target, [this, response]() {
            return owner_.load() == response;
        });
    }

private:
    std::atomic<uint32_t> owner_{0};
    uint32_t last_response_ = 0;
    uint32_t publisher_sequence_ = 0;
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint32_t> output_response_{0};
    std::atomic<uint32_t> output_ms_{0};
    SpeakingArmGesture controller_;
};
