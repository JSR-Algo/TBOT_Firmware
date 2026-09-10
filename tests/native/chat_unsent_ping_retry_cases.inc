void Setup(Application& app);

namespace unsent_ping {
using Outcome = ChatConnectionMessages::Outcome;

// Hold the real mailbox from a second thread; same-thread try_lock is not a
// portable contention fixture for std::mutex.
struct HeldMailbox {
    std::promise<void> held, release;
    std::future<void> holder;
    explicit HeldMailbox(Application& app) {
        auto released = release.get_future().share();
        holder = std::async(std::launch::async, [&, released] {
            std::lock_guard<std::mutex> lock(app.chat_outbound_worker_.mailbox_.mutex_);
            held.set_value();
            released.wait();
        });
        held.get_future().wait();
    }
    ~HeldMailbox() { release.set_value(); holder.get(); }
};

size_t Sends(const Application& app) {
    return std::count(app.protocol_->sends.begin(), app.protocol_->sends.end(), int(Kind::FullText));
}

struct LogicalPing {
    ChatConnectionMessages::Owner owner;
    uint64_t id, deadline;
    std::shared_ptr<const std::string> payload;
    explicit LogicalPing(Application& app) {
        now_us = 100;
        Setup(app);
        app.state = kDeviceStateIdle;
        app.passive_ws_intent_ = true;
        app.online_intent_ = false;
        app.chat_passive_ping_id_ = app.RequestChatConnectionText("{\"type\":\"ping\"}");
        const auto* record = app.chat_connection_messages_.Front();
        assert(record && app.chat_passive_ping_id_ == record->id);
        owner = record->owner; id = record->id;
        deadline = record->deadline_us; payload = record->payload;
        assert(deadline == 10000100ULL);
        runtime_logs.clear();
    }
    void Check(const Application& app, const ChatConnectionMessages::Record* record) const {
        assert(record && record->owner.Matches(owner) && record->id == id);
        assert(record->payload == payload && record->deadline_us == deadline);
        assert(app.chat_passive_ping_id_ == id && record->outcome == Outcome::Pending);
        if (record->submitted) {
            assert(record->physical.full_text == payload && record->physical.deadline_us == deadline);
            assert(record->physical.source.source_id == owner.source.source_id);
            assert(record->physical.source.connection_epoch == owner.source.connection_epoch);
            assert(record->physical.protocol_generation == owner.protocol_generation);
            assert(record->physical.connect_generation == owner.connect_generation);
        }
    }
    void CheckBusy(Application& app) const {
        const auto* record = app.chat_connection_messages_.Front();
        Check(app, record);
        assert(!record->submitted && !record->unsent_completion);
        assert(record->physical.request_id == 0);
        assert(!record->physical.full_text && record->physical.generation == 0);
        assert(Sends(app) == 0);
    }
};

uint64_t BusyAttempt(Application& app, const LogicalPing& ping) {
    const auto previous = app.chat_outbound_request_id_;
    {
        HeldMailbox held(app);
        app.PollChatConnectionMessages(now_us);
    }
    assert(app.chat_outbound_request_id_ == previous + 1);
    ping.CheckBusy(app);
    return app.chat_outbound_request_id_;
}

void CheckNoFault(const Application& app) {
    assert(app.chat_protocol_signals_->MatchesSource({1, 7}));
    assert(!app.chat_protocol_fault_ && !app.chat_outbound_fault_ && !app.chat_playout_recovery_);
    assert(runtime_logs.empty());
}
}

