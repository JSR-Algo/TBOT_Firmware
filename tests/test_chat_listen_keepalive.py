"""Google Live's no-audio listen refresh must not invalidate a drained reply."""
import pytest

from test_chat_playout_intake import run_terminal_application


@pytest.mark.parametrize("sanitize", ["address", "thread"])
def test_no_audio_refresh_preserves_active_listener(tmp_path, sanitize):
    run_terminal_application(tmp_path, sanitize, 0, extra_tests=r'''
    auto arm_listener=[](Application& app) {
        ReadyForRearm(app);
        app.HandleStateChangedEvent();
        app.chat_audio_prepared_=app.chat_rearm_prepared_;
        app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatPlayout(now_us);
        app.HandleStateChangedEvent();
        assert(app.state==kDeviceStateListening && app.microphone_uplink_authorized_);
    };
    // Exact envelope from GoogleLiveProvider._send_user_audio_window_expired_feedback.
    auto refresh=[](Application& app, const char* extra="", ConnectionSource source={1,7}) {
        std::string wire=R"({"type":"tts","state":"stop","session_id":"session",
            "continue_listening":true,"listen_mode":"realtime")";
        wire+=extra;wire+="}";
        auto* frame=cJSON_Parse(wire.c_str());assert(frame);
        app.HandleChatTerminalStop(app.chat_protocol_signals_,1,source,frame,now_us);
        cJSON_Delete(frame);
    };
    now_us=100;
    Application listening;arm_listener(listening);
    const auto cleanups=listening.cleanups;
    const auto sends=listening.protocol_->sends.size();
    const auto original_receipt=listening.chat_playout_stop_.received_us;
    for(int repeat=0;repeat<3;++repeat) {
        now_us+=15000000;
        refresh(listening);
        listening.PollChatPlayout(now_us);
        assert(!listening.chat_playout_recovery_);
        listening.HandleStateChangedEvent();
        assert(listening.state==kDeviceStateListening && listening.microphone_uplink_authorized_);
        assert(listening.cleanups==cleanups && listening.protocol_->sends.size()==sends);
        assert(listening.chat_playout_stop_.received_us==original_receipt);
    }
    // The next real reply still owns its START/audio/STOP and drains normally.
    now_us+=100;
    receiver_wait=[&]{listening.PollChatStart(now_us);};
    Start(listening);
    receiver_wait={};
    listening.PollChatStart(now_us);
    assert(!listening.chat_playout_recovery_ && listening.state==kDeviceStateSpeaking);
    Stop(listening,"chat:next");listening.PollChatPlayout(now_us);
    for(int poll=0;poll<3 && !listening.chat_playout_ready_;++poll) {
        listening.chat_outbound_worker_.RunOnce(now_us);listening.PollChatPlayout(now_us);
    }
    assert(listening.chat_playout_ready_ && !listening.chat_playout_recovery_);

    // Never hide missing drain IDs on active speech, interrupts or invalid IDs.
    for(int guard=0;guard<5;++guard) {
        now_us=100;
        Application guarded;arm_listener(guarded);
        if(guard==0)guarded.state=kDeviceStateSpeaking;
        if(guard==1)guarded.microphone_uplink_authorized_=false;
        if(guard==2)guarded.chat_protocol_signals_->start_audio=guarded.chat_playout_response_;
        refresh(guarded,guard==3 ? R"(,"drainId":"invalid")" :
            guard==4 ? R"(,"reason":"interrupt")" : "");
        guarded.PollChatPlayout(now_us);
        assert(guarded.chat_playout_recovery_);
    }
    now_us=100;
    Application stale;arm_listener(stale);
    refresh(stale,"",{2,8});stale.PollChatPlayout(now_us);
    assert(!stale.chat_playout_recovery_ && stale.microphone_uplink_authorized_);
    ''')
