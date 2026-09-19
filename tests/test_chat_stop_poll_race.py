from test_chat_playout_intake import run_terminal_application


def test_stop_received_after_poll_clock_sample_is_not_an_expired_drain(tmp_path):
    run_terminal_application(tmp_path, "address", 1, extra_tests=r'''
    {
        Application app;
        now_us = 1000;
        Setup(app);
        app.state = kDeviceStateSpeaking;
        app.online_intent_ = true;
        app.microphone_uplink_authorized_ = true;
        const auto poll_started_us = now_us.load();
        now_us = poll_started_us + 10;
        Stop(app);
        // The receiver publishes STOP after the app samples its poll clock.
        app.PollChatPlayout(poll_started_us);
        assert(!app.chat_playout_recovery_);
        app.PollChatPlayout(now_us);
        app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatPlayout(now_us);
        assert(!app.chat_playout_recovery_);
        assert(app.chat_playout_ready_);
    }
    ''')


def test_start_received_after_poll_clock_sample_is_not_an_expired_admission(tmp_path):
    run_terminal_application(tmp_path, "address", 1, extra_tests=r'''
    {
        Application app;
        now_us = 1000;
        Setup(app);
        app.state = kDeviceStateListening;
        const auto poll_started_us = now_us.load();
        now_us = poll_started_us + 10;
        auto& handoff = app.chat_protocol_signals_->start;
        ChatStartHandoff::Request request{{1,7},1,1,0,now_us,
            now_us + ChatStartHandoff::kAdmissionUs};
        assert(handoff.Publish(request));
        app.PollChatStart(poll_started_us);
        assert(!app.chat_playout_recovery_);
        app.PollChatStart(now_us);
        assert(handoff.Confirm(request, now_us));
        app.PollChatStart(now_us);
        assert(app.state == kDeviceStateSpeaking);
        assert(app.speaking_arm_dispatch_.begins == 1);
        assert(!app.chat_playout_recovery_);
    }
    ''')


def test_newer_receiver_timestamp_does_not_extend_original_deadlines(tmp_path):
    run_terminal_application(tmp_path, "address", 1, extra_tests=r'''
    {
        Application app;
        now_us = 1000;
        Setup(app);
        app.state = kDeviceStateSpeaking;
        const auto poll_started_us = now_us.load();
        now_us = poll_started_us + 10;
        Stop(app);
        now_us += ConversationPlayoutController::kTimeoutUs;
        app.PollChatPlayout(poll_started_us);
        assert(app.chat_playout_recovery_);
        assert(app.protocol_->sends.empty());
    }
    {
        Application app;
        now_us = 1000;
        Setup(app);
        app.state = kDeviceStateListening;
        const auto poll_started_us = now_us.load();
        now_us = poll_started_us + 10;
        auto& handoff = app.chat_protocol_signals_->start;
        ChatStartHandoff::Request request{{1,7},1,1,0,now_us,
            now_us + ChatStartHandoff::kAdmissionUs};
        assert(handoff.Publish(request));
        now_us += ChatStartHandoff::kAdmissionUs;
        app.PollChatStart(poll_started_us);
        assert(app.chat_playout_recovery_);
        assert(!handoff.Confirm(request, now_us));
        assert(app.speaking_arm_dispatch_.begins == 0);
    }
    ''')
