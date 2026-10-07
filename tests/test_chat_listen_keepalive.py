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

    // Never hide missing drain IDs on active speech or invalid IDs. A qualified
    // interrupt revokes input and enters cleanup; it is not a no-audio refresh.
    for(int guard=0;guard<5;++guard) {
        now_us=100;
        Application guarded;arm_listener(guarded);
        if(guard==0)guarded.state=kDeviceStateSpeaking;
        if(guard==1)guarded.microphone_uplink_authorized_=false;
        if(guard==2)guarded.chat_protocol_signals_->start_audio=guarded.chat_playout_response_;
        refresh(guarded,guard==3 ? R"(,"drainId":"invalid")" :
            guard==4 ? R"(,"reason":"interrupt")" : "");
        guarded.PollChatPlayout(now_us);
        if(guard==4) {
            assert(!guarded.chat_playout_recovery_ && !guarded.microphone_uplink_authorized_);
            assert(guarded.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
            assert(guarded.cleanups==2 && !guarded.chat_playout_ready_);
        } else assert(guarded.chat_playout_recovery_);
    }
    now_us=100;
    Application stale;arm_listener(stale);
    refresh(stale,"",{2,8});stale.PollChatPlayout(now_us);
    assert(!stale.chat_playout_recovery_ && stale.microphone_uplink_authorized_);
    ''')


def observe_idle_wake(fixture, _source, _header):
    fixture = fixture.replace("unsigned cleanups=0;", "unsigned cleanups=0;bool cleanup_wake=false;")
    original = "uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { ++cleanups;"
    assert original in fixture
    return "#define CONFIG_USE_AUDIO_PROCESSOR 1\n" + fixture.replace(original,
        "uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool wake,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { cleanup_wake=wake;++cleanups;")


@pytest.mark.parametrize("send_wake", [0, 1])
@pytest.mark.parametrize("sanitize", ["address", "thread"])
def test_no_audio_refresh_after_listen_timeout_keeps_idle_wake(tmp_path, sanitize, send_wake):
    # Device 2026-10-07: greeting drained, realtime listener timed out to idle,
    # then Google Live's no-audio keepalive STOP entered recovery site 203 and
    # left claimed idle without wake word until reboot.
    run_terminal_application(tmp_path, sanitize, send_wake, extra_tests=r'''
    auto arm_listener=[](Application& app) {
        ReadyForRearm(app);
        app.HandleStateChangedEvent();
        app.chat_audio_prepared_=app.chat_rearm_prepared_;
        app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatPlayout(now_us);
        app.HandleStateChangedEvent();
        assert(app.state==kDeviceStateListening && app.microphone_uplink_authorized_);
        assert(app.chat_rearm_phase_==Application::ChatRearmPhase::Armed);
    };
    auto time_out=[](Application& app) {
        now_us+=16000000;
        app.HandleListeningWatchdogTick();
        app.HandleStateChangedEvent();
        app.PollChatControls(now_us);app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatPlayout(now_us);app.PollChatControls(now_us);
        app.HandleStateChangedEvent();
        assert(app.state==kDeviceStateIdle && !app.microphone_uplink_authorized_);
        assert(app.chat_rearm_phase_==Application::ChatRearmPhase::IdleComplete);
        assert(app.cleanup_wake && "listen timeout must restore claimed idle wake");
    };
    // Exact envelope from GoogleLiveProvider._send_user_audio_window_expired_feedback.
    // Exact dormant-mode envelope from the same server method.
    static const char* manual=R"({"type":"tts","state":"stop","session_id":"session",
            "continue_listening":false,"listen_mode":"manual")";
    auto refresh=[](Application& app, const char* extra="", ConnectionSource source={1,7},
        const char* base=R"({"type":"tts","state":"stop","session_id":"session",
            "continue_listening":true,"listen_mode":"realtime")") {
        std::string wire=base;
        wire+=extra;wire+="}";
        auto* frame=cJSON_Parse(wire.c_str());assert(frame);
        app.HandleChatTerminalStop(app.chat_protocol_signals_,1,source,frame,now_us);
        cJSON_Delete(frame);
    };
    auto recovered=[]() {
        std::lock_guard<std::mutex> lock(runtime_log_mutex);
        for(const auto& line:runtime_logs) if(line.rfind("chat_recovery site=",0)==0) return true;
        return false;
    };
    {
        now_us=1000;
        Application idle;arm_listener(idle);time_out(idle);
        const auto cleanups=idle.cleanups;
        const auto speaking=idle.speaking_generation_.load();
        {std::lock_guard<std::mutex> lock(runtime_log_mutex);runtime_logs.clear();}
        for(int repeat=0;repeat<3;++repeat) {
            now_us+=4000000;
            refresh(idle);
            idle.PollChatPlayout(now_us);
            idle.HandleStateChangedEvent();
            assert(!recovered() && !idle.chat_playout_recovery_);
            assert(idle.chat_rearm_phase_==Application::ChatRearmPhase::IdleComplete);
            assert(idle.state==kDeviceStateIdle && !idle.microphone_uplink_authorized_);
            assert(idle.cleanups==cleanups && idle.cleanup_wake && "idle keepalive must not disable wake");
            assert(idle.speaking_generation_==speaking);
        }
        {
            std::lock_guard<std::mutex> lock(runtime_log_mutex);
            assert(std::count(informational_logs.begin(),informational_logs.end(),
                std::string("chat_listen_keepalive_ignored state=idle manual=0"))==3);
        }
        // The dormant-mode drainless listen end is equally satisfied by idle.
        refresh(idle,"",{1,7},manual);idle.PollChatPlayout(now_us);idle.HandleStateChangedEvent();
        assert(!recovered() && !idle.chat_playout_recovery_ && idle.cleanups==cleanups && idle.cleanup_wake);
        // Hi ESP still starts a fresh listen, and the next real reply drains normally.
        now_us+=1000000;
        assert(idle.HandleChatWake("Hi ESP",false));
        assert(idle.chat_rearm_phase_==Application::ChatRearmPhase::Pending && !idle.chat_playout_recovery_);
        assert(!idle.chat_protocol_signals_->intake.ListenerIdle(idle.chat_playout_stamp_));
        receiver_wait=[&]{idle.PollChatStart(now_us);};
        Start(idle);
        receiver_wait={};
        idle.PollChatStart(now_us);
        assert(!idle.chat_playout_recovery_ && idle.state==kDeviceStateSpeaking);
        Stop(idle,"chat:next");idle.PollChatPlayout(now_us);
        for(int poll=0;poll<3 && !idle.chat_playout_ready_;++poll) {
            idle.chat_outbound_worker_.RunOnce(now_us);idle.PollChatPlayout(now_us);
        }
        assert(idle.chat_playout_ready_ && !idle.chat_playout_recovery_ && !recovered());
    }
    // Invalid drain identity, a receiver-owned START and an idle that was not a
    // completed drained listener all keep the existing fail-closed recovery.
    for(int guard=0;guard<3;++guard) {
        now_us=1000;
        Application guarded;arm_listener(guarded);
        if(guard==2) {
            guarded.chat_rearm_phase_=Application::ChatRearmPhase::Pending;
            guarded.state=kDeviceStateListening;
        }
        time_out(guarded);
        if(guard==1)guarded.chat_protocol_signals_->start_audio=guarded.chat_playout_response_;
        now_us+=4000000;
        refresh(guarded,guard==0 ? R"(,"drainId":"invalid")" : "");
        guarded.PollChatPlayout(now_us);
        assert(guarded.chat_playout_recovery_);
    }
    // A dormant drainless listen end on an active listener is not satisfied.
    {
        now_us=1000;
        Application active;arm_listener(active);
        refresh(active,"",{1,7},manual);active.PollChatPlayout(now_us);
        assert(active.chat_playout_recovery_);
    }
    // A drained reply that completed straight to idle (site 401) is equally
    // satisfied, but an invalid drain identity still recovers.
    for(int variant=0;variant<3;++variant) {
        now_us=1000;
        Application replied;Setup(replied);replied.state=kDeviceStateSpeaking;replied.online_intent_=true;
        Stop(replied);replied.PollChatPlayout(now_us);replied.chat_outbound_worker_.RunOnce(now_us);
        replied.PollChatPlayout(now_us);replied.HandleStateChangedEvent();
        assert(replied.state==kDeviceStateIdle && replied.cleanup_wake);
        assert(replied.chat_rearm_phase_==Application::ChatRearmPhase::IdleComplete);
        const auto cleanups=replied.cleanups;
        now_us+=4000000;
        if(variant==1) refresh(replied,"",{1,7},manual);
        else refresh(replied,variant==2 ? R"(,"drainId":"invalid")" : "");
        replied.PollChatPlayout(now_us);replied.HandleStateChangedEvent();
        if(variant==2) assert(replied.chat_playout_recovery_);
        else assert(!replied.chat_playout_recovery_ && replied.cleanups==cleanups && replied.cleanup_wake);
    }
    // A replaced source cannot reach this intake.
    now_us=1000;
    Application stale;arm_listener(stale);time_out(stale);
    refresh(stale,"",{2,8});stale.PollChatPlayout(now_us);
    assert(!stale.chat_playout_recovery_ && stale.cleanup_wake);
    ''', fixture_transform=observe_idle_wake)
