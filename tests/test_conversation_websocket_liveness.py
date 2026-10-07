"""Execute the actual clock-tick gate and ping admission with host boundaries."""
from pathlib import Path
import subprocess

import pytest
from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def liveness_binary(tmp_path_factory):
    source = (ROOT / "main/application.cc").read_text()
    start = source.index("            bool passive_liveness_failed = false;")
    end = source.index("\n            if (!passive_liveness_failed &&", start)
    fixture = r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include "chat_connection_messages.h"
#include "protocol_work_lifetime.h"
#include "protocols/passive_websocket_liveness.h"
#define ESP_LOGW(...) ((void)0)
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking,
                  kDeviceStateWifiConfiguring, kDeviceStateAudioTesting };
struct ChatProtocolSignals {
    enum { Error = 1 };
    bool selected = true, current = true, failed = false;
    bool SourceSelected() { return selected; }
    bool TrySource(ConnectionSource& source) {
        source = {1, 1}; return current && !failed;
    }
    void PublishConnectionFault(ConnectionSource, uint32_t, uint32_t) { failed = true; }
};
struct Protocol {
    PassiveWebsocketLiveness liveness;
    uint32_t now_ms = 3000;
    int ObserveChatPassiveLiveness(ConnectionSource) {
        auto action = liveness.Observe(now_ms);
        return action == PassiveWebsocketLiveness::Action::kTimedOut ? -1 :
            action == PassiveWebsocketLiveness::Action::kSendPing ? 1 : 0;
    }
    bool IsAudioChannelOpened() { return true; }
    bool MaintainPassiveLiveness() { return true; }
    void CloseAudioChannel() {}
};
struct Application {
    std::atomic<bool> passive_ws_intent_{false}, online_intent_{true},
        lesson_runtime_active_{false}, connect_in_flight_{false}, backend_offline_{false};
    std::atomic<uint32_t> chat_source_connect_generation_{1};
    DeviceState state = kDeviceStateListening;
    Protocol transport; Protocol* protocol_ = &transport;
    ChatProtocolSignals signals; ChatProtocolSignals* chat_protocol_signals_ = &signals;
    ChatConnectionMessages chat_connection_messages_;
    uint64_t chat_outbound_reservation_ = 0, chat_passive_ping_id_ = 0;
    ProtocolWorkLifetime protocol_work_lifetime_;
    bool claimed = true, syncing = false, chat_cleanup_enabled_ = true;
    unsigned normal_recovery = 0, passive_recovery = 0;
    struct Controls { unsigned Size() { return 0; } } chat_control_intents_;
    enum class ChatRearmPhase { None, Pending };
    ChatRearmPhase chat_rearm_phase_ = ChatRearmPhase::None;
    uint64_t chat_playout_stamp_ = 0, chat_unpair_id_ = 0, chat_start_obsolete_reservation_ = 0;
    uint32_t chat_outbound_generation_ = 0;
    bool chat_unpair_completed_ = false;
    ChatOutboundMailbox mailbox;
    uint64_t request_id = 0;
    Application() { chat_outbound_reservation_ = protocol_work_lifetime_.Reserve(); }
    DeviceState GetDeviceState() { return state; }
    bool IsDeviceClaimed() { return claimed; }
    bool IsLessonAssetSyncQuiet() { return syncing; }
    bool MaintainChatPassiveLiveness();
    void PollChatConnectionMessages(uint64_t);
    bool IsChatConnectionCurrent(ConnectionSource source, uint64_t generation, uint32_t connect) {
        return signals.current && !signals.failed && source.source_id == 1 &&
            source.connection_epoch == 1 && generation == 1 && connect == 1;
    }
    void RetireChatOutbound() {}
    ChatOutboundMailbox::Result ActivateChatOutbound(uint32_t) {
        chat_outbound_generation_ = mailbox.AdvanceGeneration();
        return ChatOutboundMailbox::Result::Sent;
    }
    ChatOutboundMailbox::Result SubmitChatOutbound(ChatOutboundMailbox::Job& job) {
        job.request_id = ++request_id;
        job.generation = chat_outbound_generation_;
        job.protocol_generation = 1;
        job.connection_epoch = 1;
        return mailbox.TrySubmit(job) ? ChatOutboundMailbox::Result::Sent : ChatOutboundMailbox::Result::Busy;
    }
    void PollChatProtocolSignals() { if (signals.failed) ++normal_recovery; }
    void PollChatProtocolCleanup() {}
    void SchedulePassiveLessonReconnect() { ++passive_recovery; }
    uint64_t RequestChatConnectionText(const std::string& text) {
        return chat_connection_messages_.Admit({{1, 1}, 1, 1}, text, transport.now_ms * 1000ULL);
    }
    void Tick() {
// CLOCK_GATE
        (void)passive_liveness_failed;
    }
};
// PING_METHOD
int main(int argc, char** argv) {
    assert(argc == 2);
    std::string scenario = argv[1];
    Application app;
    app.transport.liveness.OnOpened(1000);
    bool eligible = true, timeout = false;
    if (scenario == "idle_after_chat") app.state = kDeviceStateIdle;
    if (scenario == "speaking") app.state = kDeviceStateSpeaking;
    if (scenario == "passive") { app.passive_ws_intent_ = true; app.online_intent_ = false; }
    if (scenario == "no_intent") { app.online_intent_ = false; eligible = false; }
    if (scenario == "unclaimed") { app.claimed = false; eligible = false; }
    if (scenario == "lesson_answer") { app.lesson_runtime_active_ = true; eligible = false; }
    if (scenario == "legacy_active") { app.signals.selected = false; eligible = false; }
    if (scenario == "syncing") { app.syncing = true; eligible = false; }
    if (scenario == "connecting") { app.connect_in_flight_ = true; eligible = false; }
    if (scenario == "owned") { app.protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose); eligible = false; }
    if (scenario == "busy") { assert(app.protocol_work_lifetime_.Reserve()); eligible = false; }
    if (scenario == "wifi_setup") { app.state = kDeviceStateWifiConfiguring; eligible = false; }
    if (scenario == "audio_test") { app.state = kDeviceStateAudioTesting; eligible = false; }
    if (scenario == "stale_source") { app.signals.current = false; timeout = true; }
    if (scenario == "active_timeout" || scenario == "passive_timeout") {
        timeout = true;
        app.transport.liveness.OnPingSent(3000);
        app.transport.now_ms = 13000;
        if (scenario == "passive_timeout") { app.passive_ws_intent_ = true; app.online_intent_ = false; }
    }
    app.Tick();
    if (timeout) {
        assert(app.chat_connection_messages_.Size() == 0);
        if (scenario == "active_timeout") {
            assert(app.signals.failed && app.normal_recovery == 1 && app.passive_recovery == 0);
        } else if (scenario == "passive_timeout") assert(app.passive_recovery == 1);
        else assert(app.normal_recovery == 0 && app.passive_recovery == 0);
        return 0;
    }
    assert(app.chat_connection_messages_.Size() == (eligible ? 1 : 0));
    if (!eligible) return 0;
    assert(*app.chat_connection_messages_.Front()->payload == "{\"type\":\"ping\"}");
    // A pending request must not create duplicate probes on subsequent ticks.
    app.Tick(); assert(app.chat_connection_messages_.Size() == 1);
    // Exercise the real mailbox and logical delivery before the next probe.
    app.chat_outbound_generation_ = app.mailbox.AdvanceGeneration();
    app.PollChatConnectionMessages(3000000);
    auto* record = app.chat_connection_messages_.Front();
    assert(record->submitted && record->reservation == app.chat_outbound_reservation_);
    ChatOutboundMailbox::Job job;
    assert(app.mailbox.TryTake(job));
    app.transport.liveness.OnPingSent(3000);
    assert(app.mailbox.TryComplete(job, ChatOutboundMailbox::Result::Sent));
    ChatOutboundMailbox::Completion completion;
    assert(app.mailbox.TryCollect(completion));
    assert(app.chat_connection_messages_.Deliver(completion));
    assert(!record->submitted && record->outcome == ChatConnectionMessages::Outcome::Sent);
    app.PollChatConnectionMessages(3100000);
    assert(app.chat_passive_ping_id_ == 0 && app.chat_connection_messages_.Size() == 0);
    app.transport.liveness.OnPong(3100);
    app.transport.now_ms = 5000;
    app.state = kDeviceStateIdle;
    app.Tick(); assert(app.chat_connection_messages_.Size() == 1);
}
'''
    folder = tmp_path_factory.mktemp("conversation-liveness")
    generated = folder / "liveness.cc"
    generated.write_text(fixture.replace("// CLOCK_GATE", source[start:end]).replace(
        "// PING_METHOD", method(source, "bool Application::MaintainChatPassiveLiveness") + "\n" +
        method(source, "void Application::PollChatConnectionMessages")))
    binary = folder / "liveness"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(generated), "-o", str(binary)], check=True, timeout=30)
    return binary


@pytest.mark.parametrize("scenario", [
    "listening", "speaking", "idle_after_chat", "passive", "no_intent", "unclaimed",
    "syncing", "connecting", "owned", "busy", "wifi_setup", "audio_test",
    "active_timeout", "passive_timeout", "stale_source", "lesson_answer", "legacy_active",
])
def test_connected_conversation_liveness(liveness_binary, scenario):
    subprocess.run([str(liveness_binary), scenario], check=True, timeout=20)
