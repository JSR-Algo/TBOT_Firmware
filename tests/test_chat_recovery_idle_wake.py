"""Chat playout/START recovery must not leave claimed idle without Hi ESP."""
import pytest

from test_chat_playout_intake import run_terminal_application


def observe_recovery_wake(fixture, _source, _header):
    fixture = fixture.replace("unsigned cleanups=0;", "unsigned cleanups=0;bool cleanup_wake=false,cleanup_reset=false,claimed=true;")
    fixture = fixture.replace("bool IsDeviceClaimed() { return true; }", "bool IsDeviceClaimed() { return claimed; }")
    original = "uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { ++cleanups;"
    assert original in fixture
    return "#define CONFIG_USE_AUDIO_PROCESSOR 1\n" + fixture.replace(original,
        "uint32_t RequestChatAudioCleanup(uint32_t,bool reset,bool,bool wake,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { cleanup_reset=reset;cleanup_wake=wake;++cleanups;")


@pytest.mark.parametrize("send_wake", [0, 1])
@pytest.mark.parametrize("sanitize", ["address", "thread"])
def test_actual_recovery_restores_claimed_idle_wake(tmp_path, sanitize, send_wake):
    run_terminal_application(tmp_path, sanitize, send_wake, extra_tests=r'''
    auto arm_listener=[](Application& app) {
        ReadyForRearm(app);app.HandleStateChangedEvent();
        app.chat_audio_prepared_=app.chat_rearm_prepared_;
        app.chat_outbound_worker_.RunOnce(now_us);app.PollChatPlayout(now_us);app.HandleStateChangedEvent();
        assert(app.state==kDeviceStateListening && app.chat_rearm_phase_==Application::ChatRearmPhase::Armed);
    };
    auto send=[](Application& app,const char* wire) {
        auto* frame=cJSON_Parse(wire);assert(frame);
        app.HandleChatTerminalStop(app.chat_protocol_signals_,1,{1,7},frame,now_us);cJSON_Delete(frame);
    };
    auto recovered=[](const char* site) {
        std::lock_guard<std::mutex> lock(runtime_log_mutex);
        return std::count(runtime_logs.begin(),runtime_logs.end(),std::string("chat_recovery site=")+site)==1;
    };
    auto clear_logs=[]{std::lock_guard<std::mutex> lock(runtime_log_mutex);runtime_logs.clear();};
    // Invalid terminal identity after a listen timeout still fails closed, but
    // the recovered claimed idle robot keeps local wake detection.
    {
        now_us=1000;clear_logs();
        Application app;arm_listener(app);
        now_us+=16000000;app.HandleListeningWatchdogTick();app.HandleStateChangedEvent();
        assert(app.state==kDeviceStateIdle && app.cleanup_wake);
        now_us+=4000000;
        send(app,R"({"type":"tts","state":"stop","drainId":"invalid","continue_listening":true,"listen_mode":"realtime"})");
        app.PollChatPlayout(now_us);
        assert(recovered("203") && app.chat_playout_recovery_);
        assert(app.cleanup_reset && app.cleanup_wake && !app.microphone_uplink_authorized_);
        for(int tick=0;tick<3;++tick) {
            now_us+=60000000;app.PollChatPlayout(now_us);app.HandleStateChangedEvent();
            assert(app.state==kDeviceStateIdle && app.chat_rearm_phase_==Application::ChatRearmPhase::Recovery);
            assert(app.cleanup_wake && !app.microphone_uplink_authorized_);
        }
        // Hi ESP starts a fresh listen from recovery.
        assert(app.HandleChatWake("Hi ESP",false));
        assert(!app.chat_playout_recovery_ && app.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
    }
    // A dormant drainless listen end on an active listener recovers to idle
    // with wake rather than leaving the robot deaf.
    {
        now_us=1000;clear_logs();
        Application app;arm_listener(app);
        send(app,R"({"type":"tts","state":"stop","continue_listening":false,"listen_mode":"manual"})");
        app.PollChatPlayout(now_us);app.HandleStateChangedEvent();
        assert(recovered("203") && app.state==kDeviceStateIdle && app.cleanup_wake);
        assert(!app.microphone_uplink_authorized_);
    }
    // START recovery follows the same idle wake policy.
    {
        now_us=1000;clear_logs();
        Application app;arm_listener(app);
        app.speaking_generation_=UINT32_MAX-1;
        // A recovered START is never admitted; let the receiver reach expiry.
        receiver_wait=[&]{app.PollChatStart(now_us);now_us+=1000000;};
        Start(app);
        receiver_wait={};
        app.PollChatStart(now_us);app.HandleStateChangedEvent();
        assert(recovered("104") && app.chat_playout_recovery_ && app.cleanup_reset && app.cleanup_wake);
        assert(app.state==kDeviceStateIdle && !app.microphone_uplink_authorized_);
    }
    // Ownership gates still keep the microphone path closed.
    for(int guard=0;guard<4;++guard) {
        now_us=1000;
        Application app;arm_listener(app);
        if(guard==0)app.claimed=false;
        if(guard==1)app.connect_in_flight_=true;
        if(guard==2)app.lesson_asset_sync_quiet_=true;
        if(guard==3)app.lesson_runtime_active_=true;
        app.RecoverChatPlayout(203);
        assert(app.chat_playout_recovery_ && app.cleanup_reset && !app.cleanup_wake);
        app.lesson_runtime_active_=false;
        app.HandleStateChangedEvent();
        assert(!app.cleanup_wake && !app.microphone_uplink_authorized_);
    }
    ''', fixture_transform=observe_recovery_wake)
