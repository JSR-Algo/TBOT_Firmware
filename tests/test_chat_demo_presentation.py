"""Demo captions must not exhaust control admission during slow display work."""
import subprocess

import pytest

from test_chat_json_admission import CJSON, ROOT
from test_protocol_work_lifetime import method
from test_chat_playout_intake import run_terminal_application


def test_demo_restores_captions_without_control_admission(tmp_path):
    def journey(fixture, source, header):
        fixture=fixture.replace("class Application {", "class Application {\npublic:\n    bool TakeChatCaption(ChatCaptionMailbox::Message&);")
        consumer=method(source, "bool Application::TakeChatCaption")
        return "#define CONFIG_TBOT_VOICE_DEMO 1\n#include \"chat_caption_mailbox.h\"\n" + fixture.split("int main() {", 1)[0] + consumer + r'''
int main() {
    now_us=100;Application app;Setup(app);app.state=kDeviceStateListening;
    ChatCaptionMailbox::Message caption;
    auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
    auto* stt=cJSON_Parse("{\"type\":\"stt\",\"text\":\"hello\"}");
    callbacks.json({1,7},stt,0,{now_us,0});cJSON_Delete(stt);
    assert(app.TakeChatCaption(caption));
    assert(strcmp(caption.text,"hello")==0 && !caption.assistant);
    receiver_wait=[&]{app.PollChatStart(now_us);};Start(app);receiver_wait={};
    app.PollChatStart(now_us);
    auto* tts=cJSON_Parse("{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"reply\"}");
    for(int i=0;i<300;++i)callbacks.json({1,7},tts,0,{now_us,0});
    assert(!app.chat_inbound_messages_.Outstanding());
    assert(app.TakeChatCaption(caption));
    assert(strcmp(caption.text,"reply")==0 && caption.assistant);
    assert(!app.TakeChatCaption(caption));
    for(int obsolete=0;obsolete<7;++obsolete) {
        callbacks.json({1,7},tts,0,{now_us,0});
        if(obsolete==0)app.state=kDeviceStateIdle;
        if(obsolete==1)app.lesson_asset_sync_quiet_=true;
        if(obsolete==2)++app.speaking_generation_;
        if(obsolete==3)++app.connect_generation_;
        if(obsolete==4)++app.protocol_generation_;
        if(obsolete==5)app.chat_protocol_owned_=true;
        if(obsolete==6)app.chat_protocol_signals_->Disable();
        assert(!app.TakeChatCaption(caption));
        if(obsolete==0)app.state=kDeviceStateSpeaking;
        if(obsolete==1)app.lesson_asset_sync_quiet_=false;
        if(obsolete==2)--app.speaking_generation_;
        if(obsolete==3)--app.connect_generation_;
        if(obsolete==4)--app.protocol_generation_;
        if(obsolete==5)app.chat_protocol_owned_=false;
        assert(!app.TakeChatCaption(caption));
    }
    cJSON_Delete(tts);
}
'''
    run_terminal_application(tmp_path, "address", 0, fixture_transform=journey)


