"""A completed normal-chat stop must leave Hi ESP available in claimed idle."""
import pytest

from test_chat_playout_intake import run_terminal_application


@pytest.mark.parametrize("sanitize", ["address", "thread"])
def test_idle_stop_and_abort_restore_wake(tmp_path, sanitize):
    def observe_cleanup(fixture, source, header):
        fixture = fixture.replace("unsigned cleanups=0;", "unsigned cleanups=0;bool cleanup_wake=false,claimed=true;")
        fixture = fixture.replace("bool IsDeviceClaimed() { return true; }", "bool IsDeviceClaimed() { return claimed; }")
        return fixture.replace(
            "uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { ++cleanups;",
            "uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool wake,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { cleanup_wake=wake;++cleanups;")

    run_terminal_application(tmp_path, sanitize, 0, extra_tests=r'''
    {
        for(int action=0;action<3;++action) {
            for(int guard=0;guard<4;++guard) {
                now_us=1000;
                Application idle;ReadyForRearm(idle);
                idle.HandleStateChangedEvent();
                idle.chat_outbound_worker_.RunOnce(now_us);idle.PollChatPlayout(now_us);
                idle.chat_audio_prepared_=idle.chat_rearm_prepared_;
                idle.HandleStateChangedEvent();
                assert(idle.state==kDeviceStateListening);
                if(guard==1)idle.claimed=false;
                if(guard==2)idle.connect_in_flight_=true;
                if(guard==3)idle.lesson_asset_sync_quiet_=true;
                now_us=20000000;
                if(action==0)idle.HandleStopListeningEvent();
                if(action==1)idle.HandleChatAbort(kAbortReasonNone,false);
                if(action==2)idle.HandleListeningWatchdogTick();
                idle.HandleStateChangedEvent();
                assert(idle.state==kDeviceStateIdle && !idle.microphone_uplink_authorized_);
                assert(idle.cleanup_wake==(guard==0) && "claimed idle must restore wake after stopping chat");
            }
        }
        now_us=100;
    }
    ''', fixture_transform=lambda fixture, source, header:
        "#define CONFIG_USE_AUDIO_PROCESSOR 1\n"+observe_cleanup(fixture, source, header))
