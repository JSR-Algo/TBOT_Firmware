#include "audio/conversation_playout_controller.h"

#include <cassert>
#include <string>

using C = ConversationPlayoutController;
using K = C::EffectKind;
using D = C::Delivery;

struct Rig {
    C controller;
    C::Ownership owner{1, 2, 3, false};
    PlaybackDrainSnapshot audio{3, 2, false, 0, 0, false, false,
        {7, AudioOutputDrainState::Drained}};
    uint64_t now = 100;

    C::BeginResult Begin(std::string_view id = "chat:1", uint64_t stop = 100) {
        return controller.Begin(stop, {1, 2, 3, id}, owner);
    }
    C::Effect Poll(bool available = true) {
        return controller.Poll(now, owner, available ? &audio : nullptr);
    }
};

static void TestAsyncAckAndDuplicateDelivery() {
    Rig r;
    assert(r.Begin().accepted);
    auto ack = r.Poll();
    assert(ack.kind == K::SubmitAck);
    assert(ack.token().drain_id == "chat:1");
    assert(ack.token().connection_epoch == 1);
    assert(ack.token().response_generation == 2);
    assert(ack.token().reset_epoch == 3);
    assert(r.Poll().kind == K::None);
    r.controller.Deliver(ack.request_id, D::Busy);
    auto retry = r.Poll();
    assert(retry.kind == K::SubmitAck && retry.request_id != ack.request_id);
    r.controller.Deliver(ack.request_id, D::Sent);
    assert(r.Poll().kind == K::None);
    r.controller.Deliver(retry.request_id, D::Sent);
    r.controller.Deliver(retry.request_id, D::Failed);
    assert(r.Poll(false).kind == K::None);
    assert(r.Poll().kind == K::Complete);
    assert(r.Poll().kind == K::None);
    assert(r.Begin().accepted);
    assert(r.Poll().kind == K::None);
}

static void TestEveryBusyStageAndLateLatch() {
    for (int stage = 0; stage < 6; ++stage) {
        Rig r;
        assert(r.Begin().accepted);
        if (stage == 0) r.audio.decode_queue_size = 1;
        if (stage == 1) r.audio.playback_queue_size = 1;
        if (stage == 2) r.audio.decode_in_flight = true;
        if (stage == 3) r.audio.output_in_flight = true;
        if (stage == 4) r.audio.codec.state = AudioOutputDrainState::Busy;
        assert(r.Poll(stage != 5).kind == K::None);
        r.audio.decode_queue_size = r.audio.playback_queue_size = 0;
        r.audio.decode_in_flight = r.audio.output_in_flight = false;
        ++r.audio.codec.epoch; // Delayed EnableOutput is legitimate before latching.
        r.audio.codec.state = AudioOutputDrainState::Pending;
        assert(r.Poll().kind == K::None);
        r.audio.codec.state = AudioOutputDrainState::Drained;
        assert(r.Poll().kind == K::SubmitAck);
    }
    for (bool in_flight : {false, true}) {
        Rig r;
        assert(r.Begin().accepted);
        r.audio.codec.state = in_flight ? AudioOutputDrainState::Drained : AudioOutputDrainState::Pending;
        auto effect = r.Poll();
        if (in_flight) r.controller.Deliver(effect.request_id, D::Sent);
        ++r.audio.codec.epoch;
        r.audio.codec.state = AudioOutputDrainState::Busy;
        assert(r.Poll().kind == K::None); // Busy has no trustworthy epoch.
        r.audio.codec.state = AudioOutputDrainState::Drained;
        assert(r.Poll().kind == K::Cancel);
        assert(r.Poll().kind == K::None);
    }
}

