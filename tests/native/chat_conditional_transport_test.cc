// Compile the actual transport methods against a controlled socket boundary.
#include <memory>
#include "protocols/protocol.h"
#include "chat_outbound_mailbox.h"
#include "protocols/connection_inbound_gate.h"
#include "protocols/passive_websocket_liveness.h"
#include <arpa/inet.h>
#include <cassert>
#include <future>
#include <thread>
#include <new>

static int cpp_allocations = -1;
void* operator new(size_t size) {
    if (cpp_allocations == 0) throw std::bad_alloc();
    if (cpp_allocations > 0) --cpp_allocations;
    if (auto* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }

#define ESP_LOGE(...) ((void)0)
namespace Lang { namespace Strings { const char* SERVER_ERROR = "error"; } }
using Result = ChatOutboundMailbox::Result;
using Kind = ChatOutboundMailbox::Kind;
using Job = ChatOutboundMailbox::Job;

uint64_t esp_timer_get_time() { return 100; }
struct Socket {
    ConnectionInboundGate& gate;
    bool success = true;
    int writes = 0;
    bool binary = false;
    std::string bytes;
    std::function<void()> barrier;
    bool Send(const void* data, size_t size, bool is_binary) {
        assert(gate.CurrentThreadHasLease());
        ++writes;
        bytes.assign(static_cast<const char*>(data), size);
        binary = is_binary;
        if (barrier) barrier();
        return success;
    }
    bool Send(const std::string& text) { return Send(text.data(), text.size(), false); }
};

class WebsocketProtocol {
public:
    PassiveWebsocketLiveness passive_liveness_;
    ConnectionInboundGate inbound_gate_;
    Socket socket{inbound_gate_, true, 0, false, {}, {}};
    Socket* websocket_ = &socket;
    uint32_t websocket_connection_epoch_ = inbound_gate_.BeginConnection();
    ConnectionSource websocket_source_{1,websocket_connection_epoch_};
    std::string session_id_ = "session\"\\\n";
    int version_ = 1;
    bool opened = true;
    int errors = 0;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;
    bool IsAudioChannelOpened() const { return opened; }
    void SetError(const std::string&) { ++errors; }
    void SetError(const std::string&,ConnectionSource source) { assert(source.source_id==websocket_source_.source_id); ++errors; }
    bool SendText(const std::string&);
    bool SendTextForSource(const std::string&,ConnectionSource);
    Result SendChatControlIfCurrent(const Job&, const std::function<bool()>&);
    Result SendChatFullTextIfCurrent(const Job&, const std::function<bool()>&);
    int ObserveChatPassiveLiveness(ConnectionSource);
    Result SendChatAudioIfCurrent(const AudioStreamPacket&, uint32_t,
                                 const std::function<bool(const ChatCaptureTag&)>&);
};

// PRODUCTION_METHODS

static bool authorized = true;
static bool Authorize() { return authorized; }
static bool AuthorizeAudio(const ChatCaptureTag&) { return authorized; }
static int allocations = -1;
static int live_cjson_allocations = 0;
static bool invalidate_on_allocation = false;
static void* Allocate(size_t size) {
    if (invalidate_on_allocation) authorized = false;
    if (allocations == 0) return nullptr;
    if (allocations > 0) --allocations;
    auto* result = std::malloc(size);
    if (result) ++live_cjson_allocations;
    return result;
}
static void Free(void* pointer) {
    if (pointer) --live_cjson_allocations;
    std::free(pointer);
}
static void AssertField(const Socket& socket, const char* field, const std::string& expected) {
    auto* root = cJSON_Parse(socket.bytes.c_str());
    assert(root);
    auto* value = cJSON_GetObjectItem(root, field);
    assert(cJSON_IsString(value) && value->valuestring == expected);
    cJSON_Delete(root);
}

int main() {
    {
        WebsocketProtocol p;
        assert(p.ObserveChatPassiveLiveness(p.websocket_source_)==0);
        assert(p.ObserveChatPassiveLiveness({9,9})==-1);
        std::promise<void> locked,release;auto wait=release.get_future().share();
        auto holder=std::async(std::launch::async,[&]{auto lease=p.inbound_gate_.Acquire(p.websocket_connection_epoch_);locked.set_value();wait.wait();});
        locked.get_future().wait();assert(p.ObserveChatPassiveLiveness(p.websocket_source_)==0);
        release.set_value();holder.get();assert(!p.socket.writes);
    }
    {
        WebsocketProtocol p;Job full;full.kind=Kind::FullText;
        full.source=p.websocket_source_;full.connection_epoch=full.source.connection_epoch;
        full.deadline_us=UINT64_MAX;full.full_text=std::make_shared<const std::string>(65535,'x');
        assert(p.SendChatFullTextIfCurrent(full,[]{return true;})==Result::Sent);
        full.source.source_id++;
        assert(p.SendChatFullTextIfCurrent(full,[]{return true;})==Result::Stale);
        full.source=p.websocket_source_;full.full_text=std::make_shared<const std::string>(65536,'x');
        assert(p.SendChatFullTextIfCurrent(full,[]{return true;})==Result::Failed);
        full.full_text=std::make_shared<const std::string>("{\"type\":\"ping\"}");
        assert(p.SendChatFullTextIfCurrent(full,[]{return false;})==Result::Stale);
        assert(p.passive_liveness_.Poll(2000)==PassiveWebsocketLiveness::Action::kSendPing);
        p.passive_liveness_.OnOpened(0);
        assert(p.SendChatFullTextIfCurrent(full,[]{return true;})==Result::Sent);
        assert(p.passive_liveness_.Poll(2000)==PassiveWebsocketLiveness::Action::kNone);
    }
    using namespace std::chrono_literals;
    for (int budget : {0, 1}) {
        WebsocketProtocol p;
        Job job;
        job.kind = Kind::Wake;
        job.connection_epoch = p.websocket_connection_epoch_;
        assert(job.SetPayload(std::string(128, 'x').data(), 128));
        std::function<bool()> authorize = Authorize;
        cJSON_Hooks hooks{Allocate, Free};
        cJSON_InitHooks(&hooks);
        cpp_allocations = budget;
        bool escaped = false;
        Result result = Result::Sent;
        try { result = p.SendChatControlIfCurrent(job, authorize); }
        catch (const std::bad_alloc&) { escaped = true; }
        cpp_allocations = -1;
        assert(!escaped && result == Result::Failed && p.socket.writes == 0);
        assert(live_cjson_allocations == 0);
        cJSON_InitHooks(nullptr);
    }
    for (int version : {2, 3}) {
        WebsocketProtocol p;
        p.version_ = version;
        AudioStreamPacket packet;
        packet.payload.resize(1024);
        std::function<bool(const ChatCaptureTag&)> authorize = AuthorizeAudio;
        cpp_allocations = 0;
        bool escaped = false;
        Result result = Result::Sent;
        try { result = p.SendChatAudioIfCurrent(packet, p.websocket_connection_epoch_, authorize); }
        catch (const std::bad_alloc&) { escaped = true; }
        cpp_allocations = -1;
        assert(!escaped && result == Result::Failed && p.socket.writes == 0 && packet.payload.size() == 1024);
    }
    for (auto kind : {Kind::ListenStart, Kind::ListenStop, Kind::Abort, Kind::Wake, Kind::DrainAck}) {
        WebsocketProtocol p;
        Job job;
        job.kind = kind;
        job.argument = kind == Kind::Abort ? kAbortReasonWakeWordDetected : kListeningModeRealtime;
        job.connection_epoch = p.websocket_connection_epoch_;
        const std::string payload = kind == Kind::DrainAck ? "chat:quoted\"\\\n" : "wake\"\\\n";
        assert(job.SetPayload(payload.data(), payload.size()));
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Sent);
        assert(p.socket.writes == 1 && !p.socket.binary);
        AssertField(p.socket, "session_id", p.session_id_);
        AssertField(p.socket, "type", kind == Kind::DrainAck ? "tts_ack" : kind == Kind::Abort ? "abort" : "listen");
        if (kind == Kind::ListenStart) AssertField(p.socket, "mode", "realtime");
        if (kind == Kind::ListenStop || kind == Kind::DrainAck) AssertField(p.socket, "state", "stop");
        if (kind == Kind::Abort) AssertField(p.socket, "reason", "wake_word_detected");
        if (kind == Kind::Wake) AssertField(p.socket, "text", payload);
        if (kind == Kind::DrainAck) AssertField(p.socket, "drainId", payload);
        p.socket.success = false;
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Failed);
        assert(p.opened && p.errors == 1 && p.socket.writes == 2);
        p.socket.success = true;
        authorized = false;
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Stale);
        authorized = true;
        assert(p.SendChatControlIfCurrent(job, {}) == Result::Stale);
        assert(p.socket.writes == 2);
        cJSON_Hooks hooks{Allocate, Free};
        cJSON_InitHooks(&hooks);
        invalidate_on_allocation = true;
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Stale);
        invalidate_on_allocation = false;
        authorized = true;
        assert(p.socket.writes == 2);
        bool success = false;
        for (int budget = 0; budget < 40; ++budget) {
            allocations = budget;
            auto result = p.SendChatControlIfCurrent(job, Authorize);
            if (result == Result::Sent) { success = true; break; }
            assert(result == Result::Failed && p.socket.writes == 2);
        }
        assert(success);
        assert(live_cjson_allocations == 0);
        cJSON_InitHooks(nullptr);
        allocations = -1;
    }
    for (int version : {1, 2, 3}) {
        WebsocketProtocol p;
        p.version_ = version;
        AudioStreamPacket packet;
        packet.payload = {0x11, 0x22, 0x33};
        packet.timestamp = 0x12345678;
        const auto original = packet.payload;
        auto send = [&] { return p.SendChatAudioIfCurrent(packet, p.websocket_connection_epoch_, AuthorizeAudio); };
        assert(send() == Result::Sent && p.socket.binary);
        auto bytes = reinterpret_cast<const unsigned char*>(p.socket.bytes.data());
        if (version == 2) {
            const unsigned char expected[] = {0,2,0,0,0,0,0,0,0x12,0x34,0x56,0x78,0,0,0,3,0x11,0x22,0x33};
            assert(p.socket.bytes.size() == sizeof(expected) && !std::memcmp(bytes, expected, sizeof(expected)));
        } else if (version == 3) {
            const unsigned char expected[] = {0,0,0,3,0x11,0x22,0x33};
            assert(p.socket.bytes.size() == sizeof(expected) && !std::memcmp(bytes, expected, sizeof(expected)));
        } else assert(p.socket.bytes == std::string("\x11\x22\x33", 3));
        p.socket.success = false;
        assert(send() == Result::Failed && p.opened);
        authorized = false;
        assert(send() == Result::Stale);
        authorized = true;
        assert(p.socket.writes == 2 && packet.payload == original);
        ChatUplinkAuthorization uplink;
        const auto revoked = uplink.Revoke();
        uplink.AcknowledgePrepared(revoked, true);
        assert(uplink.Arm(revoked));
        packet.capture_tag = uplink.Capture();
        int checks = 0;
        auto revoke_before_submit = [&](const ChatCaptureTag& captured) {
            assert(captured == packet.capture_tag);
            if (++checks == 2) uplink.Revoke();
            return uplink.IsCurrentChat(captured);
        };
        assert(p.SendChatAudioIfCurrent(packet, p.websocket_connection_epoch_, revoke_before_submit) == Result::Stale);
        assert(checks == 2 && p.socket.writes == 2 && packet.payload == original);
    }
    {
        WebsocketProtocol p;
        Job job;
        job.connection_epoch = p.websocket_connection_epoch_;
        job.kind = Kind::Wake;
        assert(job.SetPayload(std::string(128, 'x').data(), 128));
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Sent);
        job.payload_size = 129;
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Failed);
        job.payload_size = 128;
        job.payload[0] = '\0';
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Failed);
        assert(job.SetPayload("chat:", 5));
        job.kind = Kind::DrainAck;
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Failed);
        assert(job.SetPayload("lesson:x", 8));
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Failed);
        job.kind = static_cast<Kind>(99);
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Failed);
        job.kind = Kind::ListenStop;
        p.session_id_ = std::string("a\0b", 3);
        assert(p.SendChatControlIfCurrent(job, Authorize) == Result::Failed);
        assert(p.socket.writes == 1);
    }
    struct SendCase { bool audio; Kind kind; };
    for (const auto test : {SendCase{false, Kind::ListenStart}, SendCase{false, Kind::ListenStop},
                           SendCase{false, Kind::Abort}, SendCase{false, Kind::Wake},
                           SendCase{false, Kind::DrainAck}, SendCase{true, Kind::ListenStart}}) {
        WebsocketProtocol p;
        ChatOutboundMailbox mailbox;
        Job job;
        job.request_id = 1;
        job.generation = mailbox.AdvanceGeneration();
        job.protocol_generation = 1;
        job.connection_epoch = p.websocket_connection_epoch_;
        job.kind = test.kind;
        job.argument = test.kind == Kind::Abort ? kAbortReasonWakeWordDetected : kListeningModeRealtime;
        const std::string payload = test.kind == Kind::DrainAck ? "chat:barrier" : "wake";
        assert(job.SetPayload(payload.data(), payload.size()));
        assert(mailbox.TrySubmit(job));
        assert(mailbox.TryTake(job));
        AudioStreamPacket packet;
        packet.payload = {1, 2, 3};
        auto current = [&] { return mailbox.IsCurrent(job.generation); };
        auto current_audio = [&](const ChatCaptureTag&) { return current(); };
        auto send = [&] { return test.audio ? p.SendChatAudioIfCurrent(packet, job.connection_epoch, current_audio)
                                       : p.SendChatControlIfCurrent(job, current); };
        p.websocket_connection_epoch_ = 0;
        assert(send() == Result::Busy && p.socket.writes == 0);
        p.websocket_connection_epoch_ = job.connection_epoch;
        std::promise<void> held, release;
        auto released = release.get_future();
        auto holder = std::async(std::launch::async, [&] {
            auto lease = p.inbound_gate_.Acquire(job.connection_epoch);
            held.set_value();
            released.wait();
        });
        held.get_future().wait();
        assert(send() == Result::Busy && p.socket.writes == 0 && packet.payload.size() == 3);
        release.set_value();
        holder.get();
        std::promise<void> writing, finish;
        auto finished = finish.get_future();
        p.socket.barrier = [&] { writing.set_value(); finished.wait(); };
        auto pending = std::async(std::launch::async, send);
        writing.get_future().wait();
        mailbox.AdvanceGeneration();
        std::promise<void> replacing;
        auto replacement = std::async(std::launch::async, [&] {
            replacing.set_value();
            auto mutation = p.inbound_gate_.BeginConnectionMutation();
            p.websocket_connection_epoch_ = mutation.epoch();
        });
        replacing.get_future().wait();
        assert(replacement.wait_for(30ms) == std::future_status::timeout);
        finish.set_value();
        const auto result = pending.get();
        assert(result == Result::Sent);
        assert(mailbox.TryComplete(job, result));
        ChatOutboundMailbox::Completion completion;
        assert(mailbox.TryCollect(completion));
        assert(completion.stale && completion.result == Result::Sent);
        replacement.get();
        assert(!current() && p.socket.writes == 1);
        assert(send() == Result::Stale && p.socket.writes == 1);
    }
}
