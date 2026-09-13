"""Run the actual lesson START/STOP and polling methods at audio boundaries."""

from pathlib import Path
import json
import os
import subprocess

import pytest

from test_protocol_work_lifetime import method


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def playout_application(tmp_path_factory):
    tmp_path = tmp_path_factory.mktemp("lesson_playout_application")
    source = (ROOT / "main/application.cc").read_text()
    fixture = r'''
#include "lesson_audio_playout.h"
#include "chat_connection_messages.h"
#include <cJSON.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
int64_t now_ms=100;
int allocation_failure=0,allocation_calls=0,live_allocations=0;
void* TestMalloc(size_t size) {
    if(++allocation_calls==allocation_failure)return nullptr;
    auto* pointer=std::malloc(size);if(pointer)++live_allocations;return pointer;
}
void TestFree(void* pointer) { if(pointer){--live_allocations;std::free(pointer);} }
int64_t esp_timer_get_time() { return now_ms*1000; }
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking };
constexpr DeviceState kDeviceStateConnecting=static_cast<DeviceState>(3);
using ListeningMode=int;
constexpr int kListeningModeManualStop=0,kListeningModeRealtime=1,kListeningModeAutoStop=2;
namespace Lang { namespace Strings { const char* LISTENING="listen"; } namespace Sounds { const char* OGG_POPUP="popup"; } }
void xEventGroupSetBits(int,int) {}
constexpr int MAIN_EVENT_CHAT_OUTBOUND=1,MAIN_EVENT_SEND_AUDIO=2;
struct Display {
    void ClearChatMessages() {} void SetStatus(const char*) {} void SetChatMessage(const char*,const char*) {}
    void SetEmotion(const char*) {}
};
struct Context { bool current=true;std::string session_id="transport-session"; };
using ChatRequestContext = std::shared_ptr<Context>;
struct Application {
    struct Receipt { std::string id,state; uint64_t at_ms; };
    struct Protocol {
        std::vector<Receipt> receipts;
        std::vector<std::string> drains;
        bool send_ok=true;
        bool output_raw=false;
        std::string session_id_="transport-session";
        std::function<void()> send_hook;
        unsigned listen_starts=0;
        void SendStartListening(int) { ++listen_starts; }
        bool IsAudioChannelOpened() const {return true;}
        bool SendText(const std::string& text) {
            allocation_failure=0;
            auto* root=cJSON_Parse(text.c_str());assert(root);
            if(cJSON_GetObjectItem(root,"drainId")) {
                drains.push_back(cJSON_GetObjectItem(root,"drainId")->valuestring);
                cJSON_Delete(root);return send_ok;
            }
            receipts.push_back({cJSON_GetObjectItem(root,"playoutId")->valuestring,
                cJSON_GetObjectItem(root,"state")->valuestring,
                static_cast<uint64_t>(cJSON_GetObjectItem(root,"playoutAtMs")->valuedouble)});
            cJSON_Delete(root);
            if(output_raw)std::cout<<text<<std::endl;
            if(send_hook)send_hook();
            return send_ok;
        }
        static std::string EncodeLessonPlayoutAck(const std::string&,const char*,uint64_t,const std::string&);
        static std::string EncodeTtsDrainAck(const std::string&,const std::string&);
    } transport;
    Protocol* protocol_=&transport;
    ChatConnectionMessages chat_connection_messages_;
    uint64_t RequestChatConnectionText(const std::string& text,ChatRequestContext context,uint64_t,
        std::shared_ptr<std::atomic<bool>> authorization) {
        if(!context || !context->current)return 0;
        return chat_connection_messages_.Admit({{1,7},protocol_generation_.load(),1},text,now_ms*1000,authorization);
    }
    void FlushQueued() {
        while(auto* record=chat_connection_messages_.Front()) {
            if(record->owner.protocol_generation==protocol_generation_ &&
                (!record->authorization || record->authorization->load())) {
                if(!transport.SendText(*record->payload) && record->authorization)record->authorization->store(false);
            }
            chat_connection_messages_.Pop();
        }
    }
    struct Audio {
        PlaybackDrainSnapshot snapshot{7,1,false,0,0,false,false,{1,AudioOutputDrainState::Drained}};
        unsigned resets=0;
        bool epoch_available=true, snapshot_available=true, processing=true;
        std::function<void()> snapshot_hook;
        void SetPlaybackGeneration(uint32_t generation) { snapshot.playback_generation=generation; }
        void ResetDecoder() { ++resets; ++snapshot.reset_epoch; }
        bool TryGetPlaybackResetEpoch(uint64_t& epoch) { epoch=snapshot.reset_epoch;return epoch_available; }
        bool TryGetPlaybackDrainSnapshot(PlaybackDrainSnapshot& out) {
            out=snapshot;
            if(snapshot_hook)snapshot_hook();
            return snapshot_available;
        }
        void EnableVoiceProcessing(bool enabled) { processing=enabled; }
        bool IsAudioProcessorRunning() const {return processing;}
        bool WaitForPlaybackQueueEmpty(int) {assert(false);return false;}
        void EnableWakeWordDetection(bool) {}
        unsigned popups=0;
        void PlaySound(const char*) {++popups;}
        bool ArmChatUplink(uint32_t,bool) {return true;}
    } audio_service_;
    struct Arms { unsigned cancels=0;void Cancel(){++cancels;} } speaking_arm_dispatch_;
    struct Epoch { uint64_t epoch=7;uint64_t PublishedEpoch() const{return epoch;} } lesson_transport_epoch_gate_;
    std::atomic<uint64_t> protocol_generation_{1};
    std::atomic<uint32_t> speaking_generation_{1};
    std::atomic<bool> lesson_runtime_active_{true}, tts_audio_accepting_{false}, lesson_idle_repaint_suppressed_{false};
    std::atomic<int64_t> listening_started_ms_{0},last_listening_activity_ms_{0},last_speaking_activity_ms_{0};
    LessonAudioPlayout lesson_audio_playout_;
    std::mutex lesson_playout_mutex_;
    std::atomic<bool> lesson_playout_pending_{false};
    std::atomic<uint64_t> lesson_terminal_audio_generation_{0};
    static constexpr int kListeningModeRealtime=1;
    int listening_mode_=0;
    std::shared_ptr<std::atomic<bool>> lesson_playout_authorization_;
    bool lesson_playout_stop_sent_=false;
    bool QueueLessonPlayoutAck(const char*,uint64_t,bool drain=false);
    std::atomic<bool> lesson_interactive_listen_pending_{false},lesson_interactive_listening_active_{false};
    std::atomic<bool> microphone_uplink_authorized_{false},lesson_asset_sync_quiet_{false};
    std::atomic<uint32_t> connect_generation_{1};
    struct Signals {
        enum {Error=1};
        bool SourceSelected() const {return true;}
        bool TrySource(ConnectionSource& source) {source={1,7};return true;}
        void PublishConnectionFault(ConnectionSource,uint32_t,int) {assert(false);}
    } signals;
    Signals* chat_protocol_signals_=&signals;
    using ChatProtocolSignals=Signals;
    ChatConnectionMessages::Owner chat_lesson_capture_owner_;
    uint64_t chat_lesson_capture_epoch_=0,chat_lesson_capture_deadline_us_=0;
    uint32_t chat_lesson_capture_token_=0,chat_audio_prepared_=0;
    struct {uint32_t revoked=0;} chat_audio_desired_;
    bool chat_audio_fault_=false,play_popup_on_listening_=false;
    int event_group_=0;
    static constexpr int kListenPlaybackDrainTimeoutMs=1000;
    enum class ChatWakePolicy {Listening};
    uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool,bool,bool,ChatWakePolicy) {
        return ++chat_audio_desired_.revoked;
    }
    bool IsLessonVoiceRoute() const {return lesson_runtime_active_;}
    bool IsChatConnectionCurrent(ConnectionSource source,uint64_t protocol,uint32_t connect) const {
        return source.source_id==1 && source.connection_epoch==7 && protocol==protocol_generation_ && connect==connect_generation_;
    }
    void Schedule(std::function<void()>) {assert(false);}
    void ContinueOpenAudioChannel(ListeningMode) {assert(false);}
    bool RequestChatLessonCapture();
    void PollChatLessonCapture(uint64_t);
    void ApplyListeningState();
    ChatRequestContext lesson_playout_context_;
    std::string lesson_playout_id_,lesson_playout_drain_id_;
    uint32_t lesson_playout_generation_=0;
    uint64_t lesson_playout_protocol_generation_=0,lesson_playout_epoch_=0;
    int64_t lesson_playout_stop_ms_=0;
    uint64_t lesson_playout_drained_at_ms_=0;
    bool lesson_playout_start_sent_=false,aborted_=false;
    static constexpr int64_t kSpeakingTimeoutMs=60000,kTtsStopPlaybackDrainTimeoutMs=5000;
    DeviceState state=kDeviceStateIdle;
    unsigned timeout_arms=0;
    unsigned source_faults=0;
    void FailChatRequest(const ChatRequestContext&) { ++source_faults; }
    std::vector<DeviceState> states;
    DeviceState GetDeviceState() const { return state; }
    void SetDeviceState(DeviceState next) { state=next;states.push_back(next); }
    void ArmSpeakingTimeout() { ++timeout_arms; }
    bool IsChatLessonRequestCurrent(const ChatRequestContext& context) const { return !context || context->current; }
    bool HandleLessonPlayoutTts(const cJSON*,ChatRequestContext);
    void PollLessonAudioPlayout();
};
'''
    protocol = (ROOT / "main/protocols/protocol.cc").read_text()
    for name in ("EncodeLessonPlayoutAck", "EncodeTtsDrainAck"):
        fixture += method(protocol, "std::string Protocol::" + name).replace(
            "Protocol::" + name, "Application::Protocol::" + name)
    fixture += method(source, "bool Application::QueueLessonPlayoutAck")
    fixture += method(source, "bool Application::HandleLessonPlayoutTts")
    fixture += method(source, "void Application::PollLessonAudioPlayout")
    fixture += method(source, "bool Application::RequestChatLessonCapture")
    fixture += method(source, "void Application::PollChatLessonCapture")
    state_handler = method(source, "void Application::HandleStateChangedEvent")
    listening_branch = method(state_handler, "case kDeviceStateListening:")
    fixture += "void Application::ApplyListeningState(){ Display display_storage; auto* display=&display_storage;switch(state){" + listening_branch + "default:assert(false);}}"
    fixture += r'''
const std::string a="0123456789abcdef0123456789abcdef",b="1123456789abcdef0123456789abcdef";
void Wire(Application& app,const char* state,const std::string& id=a,const char* reason=nullptr,ChatRequestContext context={}) {
    auto* root=cJSON_CreateObject();
    cJSON_AddStringToObject(root,"type","tts");cJSON_AddStringToObject(root,"state",state);
    cJSON_AddStringToObject(root,"playoutId",id.c_str());cJSON_AddStringToObject(root,"drainId","drain:one");
    if(reason)cJSON_AddStringToObject(root,"reason",reason);
    assert(app.HandleLessonPlayoutTts(root,context ? context : std::make_shared<Context>()));cJSON_Delete(root);
}
void Poll(Application& app) {
    app.PollLessonAudioPlayout();
    app.FlushQueued();
    app.PollLessonAudioPlayout();
    app.FlushQueued();
}
void Output(Application& app,uint64_t at_ms=150) {
    app.lesson_audio_playout_.PublishOutput(app.speaking_generation_,true,at_ms);
    Poll(app);
}
void AssertQuiet(Application& app) {
    assert(app.state!=kDeviceStateSpeaking);
    assert(!app.tts_audio_accepting_ && app.lesson_playout_id_.empty());
}
int main(int argc,char** argv) {
    assert(argc>=2);std::string test=argv[1];Application app;
    if(test=="canonical") {
        assert(argc==3);std::ifstream input(argv[2]);assert(input);
        std::string raw((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        auto* root=cJSON_Parse(raw.c_str());assert(root);app.transport.output_raw=true;
        auto* start=cJSON_GetObjectItem(root,"start");auto* stop=cJSON_GetObjectItem(root,"stop");
        assert(app.HandleLessonPlayoutTts(start,std::make_shared<Context>()));Poll(app);
        assert(app.transport.receipts.empty());Output(app,100);
        now_ms=120;assert(app.HandleLessonPlayoutTts(stop,std::make_shared<Context>()));Poll(app);
        assert(app.transport.receipts.size()==2 && app.transport.drains==std::vector<std::string>{"lesson-1"});
        cJSON_Delete(root);return 0;
    }
    if(test=="queued_output_gap_drain") {
        Wire(app,"start");app.audio_service_.snapshot.playback_queue_size=3;
        Poll(app);assert(app.state==kDeviceStateIdle && app.transport.receipts.empty());
        app.lesson_audio_playout_.PublishOutput(app.speaking_generation_,false,120);
        Poll(app);assert(app.transport.receipts.empty());
        now_ms=170;Output(app,150);
        assert(app.state==kDeviceStateSpeaking && app.transport.receipts.size()==1);
        assert(app.transport.receipts[0].id==a && app.transport.receipts[0].state=="start" && app.transport.receipts[0].at_ms==150);
        app.audio_service_.snapshot.playback_queue_size=0;
        now_ms=12000;Poll(app);assert(app.state==kDeviceStateSpeaking);
        Wire(app,"stop");auto original_stop=app.lesson_playout_stop_ms_;
        ++now_ms;Wire(app,"stop");assert(app.lesson_playout_stop_ms_==original_stop);
        auto& d=app.audio_service_.snapshot;
        for(int i=0;i<8;++i) {
            auto saved=d;
            if(i==0)d.decode_queue_size=1;if(i==1)d.playback_queue_size=1;
            if(i==2)d.decode_in_flight=true;if(i==3)d.output_in_flight=true;
            if(i==4)d.codec.state=AudioOutputDrainState::Pending;
            if(i==5)d.codec.state=AudioOutputDrainState::Failed;
            if(i==6)++d.reset_epoch;if(i==7)d.stopped=true;
            Poll(app);assert(app.state==kDeviceStateSpeaking && app.transport.receipts.size()==1);d=saved;
        }
        now_ms+=10;Poll(app);AssertQuiet(app);
        assert(app.transport.receipts.size()==2 && app.transport.receipts[1].state=="stop");
        assert(app.transport.receipts[1].at_ms==static_cast<uint64_t>(now_ms) && app.transport.drains.size()==1);
        Poll(app);Wire(app,"stop");assert(app.transport.receipts.size()==2);
    } else if(test=="short_audio") {
        Wire(app,"start");Wire(app,"stop");Output(app,101);
        AssertQuiet(app);assert(app.transport.receipts.size()==2 && app.transport.drains.size()==1);
    } else if(test=="replacement") {
        Wire(app,"start");Output(app);assert(app.state==kDeviceStateSpeaking);
        const auto old_generation=app.speaking_generation_.load();
        Wire(app,"start",b);assert(app.state!=kDeviceStateSpeaking);
        app.lesson_audio_playout_.PublishOutput(old_generation,true,160);Poll(app);
        assert(app.transport.receipts.size()==1);
        Wire(app,"stop",a);assert(app.tts_audio_accepting_ && app.lesson_playout_id_==b);
        Output(app,180);assert(app.transport.receipts.size()==2 && app.transport.receipts.back().id==b);
        auto generation=app.speaking_generation_.load();Wire(app,"start",a);Wire(app,"start",b);
        assert(app.speaking_generation_==generation && app.transport.receipts.size()==2);
    } else if(test=="interrupt") {
        Wire(app,"start");Output(app);Wire(app,"stop",a,"interrupt");AssertQuiet(app);
        Poll(app);assert(app.transport.receipts.size()==1 && app.transport.drains.empty());
    } else if(test=="context" || test=="epoch" || test=="protocol" || test=="generation") {
        auto context=std::make_shared<Context>();Wire(app,"start",a,nullptr,context);Output(app);
        if(test=="context")context->current=false;
        if(test=="epoch")++app.lesson_transport_epoch_gate_.epoch;
        if(test=="protocol")++app.protocol_generation_;
        if(test=="generation") {++app.speaking_generation_;app.state=kDeviceStateListening;}
        Poll(app);assert(app.lesson_playout_id_.empty());
        if(test!="generation")AssertQuiet(app);else assert(app.state==kDeviceStateListening);
        assert(app.transport.receipts.size()==1 && app.transport.drains.empty());
    } else if(test=="drain_boundary_reconnect") {
        Wire(app,"start");Output(app);Wire(app,"stop");
        app.audio_service_.snapshot_hook=[&]{++app.protocol_generation_;};
        Poll(app);assert(app.transport.receipts.size()==1 && app.transport.drains.empty());
        AssertQuiet(app);
    } else if(test=="send_failure") {
        Wire(app,"start");app.transport.send_ok=false;Output(app);AssertQuiet(app);
        assert(app.audio_service_.resets==2 && app.transport.drains.empty());
    } else if(test=="invalid_admission") {
        auto stale=std::make_shared<Context>();stale->current=false;
        Wire(app,"start",a,nullptr,stale);Wire(app,"start","invalid");
        app.lesson_runtime_active_=false;Wire(app,"start");
        assert(app.audio_service_.resets==0 && app.lesson_playout_id_.empty() && !app.tts_audio_accepting_);
    } else if(test=="reset_unavailable") {
        app.audio_service_.epoch_available=false;Wire(app,"start");
        assert(!app.tts_audio_accepting_ && app.lesson_playout_id_.empty());
        app.lesson_audio_playout_.PublishOutput(app.speaking_generation_,true,120);
        Poll(app);assert(app.transport.receipts.empty());
    } else if(test=="replacement_reset_unavailable") {
        Wire(app,"start");Output(app);app.audio_service_.epoch_available=false;
        Wire(app,"start",b);Poll(app);AssertQuiet(app);
        assert(app.transport.receipts.size()==1);
    } else if(test=="start_send_reconnect") {
        Wire(app,"start");app.transport.send_hook=[&]{++app.protocol_generation_;};
        Output(app);AssertQuiet(app);
        assert(app.timeout_arms==1 && app.transport.drains.empty());
    } else if(test=="stop_send_reconnect") {
        Wire(app,"start");Output(app);Wire(app,"stop");
        app.transport.send_hook=[&]{++app.protocol_generation_;};
        Poll(app);AssertQuiet(app);assert(app.transport.drains.empty());
    } else if(test=="untagged_active") {
        Wire(app,"start");const auto generation=app.speaking_generation_.load();
        for(const char* state:{"start","stop"}) {
            auto* root=cJSON_CreateObject();cJSON_AddStringToObject(root,"state",state);
            assert(app.HandleLessonPlayoutTts(root,{}));cJSON_Delete(root);
        }
        assert(app.speaking_generation_==generation && app.tts_audio_accepting_);
    } else if(test=="old_start_after_history_capacity") {
        Wire(app,"start");
        for(unsigned response=1;response<=40;++response) {
            char id[33];std::snprintf(id,sizeof(id),"0123456789abcdef%016llx",0x0123456789abcdefULL+response);
            const auto previous_generation=app.speaking_generation_.load();
            Wire(app,"start",id);
            assert(app.speaking_generation_==previous_generation+1 && app.lesson_playout_id_==id);
        }
        const auto generation=app.speaking_generation_.load();
        const auto current_id=app.lesson_playout_id_;
        Wire(app,"start",a);
        assert(app.speaking_generation_==generation && app.lesson_playout_id_==current_id);
    } else if(test=="serialization_allocation_failures") {
        cJSON_Hooks hooks{TestMalloc,TestFree};cJSON_InitHooks(&hooks);
        unsigned failed=0,passed=0;
        for(int failure=1;failure<128;++failure) {
            allocation_calls=0;allocation_failure=failure;app.transport.receipts.clear();
            const auto encoded=Application::Protocol::EncodeLessonPlayoutAck(a,"start",100,"transport-session");
            const bool sent=!encoded.empty();
            assert(live_allocations==0);
            if(sent){++passed;assert(app.transport.receipts.empty());break;}
            ++failed;assert(app.transport.receipts.empty());
        }
        assert(failed>=10 && passed==1);cJSON_InitHooks(nullptr);
    } else if(test=="silent_stop") {
        Wire(app,"start");Wire(app,"stop");Poll(app);
        assert(app.transport.receipts.empty() && app.state==kDeviceStateIdle);
        now_ms+=Application::kTtsStopPlaybackDrainTimeoutMs;Poll(app);
        AssertQuiet(app);assert(app.transport.receipts.empty() && app.transport.drains.empty());
    } else if(test=="stop_send_failure") {
        Wire(app,"start");Output(app);Wire(app,"stop");app.transport.send_ok=false;
        Poll(app);AssertQuiet(app);
        assert(app.transport.drains.empty() && !app.tts_audio_accepting_);
    } else if(test=="bounded_queue_drain_timestamp") {
        Wire(app,"start");Output(app,100);
        assert(app.chat_connection_messages_.Admit({{1,7},1,1},"unused",100));
        assert(app.chat_connection_messages_.Admit({{1,7},1,1},"unused",100));
        now_ms=120;Wire(app,"stop");app.PollLessonAudioPlayout();
        assert(app.state==kDeviceStateIdle && app.transport.receipts.size()==1);
        app.chat_connection_messages_.Pop();app.chat_connection_messages_.Pop();
        now_ms=220;app.PollLessonAudioPlayout();
        assert(app.chat_connection_messages_.Size()==2 && app.transport.receipts.size()==1);
        app.FlushQueued();assert(app.transport.receipts.size()==2 && app.transport.drains.size()==1);
        assert(app.transport.receipts.back().at_ms==120);
    } else if(test=="realtime_capture") {
        app.listening_mode_=Application::kListeningModeRealtime;app.state=kDeviceStateListening;
        app.listening_started_ms_=40;app.last_listening_activity_ms_=50;
        Wire(app,"start");assert(app.audio_service_.processing);
        assert(app.listening_started_ms_==40 && app.last_listening_activity_ms_==50);
        Output(app);Wire(app,"start",b);assert(app.audio_service_.processing);
        assert(app.state==kDeviceStateSpeaking);
    } else if(test=="wide_timestamp") {
        now_ms=0x100000005LL;Wire(app,"start");Output(app,static_cast<uint64_t>(now_ms));
        assert(app.transport.receipts.front().at_ms==0x100000005ULL);
        now_ms+=20;Wire(app,"stop");Poll(app);
        assert(app.transport.receipts.back().at_ms==0x100000019ULL);
    } else if(test=="poll_lock_contention") {
        Wire(app,"start");
        std::unique_lock<std::mutex> lock(app.lesson_playout_mutex_);
        auto polling=std::async(std::launch::async,[&]{app.PollLessonAudioPlayout();});
        assert(polling.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        polling.get();assert(app.transport.receipts.empty());
        lock.unlock();Output(app);assert(app.transport.receipts.size()==1);
    } else if(test=="failed_codec") {
        Wire(app,"start");Wire(app,"stop");
        app.audio_service_.snapshot.codec.state=AudioOutputDrainState::Failed;
        app.lesson_audio_playout_.PublishFailure(app.speaking_generation_);
        app.PollLessonAudioPlayout();AssertQuiet(app);
        assert(app.source_faults==1 && app.transport.receipts.empty() && app.transport.drains.empty());
    } else if(test=="terminal_rejects_start") {
        app.lesson_terminal_audio_generation_=2;
        Wire(app,"start");assert(app.lesson_playout_id_.empty() && !app.tts_audio_accepting_);
        assert(app.speaking_generation_==1 && app.transport.receipts.empty());
    } else if(test=="interactive_manual" || test=="interactive_realtime" || test=="interactive_active") {
        app.listening_mode_=test=="interactive_realtime" ? kListeningModeRealtime : kListeningModeManualStop;
        const bool pending=test!="interactive_active";
        app.lesson_interactive_listen_pending_=pending;
        app.lesson_interactive_listening_active_=!pending;
        Wire(app,"start");Output(app);Wire(app,"stop");Poll(app);
        assert(app.state==kDeviceStateListening && app.transport.receipts.size()==2 && app.transport.drains.size()==1);
        app.ApplyListeningState();
        assert(!app.lesson_interactive_listen_pending_ && app.lesson_interactive_listening_active_);
        assert(app.transport.listen_starts==1 && app.audio_service_.popups==(pending ? 1u : 0u));
        assert(!app.microphone_uplink_authorized_ && app.chat_lesson_capture_token_);
        app.PollChatLessonCapture(now_ms*1000);assert(!app.microphone_uplink_authorized_);
        app.chat_audio_prepared_=app.chat_lesson_capture_token_;
        app.PollChatLessonCapture(now_ms*1000);assert(app.microphone_uplink_authorized_);
        assert(app.chat_lesson_capture_token_==0);
        Wire(app,"stop");Poll(app);assert(app.transport.listen_starts==1);
    } else if(test=="interactive_autostop" || test=="passive_realtime") {
        app.listening_mode_=test=="interactive_autostop" ? kListeningModeAutoStop : kListeningModeRealtime;
        app.lesson_interactive_listen_pending_=test=="interactive_autostop";
        Wire(app,"start");Output(app);Wire(app,"stop");Poll(app);
        AssertQuiet(app);assert(app.transport.listen_starts==0 && !app.chat_lesson_capture_token_);
    } else if(test=="timeout") {
        Wire(app,"start");Wire(app,"stop");now_ms+=Application::kTtsStopPlaybackDrainTimeoutMs;
        Poll(app);AssertQuiet(app);assert(app.transport.receipts.empty());
    } else assert(false);
}
'''
    generated = tmp_path / "application.cc"
    generated.write_text(fixture)
    cjson = Path.home() / "esp/esp-idf/components/json/cJSON"
    obj = tmp_path / "cjson.o"
    flags = ["-fsanitize=address,undefined"]
    subprocess.run(["cc", *flags, "-Wno-deprecated-declarations", "-I", str(cjson),
                    "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
    binary = tmp_path / "playout"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "main"), "-I", str(cjson), str(generated), str(obj),
                    "-o", str(binary)], check=True)
    return binary