static void TestOriginalDeadlineIncludesDelivery() {
    for (int stage = 0; stage < 5; ++stage) {
        Rig r;
        assert(r.Begin().accepted);
        if (stage == 0) r.audio.playback_queue_size = 1;
        if (stage == 1) r.audio.codec.state = AudioOutputDrainState::Busy;
        auto ack = r.Poll(stage != 2);
        if (stage == 4) r.controller.Deliver(ack.request_id, D::Busy);
        r.now += C::kTimeoutUs - 1;
        assert(r.Begin("chat:1", r.now).accepted);
        assert(r.Poll(stage != 2).kind != K::Complete);
        ++r.now;
        assert(r.Poll(stage != 2).kind == K::Recover);
        r.controller.Deliver(ack.request_id, D::Sent);
        assert(r.Poll().kind == K::None);
        assert(r.Begin().accepted);
        assert(r.Poll().kind == K::None);
    }
}

static void TestOwnershipAndReplacement() {
    for (int fence = 0; fence < 6; ++fence) {
        Rig r;
        assert(r.Begin().accepted);
        auto ack = r.Poll();
        r.controller.Deliver(ack.request_id, D::Sent);
        if (fence == 0) ++r.owner.connection_epoch;
        if (fence == 1) ++r.owner.response_generation;
        if (fence == 2) ++r.owner.reset_epoch;
        if (fence == 3) r.owner.stopped = true;
        if (fence == 4) ++r.audio.reset_epoch;
        if (fence == 5) ++r.audio.playback_generation;
        assert(r.Poll(fence >= 4).kind == K::Cancel);
        assert(r.Poll().kind == K::None);
    }
    Rig r;
    assert(r.Begin().accepted);
    auto old = r.Poll();
    auto replacement = r.Begin("chat:2");
    assert(replacement.accepted && replacement.effect.kind == K::Cancel);
    assert(replacement.effect.token().drain_id == "chat:1");
    assert(old.token().drain_id == "chat:1"); // Worker owns its copied ID.
    auto next = r.Poll();
    r.controller.Deliver(old.request_id, D::Sent);
    assert(r.Poll().kind == K::None);
    r.controller.Deliver(next.request_id, D::Sent);
    assert(r.Poll().kind == K::Complete);
}

static void TestFailureAndDeferredCancel() {
    for (auto failure : {AudioOutputDrainState::Failed, AudioOutputDrainState::Unsupported}) {
        Rig r;
        assert(r.Begin().accepted);
        r.audio.codec.state = failure;
        assert(r.Poll().kind == K::Recover);
        assert(r.Poll().kind == K::None);
    }
    for (auto result : {D::Failed, D::Stale}) {
        Rig r;
        assert(r.Begin().accepted);
        auto ack = r.Poll();
        r.controller.Deliver(ack.request_id, result);
        assert(r.Poll(false).kind == (result == D::Failed ? K::Recover : K::Cancel));
        assert(r.Poll().kind == K::None);
    }
    Rig r;
    assert(r.Begin().accepted);
    r.controller.Cancel();
    assert(r.Poll(false).kind == K::Cancel);
    assert(r.Poll().kind == K::None);
}

static void TestAckRequiresContinuedQuiescence() {
    Rig r;
    assert(r.Begin().accepted);
    auto ack = r.Poll();
    r.controller.Deliver(ack.request_id, D::Sent);
    r.audio.output_in_flight = true;
    assert(r.Poll().kind == K::None);
    r.audio.output_in_flight = false;
    r.audio.codec.state = AudioOutputDrainState::Pending;
    assert(r.Poll().kind == K::None);
    r.audio.codec.state = AudioOutputDrainState::Drained;
    assert(r.Poll().kind == K::Complete);
}

static void TestInvalidStopsDoNotReplace() {
    Rig r;
    assert(!r.Begin("bad").accepted);
    assert(r.Poll().kind == K::None);
    assert(r.Begin().accepted);
    assert(!r.Begin("chat:").accepted);
    auto stale = ConversationPlayoutDrain::Token{0, 2, 3, "chat:old"};
    assert(!r.controller.Begin(0, stale, r.owner).accepted);
    assert(r.Poll().kind == K::SubmitAck);
}

int main() {
    TestAsyncAckAndDuplicateDelivery();
    TestEveryBusyStageAndLateLatch();
    TestOriginalDeadlineIncludesDelivery();
    TestOwnershipAndReplacement();
    TestFailureAndDeferredCancel();
    TestAckRequiresContinuedQuiescence();
    TestInvalidStopsDoNotReplace();
}
