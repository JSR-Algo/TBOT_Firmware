"""A long, progressing response must not trip the speaking inactivity timer."""
import pytest

from test_chat_playout_intake import run_terminal_application


@pytest.mark.parametrize("sanitize", ["address", "thread"])
def test_accepted_audio_refreshes_speaking_timeout(tmp_path, sanitize):
    def queue_admission(fixture, source, header):
        fixture = fixture.replace("unsigned snapshot_calls=0;", "unsigned snapshot_calls=0;std::function<bool()> decode_admission;")
        return fixture.replace("received_generations.push_back(packet->generation);",
                               "if(decode_admission && !decode_admission())return false;received_generations.push_back(packet->generation);")

    run_terminal_application(tmp_path, sanitize, 0, extra_tests=r'''
    now_us=1000;
    Application active;Setup(active);active.state=kDeviceStateSpeaking;
    active.chat_protocol_signals_->start_audio=active.chat_playout_response_;
    active.last_speaking_activity_ms_=1;
    for(int seconds : {11,22,33,44}) {
        now_us=seconds*1000000ULL;
        active.HandleChatAudio(active.chat_protocol_signals_,1,{1,7},std::make_unique<AudioStreamPacket>());
        assert(active.last_speaking_activity_ms_==seconds*1000);
        active.HandleSpeakingTimeout(1);
        assert(!active.chat_control_intents_.Size() && active.state==kDeviceStateSpeaking);
    }
    assert(active.timeout_rearms==4);
    const auto accepted=active.last_speaking_activity_ms_.load();
    now_us+=1000000;
    active.HandleChatAudio(active.chat_protocol_signals_,1,{2,8},std::make_unique<AudioStreamPacket>());
    active.HandleChatAudio(active.chat_protocol_signals_,2,{1,7},std::make_unique<AudioStreamPacket>());
    active.HandleChatAudio(active.chat_protocol_signals_,1,{1,7},nullptr);
    active.tts_audio_accepting_=false;
    active.HandleChatAudio(active.chat_protocol_signals_,1,{1,7},std::make_unique<AudioStreamPacket>());
    assert(active.last_speaking_activity_ms_==accepted);
    active.tts_audio_accepting_=true;
    active.audio_service_.decode_admission=[] { return false; };
    active.HandleChatAudio(active.chat_protocol_signals_,1,{1,7},std::make_unique<AudioStreamPacket>());
    assert(active.last_speaking_activity_ms_==accepted);
    active.audio_service_.decode_admission=[&] { ++active.speaking_generation_;return true; };
    active.HandleChatAudio(active.chat_protocol_signals_,1,{1,7},std::make_unique<AudioStreamPacket>());
    assert(active.last_speaking_activity_ms_==accepted);
    --active.speaking_generation_;active.audio_service_.decode_admission={};
    now_us=(accepted+Application::kSpeakingTimeoutMs)*1000;
    active.HandleSpeakingTimeout(1);
    assert(active.chat_control_intents_.Size()==1 && !active.tts_audio_accepting_);
    ''', fixture_transform=queue_admission)
