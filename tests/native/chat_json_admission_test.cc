#include "chat_inbound_messages.h"
#include "chat_protocol_signals.h"
#include "protocols/connection_inbound_gate.h"
#include <cassert>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <thread>
#include <vector>

char last_log[256]{};
std::mutex log_mutex;
void Log(const char*,const char* format,...) {
    std::lock_guard<std::mutex> lock(log_mutex);
    va_list arguments;va_start(arguments,format);
    vsnprintf(last_log,sizeof(last_log),format,arguments);va_end(arguments);
}
#define ESP_LOGW(...) Log(__VA_ARGS__)
constexpr const char* TAG = "test";
constexpr int MAIN_EVENT_CHAT_OUTBOUND = 1, kDeviceStateConnecting = 1;
std::atomic<uint64_t> now_us{100};
uint64_t esp_timer_get_time() { return now_us.load(); }
void xEventGroupSetBits(std::atomic<unsigned>* events, int bits) { events->fetch_or(bits); }
std::function<void()> receiver_wait;
bool forbid_new=false;
void* operator new(size_t size) {
    if(forbid_new)throw std::bad_alloc();
    if(auto* pointer=std::malloc(size ? size : 1))return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void vTaskDelay(int ticks) { assert(ticks == 1); if (receiver_wait) receiver_wait(); else ++now_us; }
struct AudioStreamPacket {};
struct Protocol {
    struct SourceCallbacks {
        std::function<void(ConnectionSource,uint64_t)> opened;
        std::function<bool(ConnectionSource)> adopted;
        std::function<void(ConnectionSource,const std::string&)> error;
        std::function<void(ConnectionSource)> closed;
        std::function<void(ConnectionSource,const cJSON*,uint64_t,ConnectionReceipt)> json;
        std::function<void(ConnectionSource,std::unique_ptr<AudioStreamPacket>)> audio;
    };
    std::string session = "original-session";
    int server_sample_rate() const { return 24000; }
    const std::string& session_id() const { return session; }
};
struct Application {
    std::unique_ptr<Protocol> protocol_{new Protocol};
    std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_ = std::make_shared<ChatProtocolSignals>();
    std::atomic<uint64_t> protocol_generation_{1};
    std::atomic<uint32_t> protocol_callback_connect_generation_{1}, connect_generation_{1}, chat_source_connect_generation_{1};
    std::atomic<bool> chat_protocol_owned_{false}, passive_ws_intent_{false}, connect_in_flight_{false}, lesson_asset_sync_quiet_{false};
    std::atomic<unsigned> events{0};
    std::atomic<unsigned>* event_group_ = &events;
    ChatInboundMessages chat_inbound_messages_;
    std::vector<int> dispatched;
    std::vector<ChatRequestContext> retained;
    std::function<void(ChatRequestContext)> dispatch;
    bool IsLessonVoiceRoute() const { return false; }
    bool IsChatLessonRequestCurrent(const ChatRequestContext&) const { return true; }
    int GetDeviceState() const { return 0; }
    void HandleChatStart(std::shared_ptr<ChatProtocolSignals>,uint64_t,ConnectionSource,const cJSON*,ConnectionReceipt) {}
    void HandleChatTerminalStop(std::shared_ptr<ChatProtocolSignals>,uint64_t,ConnectionSource,const cJSON*,uint64_t) {}
    void HandleChatLessonAudio(std::shared_ptr<ChatProtocolSignals>,uint64_t,ConnectionSource,std::unique_ptr<AudioStreamPacket>) {}
    void HandleChatAudio(std::shared_ptr<ChatProtocolSignals>,uint64_t,ConnectionSource,std::unique_ptr<AudioStreamPacket>) {}
    void DispatchIncomingJson(const cJSON* root, uint64_t, bool, ChatRequestContext context) {
        dispatched.push_back(cJSON_GetObjectItem(root,"number")->valueint);
        if (dispatch) dispatch(std::move(context));
    }
    Protocol::SourceCallbacks MakeChatSourceCallbacks(uint64_t,std::shared_ptr<ChatProtocolSignals>);
    void PollChatInboundMessages();
    void PollChatStart(uint64_t) {}
    bool IsChatConnectionCurrent(ConnectionSource,uint64_t,uint32_t) const;
    bool IsChatRequestCurrent(const ChatRequestContext&) const;
    void FailChatRequest(const ChatRequestContext&);
    Application() { assert(chat_protocol_signals_->EnableForSource({1,7})); }
};
// PRODUCTION_METHODS

using Json = std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
struct Barrier {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered=false, released=false;
    void Block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex);released=true;cv.notify_all(); }
};
std::function<bool()> allocate;
void* JsonAllocate(size_t size) {
    if(allocate && !allocate()) return nullptr;
    return std::malloc(size);
}
void Hooks(std::function<bool()> callback) {
    allocate=std::move(callback);
    cJSON_Hooks hooks{JsonAllocate,std::free};cJSON_InitHooks(&hooks);
}
void ResetHooks() { cJSON_InitHooks(nullptr);allocate={}; }
Json Frame(int number) {
    Json root(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(root.get(),"type","mcp");
    cJSON_AddNumberToObject(root.get(),"number",number);
    return root;
}
void Enqueue(Application& app, int number, ConnectionSource source={1,7}) {
    auto root=Frame(number);
    assert(app.chat_inbound_messages_.Admit(root.get(),{source,1,1},now_us,88,"original-session"));
}
int main() {
    using Admission=ChatInboundMessages::Admission;
    using ReadStatus=ChatInboundMessages::ReadStatus;
    {
        Application app;
        for(int i=0;i<4;++i) Enqueue(app,i);
        app.PollChatInboundMessages();
        assert((app.dispatched == std::vector<int>{0,1,2,3}));
        assert(app.chat_inbound_messages_.Outstanding()==0);
    }
    {
        Application app;
        Enqueue(app,0,{9,7});
        for(int i=1;i<4;++i) Enqueue(app,i);
        app.PollChatInboundMessages();
        assert((app.dispatched == std::vector<int>{1,2,3}));
        assert(app.chat_inbound_messages_.Outstanding()==0);
    }
    {
        Application app;
        auto root=Frame(0);
        Barrier copying;
        Hooks([&]{copying.Block();return true;});
        auto producer=std::async(std::launch::async,[&]{
            return app.chat_inbound_messages_.Admit(root.get(),{{1,7},1,1},now_us,88);
        });
        copying.Wait();
        assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},1,1},now_us,88)==Admission::Busy);
        assert(app.chat_inbound_messages_.TryTake().status==ReadStatus::Busy);
        assert(app.chat_inbound_messages_.Pending());
        app.events=0;
        app.PollChatInboundMessages();
        assert(app.events.load()==MAIN_EVENT_CHAT_OUTBOUND);
        copying.Release();assert(producer.get());ResetHooks();
        app.events=0;app.PollChatInboundMessages();
        assert((app.dispatched==std::vector<int>{0}));
        assert(!app.events && !app.chat_inbound_messages_.Outstanding());
        assert(app.chat_inbound_messages_.TryTake().status==ReadStatus::Empty);
    }
    {
        Application app;auto root=Frame(0);
        assert(!app.chat_inbound_messages_.Admit(root.get(),{},now_us,88));
        assert(!app.chat_inbound_messages_.Outstanding());
    }
    {
        Application app;
        for(int i=0;i<4;++i) Enqueue(app,i);
        app.dispatch=[&](ChatRequestContext context){app.retained.push_back(std::move(context));};
        app.PollChatInboundMessages();assert(app.retained.size()==4);
        unsigned waits=0;
        receiver_wait=[&]{++waits;app.retained.pop_back();now_us+=1;};
        auto root=Frame(4);auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        callbacks.json({1,7},root.get(),88,{now_us,0});receiver_wait={};
        assert(waits==1 && app.chat_protocol_signals_->MatchesSource({1,7}));
        app.dispatch={};app.PollChatInboundMessages();
        assert((app.dispatched==std::vector<int>{0,1,2,3,4}));
        app.retained.clear();assert(!app.chat_inbound_messages_.Outstanding());
    }
    {
        Application app;auto root=Frame(9);
        assert(app.chat_inbound_messages_.TryAdmit(nullptr,{{1,7},1,1},100,88)==Admission::Invalid);
        assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},0,1},100,88)==Admission::Invalid);
        assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},1,0},100,88)==Admission::Invalid);
        assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},1,1},UINT64_MAX,88)==Admission::Invalid);
        forbid_new=true;
        assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},1,1},100,88)==Admission::NoMemory);
        forbid_new=false;
        assert(!app.chat_inbound_messages_.Outstanding() && !app.chat_inbound_messages_.Pending());
        for(size_t failed=0;failed<6;++failed) {
            size_t allocations=0;Hooks([&]{return allocations++!=failed;});
            assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},1,1},100,88)==Admission::NoMemory);
            ResetHooks();assert(!app.chat_inbound_messages_.Outstanding() && !app.chat_inbound_messages_.Pending());
        }
        assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},1,1},100,88,"captured")==Admission::Accepted);
        auto context=app.chat_inbound_messages_.TryTake().context;
        assert(context->received_us==100 && context->deadline_us==10000100 && context->lesson_epoch==88);
        assert(context->session_id=="captured" && context->owner.Matches({{1,7},1,1}));
        assert(context->root.get()!=root.get());
    }
    for(int mode=0;mode<5;++mode) {
        now_us=100;
        Application app;auto root=Frame(4);
        for(int i=0;i<4;++i) Enqueue(app,i);
        app.dispatch=[&](ChatRequestContext context){app.retained.push_back(std::move(context));};
        app.PollChatInboundMessages();
        assert(app.chat_inbound_messages_.TryAdmit(root.get(),{{1,7},1,1},100,88)==Admission::Full);
        unsigned waits=0;
        receiver_wait=[&]{
            ++waits;now_us+=50000;
            if(mode==2) {app.connect_generation_=2;app.chat_source_connect_generation_=2;}
            if(mode==3) assert(app.chat_protocol_signals_->EnableForSource({2,8}));
            if(mode==4) {app.protocol_->session="successor-session";app.retained.pop_back();}
        };
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        callbacks.json({1,7},root.get(),88,{100,mode==1 ? 100100ULL : 0});receiver_wait={};
        ChatProtocolSignals::Failure failure;
        if(mode<2) {
            assert(waits==(mode==1 ? 2U : 5U));
            assert(app.chat_protocol_signals_->ReadFailure(failure));
            assert(failure.source.source_id==1 && failure.connect_generation==1);
            assert(!app.chat_inbound_messages_.Pending());
        } else if(mode<4) {
            assert(waits==1 && !app.chat_protocol_signals_->ReadFailure(failure));
            assert(!app.chat_inbound_messages_.Pending());
        } else {
            assert(waits==1);
            auto context=app.chat_inbound_messages_.Take();
            assert(context && context->session_id=="original-session" && context->received_us==100);
            assert(context->deadline_us==10000100 && context->owner.Matches({{1,7},1,1}));
        }
    }
    for(int mode=0;mode<6;++mode) {
        now_us=100;
        Application app;auto root=Frame(0);unsigned waits=0;
        receiver_wait=[&]{++waits;};
        if(mode==0) Hooks([]{return false;});
        if(mode==1) Hooks([]{now_us=250100;return true;});
        if(mode==2) Hooks([&]{app.connect_generation_=2;app.chat_source_connect_generation_=2;return true;});
        if(mode==3) now_us=250100;
        if(mode==5) now_us=99;
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        callbacks.json({1,7},mode==4 ? nullptr : root.get(),88,{100,0});
        ResetHooks();receiver_wait={};
        assert(!waits && !app.chat_inbound_messages_.Outstanding() && !app.chat_inbound_messages_.Pending());
        ChatProtocolSignals::Failure failure;
        assert(app.chat_protocol_signals_->ReadFailure(failure)==(mode!=2));
    }
    {
        now_us=100;Application app;
        for(int i=0;i<4;++i) Enqueue(app,i);
        app.dispatch=[&](ChatRequestContext context) {
            if(cJSON_GetObjectItem(context->root.get(),"number")->valueint==1) Enqueue(app,4);
        };
        app.events=0;app.PollChatInboundMessages();
        assert((app.dispatched==std::vector<int>{0,1,2,3}));
        assert(app.events && app.chat_inbound_messages_.Pending());
        app.events=0;app.PollChatInboundMessages();
        assert((app.dispatched==std::vector<int>{0,1,2,3,4}));
        assert(!app.events && !app.chat_inbound_messages_.Outstanding());
    }
    {
        now_us=100;Application app;auto root=Frame(0);Barrier copying;
        Hooks([&]{copying.Block();return true;});
        auto producer=std::async(std::launch::async,[&]{
            return app.chat_inbound_messages_.Admit(root.get(),{{1,7},1,1},100,88);
        });
        copying.Wait();
        unsigned waits=0;
        receiver_wait=[&]{++waits;copying.Release();assert(producer.get());ResetHooks();};
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        callbacks.json({1,7},root.get(),88,{100,0});receiver_wait={};
        assert(waits==1 && app.chat_protocol_signals_->MatchesSource({1,7}));
        app.PollChatInboundMessages();assert(app.dispatched.size()==2);
        assert(!app.chat_inbound_messages_.Outstanding());
    }
    for(bool throwing : {false,true}) {
        now_us=100;Application app;
        for(int i=0;i<4;++i)Enqueue(app,i);
        if(throwing)app.dispatch=[](ChatRequestContext){throw std::runtime_error("dispatch");};
        else now_us=10000100;
        app.PollChatInboundMessages();
        assert(!app.chat_inbound_messages_.Outstanding() && !app.chat_inbound_messages_.Pending());
        ChatProtocolSignals::Failure failure;assert(app.chat_protocol_signals_->ReadFailure(failure));
        assert(failure.source.source_id==1);
    }
    {
        now_us=100;Application app;auto root=Frame(0);
        app.protocol_->session.assign(100,'s');
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        forbid_new=true;callbacks.json({1,7},root.get(),88,{100,0});forbid_new=false;
        assert(strstr(last_log,"reason=json_admission outcome=4"));
        assert(!app.chat_inbound_messages_.Outstanding());
        ChatProtocolSignals::Failure failure;assert(app.chat_protocol_signals_->ReadFailure(failure));
    }
    {
        now_us=100;Application app;auto root=Frame(4);
        for(int i=0;i<4;++i)Enqueue(app,i);
        Barrier consumer;
        app.dispatch=[&](ChatRequestContext){consumer.Block();};
        auto application=std::async(std::launch::async,[&]{app.PollChatInboundMessages();});
        consumer.Wait();
        ConnectionInboundGate gate;const auto epoch=gate.BeginConnection();
        unsigned waits=0;
        receiver_wait=[&]{assert(gate.CurrentThreadHasLease());++waits;now_us+=25000;};
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        {
            auto lease=gate.Acquire(epoch);assert(lease);
            callbacks.json({1,7},root.get(),88,{100,0});
        }
        receiver_wait={};
        assert(waits==10 && now_us==250100);
        assert(application.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
        assert(app.chat_inbound_messages_.Outstanding()==4);
        ChatProtocolSignals::Failure failure;assert(app.chat_protocol_signals_->ReadFailure(failure));
        consumer.Release();application.get();
        assert(!app.chat_inbound_messages_.Outstanding() && !app.chat_inbound_messages_.Pending());
    }
}