void TestUnsentPingWake() {
    using namespace unsent_ping;
    Application app; LogicalPing ping(app);
    const auto generation = app.chat_outbound_generation_;
    const auto reservation = app.chat_outbound_reservation_;
    const auto attempt = BusyAttempt(app, ping);
    assert(app.HandleChatWake("wake", false));
    assert(app.state == kDeviceStateSpeaking);
#if CONFIG_SEND_WAKE_WORD_DATA
    app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    assert(app.chat_outbound_last_admitted_id_ > attempt);
    app.HandleStateChangedEvent();
    assert(!app.chat_rearm_admitted_);
    assert(app.chat_outbound_worker_.RunOnce(now_us));
    app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    assert(!app.chat_control_intents_.Size());
#endif
    app.HandleStateChangedEvent();
    assert(app.chat_rearm_admitted_ && app.chat_rearm_job_.request_id > attempt);
    app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    auto* record = app.chat_connection_messages_.Front();
    ping.Check(app, record);
    assert(record->submitted && record->physical.request_id > attempt);
    const auto admitted = record->physical.request_id;
    for (int i = 0; i < 4; ++i) {
        app.PollChatConnectionMessages(now_us);
        assert(record->submitted && record->physical.request_id == admitted);
    }
    app.chat_audio_prepared_ = app.chat_rearm_prepared_;
    app.chat_audio_reset_completed_ = app.audio_service_.reset_token;
    for (int i = 0; i < 6; ++i) {
        app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        app.HandleStateChangedEvent();
    }
    assert(Sends(app) == 1 && !app.chat_connection_messages_.Size() && !app.chat_passive_ping_id_);
    assert(app.state == kDeviceStateListening && app.microphone_uplink_authorized_);
    assert(app.chat_outbound_generation_ == generation && app.chat_outbound_reservation_ == reservation);
    CheckNoFault(app);
}

void TestUnsentPingFullQueue() {
    using namespace unsent_ping;
    Application app; LogicalPing ping(app);
    Job first, second, newer;
    first.kind = second.kind = newer.kind = Kind::ListenStop;
    assert(app.SubmitChatOutbound(first) == Result::Sent);
    assert(app.SubmitChatOutbound(second) == Result::Sent);
    app.PollChatConnectionMessages(now_us);
    const auto rejected = app.chat_outbound_request_id_;
    ping.CheckBusy(app);
    assert(app.chat_outbound_worker_.RunOnce(now_us));
    app.PollChatOutbound();
    assert(app.SubmitChatOutbound(newer) == Result::Sent && newer.request_id > rejected);
    app.PollChatConnectionMessages(now_us);
    ping.CheckBusy(app);
    assert(app.chat_outbound_worker_.RunOnce(now_us));
    app.PollChatOutbound();
    app.PollChatConnectionMessages(now_us);
    const auto* record = app.chat_connection_messages_.Front();
    ping.Check(app, record);
    assert(record->submitted && record->physical.request_id > newer.request_id);
    for (int i = 0; i < 4; ++i) {
        app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatOutbound();
        app.PollChatConnectionMessages(now_us);
    }
    assert(Sends(app) == 1 && !app.chat_connection_messages_.Size());
    CheckNoFault(app);
}

void TestUnsentPingExpiry() {
    using namespace unsent_ping;
    Application app; LogicalPing ping(app);
    for (uint64_t time : {100ULL, 5000100ULL, 10000099ULL}) {
        now_us = time;
        BusyAttempt(app, ping);
        CheckNoFault(app);
    }
    now_us = ping.deadline;
    const auto allocated = app.chat_outbound_request_id_;
    app.PollChatConnectionMessages(now_us);
    assert(!app.chat_connection_messages_.Size() && !app.chat_passive_ping_id_);
    assert(app.chat_outbound_request_id_ == allocated && Sends(app) == 0);
    assert(runtime_logs == std::vector<std::string>{"chat_source_fault reason=ping_delivery"});
    assert(!app.chat_protocol_signals_->MatchesSource(ping.owner.source));
    now_us = 100;
}

