#include <atomic>
#include <mutex>
#include <functional>
#define private public
#include "chat_protocol_signals.h"
#undef private
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <future>
std::function<void()> source_publish_hook;
#include "chat_inbound_messages.h"
uint64_t esp_timer_get_time() { return 100; }
void vTaskDelay(int) { assert(false && "unexpected JSON admission wait in ownership fixture"); }
struct AudioStreamPacket {};

constexpr int MAIN_EVENT_ERROR=2, MAIN_EVENT_CHAT_OUTBOUND=8192;
constexpr int kDeviceStateWifiConfiguring=3, kDeviceStateAudioTesting=4, kDeviceStateIdle=0, kDeviceStateConnecting=5;
#define ESP_LOGI(...) ((void)0)
template<class... Args> void TestLog(Args...) {}
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
constexpr const char* TAG="test";
namespace Lang {
namespace Strings { const char* PLEASE_WAIT="wait"; const char* SERVER_UNAVAILABLE_RETRYING="retry"; }
namespace Sounds { const char* OGG_EXCLAMATION="sound"; }
}
enum class PowerSaveLevel { LOW_POWER };
struct Board {
    static Board& GetInstance() { static Board b; return b; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
    Board* GetDisplay() { return this; }
    void SetChatMessage(const char*, const char*) {}
    void SetStatus(const char*) {}
    void SetEmotion(const char*) {}
};
bool PassiveReconnectHasOwner(bool reconnect, bool connect) { return reconnect || connect; }
static unsigned errors=0;
void xEventGroupSetBits(int, int bits) { if (bits==MAIN_EVENT_ERROR) ++errors; }
struct Protocol {
    std::string session_id() const { return "session"; }
    int server_sample_rate() const { return 24000; }
    struct SourceCallbacks {
        std::function<void(ConnectionSource,const std::string&)> error;
        std::function<void(ConnectionSource)> closed;
        std::function<void(ConnectionSource,uint64_t)> opened;
        std::function<bool(ConnectionSource)> adopted;
        std::function<void(ConnectionSource,const cJSON*,uint64_t,ConnectionReceipt)> json;
        std::function<void(ConnectionSource,std::unique_ptr<AudioStreamPacket>)> audio;
    };
    std::function<void(const std::string&)> error;
    std::function<void()> closed;
    void OnNetworkError(std::function<void(const std::string&)> value) { error=std::move(value); }
    void OnAudioChannelClosed(std::function<void()> value) { closed=std::move(value); }
};
bool ProtocolLifetimeMatches(Protocol* a, Protocol* b, uint64_t x, uint64_t y) { return a==b && x==y; }
struct Application {
    ChatInboundMessages chat_inbound_messages_;
    bool IsChatConnectionCurrent(ConnectionSource source,uint64_t protocol,uint32_t connect) const {
        return chat_protocol_signals_ && chat_protocol_signals_->MatchesSource(source) &&
            protocol==protocol_generation_.load() && connect==connect_generation_.load() && !chat_protocol_owned_.load();
    }
    std::unique_ptr<Protocol> protocol_{new Protocol};
    std::atomic<uint64_t> protocol_generation_{1};
    std::atomic<uint32_t> connect_generation_{1},chat_source_connect_generation_{0};
    std::atomic<bool> chat_protocol_owned_{false};
    std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
    bool chat_protocol_fault_=false, keep_heartbeat=true;
    uint64_t chat_protocol_fault_generation_=0;
    uint32_t chat_protocol_fault_era_=0;
    std::atomic<bool> backend_offline_{false};
    std::atomic<bool> tts_audio_accepting_{true}, lesson_runtime_active_{false}, passive_ws_intent_{false};
    std::atomic<bool> lesson_asset_sync_quiet_{false};
    std::atomic<bool> reconnect_passive_{false}, connect_in_flight_{false}, online_intent_{false};
    std::atomic<bool> lesson_interactive_listen_pending_{false}, lesson_interactive_listening_active_{false};
    std::atomic<unsigned> lesson_interactive_listen_generation_{0};
    struct Arms { unsigned cancels=0; void Cancel() { ++cancels; } } speaking_arm_dispatch_;
    struct Audio {
        unsigned resets=0, pops=0;
        void* PopPacketFromSendQueue() { ++pops; return nullptr; }
        void ResetDecoder() { ++resets; }
        void PlaySound(const char*) {}
    } audio_service_;
    int state=1;
    unsigned starts=0, stops=0, heartbeats=0;
    std::string last_error_message_;
    std::deque<std::function<void()>> tasks;
    bool schedule_failure=false;
    int event_group_=0;
    void Schedule(std::function<void()>&& work) { if (schedule_failure) throw 1; tasks.push_back(std::move(work)); }
    void RunTasks() { while (!tasks.empty()) { auto work=std::move(tasks.front()); tasks.pop_front(); work(); } }
    bool ShouldKeepManagementHeartbeat() { return keep_heartbeat; }
    void StartHeartbeat() { ++starts; }
    void StopHeartbeat() { ++stops; }
    void DispatchDeviceHeartbeat() { ++heartbeats; }
    void RequestLessonStorageAbandonment() {}
    int GetDeviceState() { return state; }
    void SetDeviceState(int value) { state=value; }
    void SchedulePassiveLessonReconnect() {}
    int GetDefaultListeningMode() { return 0; }
    void ScheduleReconnect(int, bool) {}
    void PollChatProtocolSignals();
    Protocol::SourceCallbacks MakeChatSourceCallbacks(uint64_t,std::shared_ptr<ChatProtocolSignals>);
    bool SelectChatProtocolSource(ConnectionSource,uint64_t,uint32_t);
    void HandleChatTerminalStop(const std::shared_ptr<ChatProtocolSignals>&,uint64_t,ConnectionSource,const cJSON*,uint64_t=0) {}
    unsigned chat_starts=0,lesson_json=0,lesson_audio=0;
    bool lesson_voice=false;uint64_t lesson_epoch=99;
    bool IsLessonVoiceRoute() const { return lesson_voice; }
    bool IsChatLessonRequestCurrent(const ChatRequestContext& context) const { return IsChatConnectionCurrent(context->owner.source,context->owner.protocol_generation,context->owner.connect_generation) && context->lesson_epoch==lesson_epoch; }
    void DispatchIncomingJson(const cJSON*,uint64_t epoch,bool,ChatRequestContext context) { assert(context && epoch==99);++lesson_json; }
    void HandleChatLessonAudio(const std::shared_ptr<ChatProtocolSignals>&,uint64_t,ConnectionSource,std::unique_ptr<AudioStreamPacket>) { ++lesson_audio; }
    void HandleChatStart(const std::shared_ptr<ChatProtocolSignals>&,uint64_t,ConnectionSource,const cJSON*,ConnectionReceipt={}) { ++chat_starts; }
    void HandleChatAudio(const std::shared_ptr<ChatProtocolSignals>&,uint64_t,ConnectionSource,std::unique_ptr<AudioStreamPacket>) {}
    uint32_t chat_start_handled_serial_=0,chat_start_failed_serial_=0,chat_start_effects_serial_=0;
    bool chat_cleanup_enabled_=false;
    uint32_t chat_source_failure_handled_=0;
    void HandleChatSourceFailure() {}
    void FailChatRequest(const ChatRequestContext&) { assert(false); }
    std::atomic<uint32_t> protocol_callback_connect_generation_{1};
    void Install() {
        const auto callback_protocol_generation=protocol_generation_.load();
        auto* callback_protocol=protocol_.get();
        const auto callback_signals=chat_protocol_signals_;
        // PRODUCTION_ERROR_CALLBACK
        // PRODUCTION_CLOSED_CALLBACK
    }
};
// PRODUCTION_METHODS
int main() {
    {
        Application opening;auto callbacks=opening.MakeChatSourceCallbacks(1,opening.chat_protocol_signals_);
        assert(callbacks.opened && callbacks.adopted);
        callbacks.opened({1,5},250100);ChatProtocolSignals::Opened published;
        assert(opening.chat_protocol_signals_->ReadOpened(published));
        assert(published.source.source_id==1 && published.connect_generation==1 && published.sample_rate==24000);
        assert(!callbacks.adopted({1,5}));
        assert(opening.SelectChatProtocolSource({1,5},1,1));assert(callbacks.adopted({1,5}));
        opening.connect_in_flight_=true;assert(!callbacks.adopted({1,5}));
        opening.connect_in_flight_=false;opening.state=kDeviceStateConnecting;assert(!callbacks.adopted({1,5}));
        opening.passive_ws_intent_=true;assert(callbacks.adopted({1,5}));opening.passive_ws_intent_=false;
        opening.state=kDeviceStateIdle;assert(callbacks.adopted({1,5}));
        callbacks.closed({1,5});assert(!callbacks.adopted({1,5}));
    }
    {
        Application lesson;assert(lesson.SelectChatProtocolSource({1,5},1,1));lesson.lesson_voice=true;
        auto callbacks=lesson.MakeChatSourceCallbacks(1,lesson.chat_protocol_signals_);
        auto* frame=cJSON_Parse("{\"type\":\"tts\",\"state\":\"start\"}");
        callbacks.json({1,5},frame,99,{100,0});cJSON_Delete(frame);
        assert(lesson.lesson_json==1 && lesson.chat_starts==0);
        callbacks.audio({1,5},std::make_unique<AudioStreamPacket>());assert(lesson.lesson_audio==1);
        frame=cJSON_Parse("{\"type\":\"tts\",\"state\":\"stop\"}");
        callbacks.json({1,5},frame,98,{100,0});assert(lesson.lesson_json==1);
        callbacks.json({1,5},frame,99,{100,0});assert(lesson.lesson_json==2);cJSON_Delete(frame);
    }
    Application source_app;
    auto callbacks=source_app.MakeChatSourceCallbacks(1,source_app.chat_protocol_signals_);
    const ConnectionSource first_source{1,5},second_source{2,9};
    assert(!source_app.SelectChatProtocolSource(first_source,2,1));
    assert(source_app.SelectChatProtocolSource(first_source,1,1));
    callbacks.error(first_source,"current"); source_app.PollChatProtocolSignals();
    assert(source_app.chat_protocol_fault_); source_app.chat_protocol_fault_=false;
    source_app.chat_protocol_signals_->Disable();
    assert(source_app.SelectChatProtocolSource(second_source,1,1));
    callbacks.closed(first_source); source_app.PollChatProtocolSignals();
    assert(!source_app.chat_protocol_fault_);
    callbacks.closed(second_source);
    ++source_app.connect_generation_;
    source_app.PollChatProtocolSignals(); assert(!source_app.chat_protocol_fault_);

    Application blocked_source;
    auto blocked_callbacks=blocked_source.MakeChatSourceCallbacks(1,blocked_source.chat_protocol_signals_);
    assert(blocked_source.SelectChatProtocolSource(first_source,1,1));
    std::unique_lock<std::mutex> source_mutex(blocked_source.chat_protocol_signals_->mutex_);
    std::promise<void> publishing;
    source_publish_hook=[&]{publishing.set_value();};
    auto blocked=std::async(std::launch::async,[&]{blocked_callbacks.closed(first_source);});
    publishing.get_future().wait(); ++blocked_source.connect_generation_;
    source_mutex.unlock(); blocked.get(); source_publish_hook={};
    blocked_source.PollChatProtocolSignals(); assert(!blocked_source.chat_protocol_fault_);
    callbacks.closed(second_source); source_app.PollChatProtocolSignals();
    assert(!source_app.chat_protocol_fault_);
    source_app.chat_protocol_owned_=true;
    source_app.protocol_.reset(); callbacks.error(second_source,"moved");
    source_app.PollChatProtocolSignals(); assert(!source_app.chat_protocol_fault_);

    Application app;
    app.Install();
    app.protocol_->error("network down");
    assert(!app.backend_offline_ && app.last_error_message_.empty());
    app.RunTasks();
    assert(app.backend_offline_ && app.last_error_message_=="network down");
    assert(app.starts==1 && app.heartbeats==1 && errors==1);
    app.backend_offline_=false;
    app.protocol_->error("old queued error");
    app.chat_protocol_signals_->SelectDeferred();
    app.chat_protocol_signals_->Disable();
    app.chat_protocol_owned_=true;
    auto held=std::move(app.protocol_);
    held->error("cleanup callback");
    app.protocol_=std::move(held);
    app.chat_protocol_owned_=false;
    app.RunTasks();
    app.PollChatProtocolSignals();
    assert(!app.backend_offline_ && !app.chat_protocol_fault_);
    assert(app.last_error_message_=="network down" && errors==1);
    app.chat_protocol_signals_->Enable();
    app.protocol_->error("current deferred error");
    assert(app.tasks.empty()); app.PollChatProtocolSignals();
    assert(app.chat_protocol_fault_ && !app.backend_offline_);

    Application stale;
    stale.Install();
    auto old_protocol=std::move(stale.protocol_);
    stale.chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
    ++stale.protocol_generation_;
    stale.protocol_=std::make_unique<Protocol>();
    stale.Install();
    old_protocol->error("old source"); stale.RunTasks();
    assert(!stale.backend_offline_);
    stale.schedule_failure=true;
    stale.protocol_->error("allocation failure");
    stale.PollChatProtocolSignals();
    assert(stale.chat_protocol_fault_);

    Application closed;
    closed.Install();
    closed.protocol_->closed();
    assert(closed.speaking_arm_dispatch_.cancels==0);
    closed.chat_protocol_signals_->SelectDeferred();
    closed.chat_protocol_signals_->Disable();
    auto moved=std::move(closed.protocol_);
    moved->closed();
    closed.protocol_=std::move(moved);
    closed.RunTasks(); closed.PollChatProtocolSignals();
    assert(closed.speaking_arm_dispatch_.cancels==0 && closed.state==1);
    assert(closed.audio_service_.pops==0 && !closed.chat_protocol_fault_);
    closed.chat_protocol_signals_->Enable();
    closed.protocol_->closed(); closed.PollChatProtocolSignals();
    assert(closed.chat_protocol_fault_ && closed.tasks.empty());

    Application legacy_closed;
    legacy_closed.Install(); legacy_closed.protocol_->closed();
    assert(legacy_closed.tts_audio_accepting_ && legacy_closed.state==1);
    assert(legacy_closed.speaking_arm_dispatch_.cancels==0);
    legacy_closed.RunTasks();
    assert(legacy_closed.speaking_arm_dispatch_.cancels==1 && legacy_closed.state==kDeviceStateIdle);
    assert(legacy_closed.audio_service_.pops==1 && !legacy_closed.tts_audio_accepting_);
}
