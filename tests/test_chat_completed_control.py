"""Completed drain ACKs must never re-enter the control FIFO on stop/abort."""
import pytest

from test_chat_playout_intake import run_terminal_application


@pytest.mark.parametrize("sanitize", ["address", "thread"])
@pytest.mark.parametrize("action", ["stop", "abort", "silence", "snapshot_busy"])
def test_completed_ack_is_not_retained(tmp_path, sanitize, action):
    run_terminal_application(tmp_path, sanitize, 0, extra_tests=r'''
    {
    now_us=1000;
    Application app;Setup(app);app.state=kDeviceStateSpeaking;
    app.online_intent_=true;app.microphone_uplink_authorized_=true;
    Stop(app);app.PollChatPlayout(now_us);
    app.chat_outbound_worker_.RunOnce(now_us);
    const bool snapshot_busy=ACTION==3;
    app.audio_service_.snapshot_busy=snapshot_busy;
    app.PollChatPlayout(now_us);
    if (!snapshot_busy) {
        assert(app.chat_playout_ready_);
        app.HandleStateChangedEvent();
        app.chat_outbound_worker_.RunOnce(now_us);app.PollChatPlayout(now_us);
        app.chat_audio_prepared_=app.chat_rearm_prepared_;
        app.HandleStateChangedEvent();
        assert(app.state==kDeviceStateListening && app.microphone_uplink_authorized_);
        // Both physical sends completed long before the later user action.
        now_us=20000000;
    }
    if (ACTION==0) app.HandleStopListeningEvent();
    if (ACTION==1 || snapshot_busy) app.HandleChatAbort(kAbortReasonNone,false);
    if (ACTION==2) app.HandleListeningWatchdogTick();
    assert(app.chat_control_intents_.Size()==1 && "completed ACK was retained as active");
    app.PollChatControls(now_us);
    assert(!app.chat_playout_recovery_ && "completed ACK expired after relistening");
    app.chat_outbound_worker_.RunOnce(now_us);app.PollChatOutbound();
    assert(!app.chat_control_intents_.Size());
    assert(app.protocol_->sends.back()==int((ACTION==1 || snapshot_busy)?Kind::Abort:Kind::ListenStop));
    assert(app.protocol_->sends.size()==(snapshot_busy?2:3));
    now_us=100;
    }
    '''.replace("ACTION", str(["stop", "abort", "silence", "snapshot_busy"].index(action))),
        fixture_transform=lambda fixture, source, header: "#define CONFIG_USE_AUDIO_PROCESSOR 1\n" + fixture)