void TestUnsentPingRetirement() {
    using namespace unsent_ping;
    Application app; LogicalPing ping(app);
    BusyAttempt(app, ping);
    const auto generation = app.chat_outbound_generation_;
    const auto reservation = app.chat_outbound_reservation_;
    Job blocking; blocking.kind = Kind::ListenStop;
    assert(app.SubmitChatOutbound(blocking) == Result::Sent);
    Barrier transport; app.protocol_->send_barrier = &transport;
    auto sending = std::async(std::launch::async, [&] { app.chat_outbound_worker_.RunOnce(now_us); });
    transport.Wait();
    app.RetireChatOutbound();
    for (int i = 0; i < 3; ++i) {
        app.PollChatOutbound();
        app.PollChatConnectionMessages(now_us);
        ping.CheckBusy(app);
        assert(!app.chat_outbound_generation_ && app.chat_outbound_reservation_ == reservation);
    }
    transport.Release(); sending.get(); app.protocol_->send_barrier = nullptr;
    app.PollChatOutbound();
    app.PollChatConnectionMessages(now_us);
    ping.CheckBusy(app);
    assert(app.chat_outbound_reservation_ == reservation);
    app.chat_outbound_worker_.RunOnce(now_us);
    app.PollChatOutbound();
    assert(!app.chat_outbound_reservation_);
    app.PollChatConnectionMessages(now_us);
    const auto* record = app.chat_connection_messages_.Front();
    ping.Check(app, record);
    assert(record->submitted && record->physical.generation != generation);
    assert(record->reservation && record->reservation != reservation);
    app.chat_outbound_worker_.RunOnce(now_us);
    app.PollChatOutbound(); app.PollChatConnectionMessages(now_us);
    assert(Sends(app) == 1 && !app.chat_connection_messages_.Size());
    CheckNoFault(app);
}

void TestUnsentPingReplacement() {
    using namespace unsent_ping;
    Application app; LogicalPing ping(app);
    BusyAttempt(app, ping);
    app.protocol_->healthy_epoch = 8;
    app.connect_generation_ = 2;
    assert(app.SelectChatProtocolSource({2, 8}, 1, 2));
    app.PollChatConnectionMessages(now_us);
    for (int i = 0; i < 3; ++i) {
        app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatOutbound(); app.PollChatConnectionMessages(now_us);
    }
    assert(Sends(app) == 0 && !app.chat_connection_messages_.Size() && !app.chat_passive_ping_id_);
    assert(app.chat_protocol_signals_->MatchesSource({2, 8}) && runtime_logs.empty());
}

void TestUnsentPingCompletions() {
    using namespace unsent_ping;
    for (auto result : {Result::Sent, Result::Failed}) {
        for (bool retire : {false, true}) {
            Application app; LogicalPing ping(app);
            BusyAttempt(app, ping);
            app.PollChatConnectionMessages(now_us);
            auto* record = app.chat_connection_messages_.Front();
            assert(record->submitted);
            const auto admitted = record->physical.request_id;
            Barrier transport; app.protocol_->send_barrier = &transport;
            app.protocol_->result = result;
            auto sending = std::async(std::launch::async, [&] { app.chat_outbound_worker_.RunOnce(now_us); });
            transport.Wait();
            {
                HeldMailbox held(app);
                transport.Release(); sending.get(); app.protocol_->send_barrier = nullptr;
                if (retire) app.RetireChatOutbound();
                for (int i = 0; i < 3; ++i) {
                    app.PollChatOutbound(); app.PollChatConnectionMessages(now_us);
                    app.chat_outbound_worker_.RunOnce(now_us);
                    ping.Check(app, record);
                    assert(record->submitted && record->physical.request_id == admitted);
                    assert(Sends(app) == 1);
                }
            }
            app.chat_outbound_worker_.RunOnce(now_us);
            // A published completion also blocks redispatch until collection.
            for (int i = 0; i < 3; ++i) {
                app.PollChatConnectionMessages(now_us);
                app.chat_outbound_worker_.RunOnce(now_us);
                ping.Check(app, record);
                assert(record->submitted && record->physical.request_id == admitted);
                assert(Sends(app) == 1);
            }
            app.PollChatOutbound();
            assert(!record->submitted && record->outcome == (result == Result::Sent ? Outcome::Sent : Outcome::Failed));
            for (int i = 0; i < 4; ++i) {
                app.PollChatConnectionMessages(now_us);
                app.chat_outbound_worker_.RunOnce(now_us);
                app.PollChatOutbound();
            }
            assert(Sends(app) == 1 && !app.chat_connection_messages_.Size() && !app.chat_passive_ping_id_);
            if (result == Result::Sent) CheckNoFault(app);
            else {
                assert(runtime_logs == std::vector<std::string>{"chat_source_fault reason=ping_delivery"});
                assert(!app.chat_protocol_signals_->MatchesSource(ping.owner.source));
            }
        }
    }
}