@pytest.mark.parametrize("demo", [0, 1])
def test_demo_presentation_burst_preserves_control_queue(tmp_path, demo):
    source = (ROOT / "main/application.cc").read_text()
    fixture = (ROOT / "tests/native/chat_json_admission_test.cc").read_text()
    fixture = fixture.split("int main() {", 1)[0]
    fixture = fixture.replace("struct Application {", "struct Application {\n    std::atomic<uint32_t> speaking_generation_{1};")
    fixture = fixture.replace(
        "bool IsLessonVoiceRoute() const { return false; }",
        "bool lesson_route=false; bool IsLessonVoiceRoute() const { return lesson_route; }",
    )
    names = [
        "void Application::PollChatInboundMessages",
        "bool Application::IsChatConnectionCurrent",
        "bool Application::IsChatRequestCurrent",
        "void Application::FailChatRequest",
        "Protocol::SourceCallbacks Application::MakeChatSourceCallbacks",
    ]
    fixture = fixture.replace(
        "// PRODUCTION_METHODS", "\n".join(method(source, name) for name in names)
    )
    fixture += r'''
int main() {
    Application app;
    auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
    unsigned waits=0;
    receiver_wait=[&]{ ++waits; now_us+=10000; };
    if (CONFIG_TBOT_VOICE_DEMO)
        for(int i=0;i<4;++i)Enqueue(app,i);
    // A display operation can already be holding all four admitted contexts.
    for(int i=0;i<100;++i) {
        for(const char* type : {"stt","llm","tts"}) {
            auto root=Frame(42);
            cJSON_ReplaceItemInObject(root.get(),"type",cJSON_CreateString(type));
            cJSON_AddStringToObject(root.get(),"state","sentence_start");
            cJSON_AddStringToObject(root.get(),"text","caption");
            callbacks.json({1,7},root.get(),88,{now_us,0});
            assert(app.chat_protocol_signals_->MatchesSource({1,7}));
            assert(waits==0);
            if(CONFIG_TBOT_VOICE_DEMO) {
                assert(app.chat_inbound_messages_.Outstanding()==4);
            } else {
                assert(app.chat_inbound_messages_.Outstanding()==1);
                app.PollChatInboundMessages();
                assert(app.dispatched.back()==42);
            }
        }
    }
    app.PollChatInboundMessages();
    assert(app.chat_inbound_messages_.Outstanding()==0);
    // Lesson presentation and real controls retain their original admission.
    for(bool lesson : {false,true}) {
        app.lesson_route=lesson;
        for(const char* type : {"mcp","robot_action","system","lesson_start","stt"}) {
            if(!lesson && CONFIG_TBOT_VOICE_DEMO && strcmp(type,"stt")==0)continue;
            auto root=Frame(73);
            cJSON_ReplaceItemInObject(root.get(),"type",cJSON_CreateString(type));
            callbacks.json({1,7},root.get(),88,{now_us,0});
            assert(app.chat_inbound_messages_.Outstanding()==1);
            app.PollChatInboundMessages();
            assert(app.dispatched.back()==73);
        }
    }
    app.lesson_route=false;
    // Real control pressure still fails at the original bounded deadline.
    for(int i=0;i<4;++i)Enqueue(app,i);
    auto control=Frame(99);
    callbacks.json({1,7},control.get(),88,{now_us,0});
    assert(waits>0);
    ChatProtocolSignals::Failure failure;
    assert(app.chat_protocol_signals_->ReadFailure(failure));
    assert(failure.flags & ChatProtocolSignals::Error);
}
'''
    generated = tmp_path / "demo.cc"
    generated.write_text(fixture)
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    obj = tmp_path / "cjson.o"
    subprocess.run([
        "cc", *flags, "-Wno-deprecated-declarations", "-I", str(CJSON),
        "-c", str(CJSON / "cJSON.c"), "-o", str(obj),
    ], check=True)
    binary = tmp_path / "demo"
    subprocess.run([
        "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
        *flags, f"-DCONFIG_TBOT_VOICE_DEMO={demo}", "-I", str(ROOT / "main"),
        "-I", str(CJSON), str(generated), str(obj), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_demo_still_starts_speech_and_delivers_drain_ack(tmp_path):
    def demo_journey(fixture, source, header):
        return "#define CONFIG_TBOT_VOICE_DEMO 1\n" + fixture.split("int main() {", 1)[0] + r'''
int main() {
    now_us=100;
    Application app;Setup(app);app.state=kDeviceStateListening;
    app.online_intent_=true;app.microphone_uplink_authorized_=true;
    receiver_wait=[&]{app.PollChatStart(now_us);};Start(app);receiver_wait={};
    app.PollChatStart(now_us);
    assert(app.state==kDeviceStateSpeaking);
    assert(app.speaking_arm_dispatch_.begins==1);
    auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
    callbacks.audio({1,7},std::make_unique<AudioStreamPacket>());
    assert(app.audio_service_.received_generations==std::vector<uint32_t>{2});
    auto* caption=cJSON_Parse("{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"caption\"}");
    for(int i=0;i<300;++i)callbacks.json({1,7},caption,0,{now_us,0});
    cJSON_Delete(caption);
    assert(app.chat_inbound_messages_.Outstanding()==0);
    Stop(app);
    // Retire Setup's previous outbound generation before sending this ACK.
    for(int i=0;i<4 && !app.chat_playout_ready_;++i) {
        app.PollChatPlayout(now_us);
        app.chat_outbound_worker_.RunOnce(now_us);
        app.PollChatPlayout(now_us);
    }
    assert(!app.chat_playout_recovery_);
    assert(app.chat_playout_ready_);
    assert(std::find(app.protocol_->sends.begin(),app.protocol_->sends.end(),int(Kind::DrainAck))!=app.protocol_->sends.end());
}
'''

    run_terminal_application(tmp_path, "address", 1, fixture_transform=demo_journey)
