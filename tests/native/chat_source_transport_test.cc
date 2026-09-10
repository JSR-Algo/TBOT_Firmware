#define private public
#include "protocols/connection_source.h"
#undef private
#include "protocols/connection_inbound_gate.h"
#include "protocols/connection_close_state.h"
#include <cJSON.h>
#include <arpa/inet.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
template<class... T> void Log(T&&...) {}
#define ESP_LOGD(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_LOGI(...) Log(__VA_ARGS__)
const char* TAG="test";
constexpr int WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT=1;
void xEventGroupSetBits(int,int) {}
static uint64_t now_us=100;
int64_t esp_timer_get_time() { return now_us; }
static std::function<void()> publication_wait;
void vTaskDelay(int) { if(publication_wait)publication_wait();++now_us; }
bool JsonHasForbiddenDecodedNull(const char*,size_t) { return false; }
struct ServerHelloSignal { int handle=1;std::atomic<bool> validated{false},published{false};uint64_t deadline_us=0; };
enum class WebsocketSessionMode { kUnclaimedPublicLesson,kAuthenticatedRealtime };
struct AudioStreamPacket { int sample_rate=0,frame_duration=0; uint32_t timestamp=0; std::vector<uint8_t> payload; };
// PRODUCTION_BINARY
struct WebSocket {
    std::function<void(const char*,size_t,bool)> data;
    std::function<void()> disconnected;
    void OnData(decltype(data) fn) { data=std::move(fn); }
    void OnDisconnected(decltype(disconnected) fn) { disconnected=std::move(fn); }
    int GetLastError() { return 7; }
};
struct WebsocketProtocol {
    // PRODUCTION_SOURCE_CALLBACKS
    SourceCallbacks source_callbacks_;
    std::function<void(std::unique_ptr<AudioStreamPacket>)> on_incoming_audio_;
    std::function<void(const cJSON*,uint64_t)> on_incoming_json_;
    std::function<void()> on_audio_channel_opened_,on_audio_channel_closed_;
    std::function<void(const std::string&)> on_network_error_;
    ConnectionInboundGate inbound_gate_;
    ConnectionCloseState close_state_;
    ConnectionSourceSequence source_sequence_;
    ConnectionSource current_source_,websocket_source_;
    std::shared_ptr<ServerHelloSignal> last_hello;
    uint32_t websocket_connection_epoch_=0;
    std::unique_ptr<WebSocket> websocket_;
    bool error_occurred_=false;
    int version_=1,server_sample_rate_=24000,server_frame_duration_=60;
    WebsocketSessionMode session_mode_=WebsocketSessionMode::kAuthenticatedRealtime;
    struct { void OnPong(uint32_t) {} void OnOpened(uint32_t) {} } passive_liveness_;
    std::chrono::steady_clock::time_point last_incoming_time_;
    bool IsTimeout() { return false; }
    bool ParseServerHello(const cJSON*) { return true; }
    bool IsAllowedUnclaimedPublicLessonMessage(const cJSON*) { return true; }
    // PRODUCTION_DELIVERY
    void NotifyAudioChannelClosedOnce(ConnectionSource);
    void SetError(const std::string&,ConnectionSource);
    void CompleteCloseAndNotify();
    void DetachAndResetWebsocket(uint32_t,bool=false,ConnectionSource={});
    bool Publish(std::unique_ptr<WebSocket> replacement_websocket) {
        const auto source=current_source_;
        const auto connection_epoch=source.connection_epoch;
        const auto hello_signal=last_hello;
        // PRODUCTION_PUBLICATION
    ConnectionSource Attach(WebSocket* candidate_websocket, uint64_t callback_transport_epoch) {
        uint32_t connection_epoch=0;
        ConnectionSource source;
        // PRODUCTION_CONNECTION
        auto hello_signal=std::make_shared<ServerHelloSignal>();
        last_hello=hello_signal;
        // PRODUCTION_CALLBACKS
        return source;
    }
};
// PRODUCTION_METHODS
int main() {
    {
        WebsocketProtocol early;auto candidate=std::make_unique<WebSocket>();auto& socket=*candidate;
        bool adopted=false;unsigned delivered=0,waits=0;
        early.source_callbacks_.opened=[&](ConnectionSource source,uint64_t deadline){assert(early.websocket_source_.source_id==source.source_id && deadline==250100 && !adopted);};
        // PRODUCTION_ADOPTED_TEST
        uint64_t received=0,admission_deadline=0;
        // PRODUCTION_RECEIPT_TEST
        auto source=early.Attach(&socket,99);
        const char* hello="{\"type\":\"hello\"}";socket.data(hello,strlen(hello),false);
        publication_wait=[&]{
            {auto lease=early.inbound_gate_.TryAcquire(source.connection_epoch);assert(lease);}
            ++waits;
            if(waits==1){assert(early.Publish(std::move(candidate)));assert(early.last_hello->published && !adopted);}
            else adopted=true;
        };
        const char* message="{\"type\":\"lesson_state\"}";socket.data(message,strlen(message),false);
        assert(waits==2 && delivered==1 && adopted);publication_wait={};
        assert(received==100 && admission_deadline==250100);
    }
    for(int failure:{0,1,2}) {
        now_us=100;
        WebsocketProtocol held;auto candidate=std::make_unique<WebSocket>();auto* socket=candidate.get();
        bool adopted=false;unsigned delivered=0,errors=0;
        held.source_callbacks_.adopted=[&](ConnectionSource){return adopted;};
        held.source_callbacks_.json=[&](ConnectionSource,const cJSON*,uint64_t,ConnectionReceipt){++delivered;};
        held.source_callbacks_.audio=[&](ConnectionSource,std::unique_ptr<AudioStreamPacket>){++delivered;};
        held.source_callbacks_.error=[&](ConnectionSource,const std::string&){++errors;};
        auto source=held.Attach(socket,77);
        const char* hello="{\"type\":\"hello\"}";socket->data(hello,strlen(hello),false);
        assert(held.Publish(std::move(candidate)));
        publication_wait=[&]{
            assert(held.inbound_gate_.TryAcquire(source.connection_epoch));
            if(failure==0)now_us=250100;
            if(failure==1)socket->disconnected();
            if(failure==2)held.inbound_gate_.BeginConnection();
        };
        socket->data("x",1,true);
        assert(!delivered && errors==(failure==0 ? 1U : 0U));publication_wait={};
    }
    now_us=100;
    WebsocketProtocol protocol;
    WebSocket old_socket,new_socket;
    std::vector<ConnectionSource> audio,json,closed,errors,opened;
    uint64_t lesson=0;
    protocol.source_callbacks_.audio=[&](ConnectionSource s,std::unique_ptr<AudioStreamPacket> p){assert(p->payload.size()==1);audio.push_back(s);};
    protocol.source_callbacks_.json=[&](ConnectionSource s,const cJSON*,uint64_t e,ConnectionReceipt){json.push_back(s);lesson=e;};
    protocol.source_callbacks_.closed=[&](ConnectionSource s){closed.push_back(s);};
    protocol.source_callbacks_.opened=[&](ConnectionSource s,uint64_t){opened.push_back(s);};
    protocol.source_callbacks_.error=[&](ConnectionSource s,const std::string&){errors.push_back(s);};
    const auto a=protocol.Attach(&old_socket,501);
    old_socket.data("x",1,true);
    const auto b=protocol.Attach(&new_socket,777);
    old_socket.data("x",1,true); old_socket.disconnected(); protocol.SetError("old",a);
    assert(audio.size()==1 && closed.empty() && errors.empty());
    new_socket.data("x",1,true);
    const char* message="{\"type\":\"lesson_state\"}";
    new_socket.data(message,strlen(message),false);
    assert(audio.back().source_id==b.source_id && json.back().source_id==b.source_id && lesson==777);
    protocol.DeliverAudioChannelOpened(b); assert(opened.back().source_id==b.source_id);
    protocol.SetError("new",b); assert(errors.back().source_id==b.source_id);
    new_socket.disconnected(); assert(closed.back().source_id==b.source_id);
    new_socket.disconnected(); assert(closed.size()==1);

    for (int version : {2,3}) {
        WebSocket socket;
        const auto source=protocol.Attach(&socket,42);
        protocol.version_=version;
        std::vector<uint8_t> frame((version==2 ? sizeof(BinaryProtocol2) : sizeof(BinaryProtocol3))+1);
        if (version==2) {
            auto* header=reinterpret_cast<BinaryProtocol2*>(frame.data());
            header->version=htons(2);header->payload_size=htonl(1);
        } else {
            auto* header=reinterpret_cast<BinaryProtocol3*>(frame.data());header->payload_size=htons(1);
        }
        socket.data(reinterpret_cast<char*>(frame.data()),frame.size(),true);
        assert(audio.back().source_id==source.source_id);
    }

    // Closing installed socket A during candidate B handshake identifies A.
    WebsocketProtocol explicit_close;
    WebSocket candidate;
    auto installed=std::make_unique<WebSocket>();
    const auto installed_source=explicit_close.Attach(installed.get(),1);
    explicit_close.websocket_source_=installed_source;
    explicit_close.websocket_=std::move(installed);
    explicit_close.Attach(&candidate,2);
    unsigned explicit_notifications=0;
    explicit_close.source_callbacks_.closed=[&](ConnectionSource s){++explicit_notifications;assert(s.source_id==installed_source.source_id);};
    explicit_close.CompleteCloseAndNotify();
    assert(explicit_notifications==1);
    const auto candidate_source=explicit_close.Attach(&candidate,3);
    explicit_close.source_callbacks_.closed=[&](ConnectionSource s){++explicit_notifications;assert(s.source_id==candidate_source.source_id);};
    explicit_close.CompleteCloseAndNotify(); assert(explicit_notifications==2);

    // Legacy fallback retains both payload and the independent lesson epoch.
    WebsocketProtocol legacy;
    WebSocket legacy_socket;
    unsigned legacy_audio=0,legacy_json=0;
    legacy.on_incoming_audio_=[&](std::unique_ptr<AudioStreamPacket>){++legacy_audio;};
    legacy.on_incoming_json_=[&](const cJSON*,uint64_t e){assert(e==999);++legacy_json;};
    legacy.Attach(&legacy_socket,999);
    legacy_socket.data("x",1,true); legacy_socket.data(message,strlen(message),false);
    assert(legacy_audio==1 && legacy_json==1);

    WebsocketProtocol exhausted;
    WebSocket never_connected;
    exhausted.source_sequence_.next_=UINT32_MAX-1;
    assert(!exhausted.Attach(&never_connected,1).Valid());
    assert(!never_connected.data && exhausted.inbound_gate_.HealthyEpoch()==0);
}