@pytest.mark.parametrize("case", [
    "queued_output_gap_drain", "short_audio", "replacement", "interrupt",
    "context", "epoch", "protocol", "generation", "drain_boundary_reconnect",
    "send_failure", "timeout", "invalid_admission", "reset_unavailable",
    "silent_stop", "stop_send_failure",
    "replacement_reset_unavailable", "start_send_reconnect", "stop_send_reconnect",
    "untagged_active",
    "old_start_after_history_capacity",
    "serialization_allocation_failures",
    "bounded_queue_drain_timestamp", "realtime_capture", "wide_timestamp",
    "poll_lock_contention", "failed_codec",
    "terminal_rejects_start",
    "interactive_manual", "interactive_realtime", "interactive_active", "interactive_autostop", "passive_realtime",
])
def test_actual_lesson_playout_application(playout_application, case):
    subprocess.run([str(playout_application), case], check=True, timeout=15)


def test_canonical_esp_fixture_roundtrips_actual_firmware_receipts(playout_application):
    esp = Path(os.environ.get("TBOT_ESP_WORKTREE", str(ROOT.parent / "esp-cpr20260911")))
    fixture = esp / "main/tbot-server/tests/fixtures/course-mode/speech-playout.v1.json"
    assert fixture.is_file(), f"Canonical ESP playout fixture is required: {fixture}"
    expected = json.loads(fixture.read_text())
    result = subprocess.run([str(playout_application), "canonical", str(fixture)],
                            check=True, text=True, capture_output=True, timeout=15)
    assert [json.loads(line) for line in result.stdout.splitlines()] == [
        expected["started"], expected["drained"],
    ]
