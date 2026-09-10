// The runner inserts the production conditional sender body at the marker.
// Only the socket effect is substituted; gate and cJSON are production code.
#include "protocols/connection_inbound_gate.h"
#include <cJSON.h>
#include <cassert>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

enum class ConversationTtsAckResult { Busy, Stale, Sent, Failed };
class WebsocketProtocol {
public:
    ConnectionInboundGate inbound_gate_;
    uint32_t websocket_connection_epoch_ = 0;
    std::string session_id_ = "session\"\\\n";
    std::string sent;
    bool fail_send = false;
    ConversationTtsAckResult SendConversationTtsDrainAckIfCurrent(
        const std::string&, uint32_t);
    bool SendText(const std::string& text) {
        assert(inbound_gate_.CurrentThreadHasLease());
        if (fail_send) {
            inbound_gate_.FailCurrent();
            return false;
        }
        sent = text;
        return true;
    }
};

// PRODUCTION_SENDER

static int remaining_allocations = -1;
static void* Allocate(size_t size) {
    if (remaining_allocations == 0) return nullptr;
    if (remaining_allocations > 0) --remaining_allocations;
    return std::malloc(size);
}

int main() {
    using Result = ConversationTtsAckResult;
    WebsocketProtocol protocol;
    const auto old = protocol.inbound_gate_.BeginConnection();
    const std::string id = "chat:quoted\"\\\n";
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, old) == Result::Busy);
    assert(protocol.sent.empty());
    protocol.websocket_connection_epoch_ = old;
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, old) == Result::Sent);
    cJSON* parsed = cJSON_Parse(protocol.sent.c_str());
    assert(parsed && cJSON_GetArraySize(parsed) == 4);
    assert(std::string(cJSON_GetObjectItem(parsed, "type")->valuestring) == "tts_ack");
    assert(std::string(cJSON_GetObjectItem(parsed, "state")->valuestring) == "stop");
    assert(std::string(cJSON_GetObjectItem(parsed, "drainId")->valuestring) == id);
    assert(std::string(cJSON_GetObjectItem(parsed, "session_id")->valuestring) == protocol.session_id_);
    cJSON_Delete(parsed);
    for (const auto& invalid : {std::string(), std::string("chat:"), std::string("lesson:x"),
                               std::string("chat:") + std::string(124, 'x'), std::string("chat:a\0b", 8)}) {
        protocol.sent.clear();
        assert(protocol.SendConversationTtsDrainAckIfCurrent(invalid, old) == Result::Failed);
        assert(protocol.sent.empty());
    }
    assert(protocol.SendConversationTtsDrainAckIfCurrent("chat:" + std::string(123, 'x'), old) == Result::Sent);
    cJSON_Hooks hooks{Allocate, std::free};
    cJSON_InitHooks(&hooks);
    bool reached_success = false;
    for (int budget = 0; budget < 30; ++budget) {
        remaining_allocations = budget;
        protocol.sent.clear();
        const auto result = protocol.SendConversationTtsDrainAckIfCurrent(id, old);
        if (result == Result::Sent) { reached_success = true; break; }
        assert(result == Result::Failed && protocol.sent.empty());
    }
    assert(reached_success);
    cJSON_InitHooks(nullptr);
    protocol.sent.clear();
    std::promise<void> held, release;
    auto released = release.get_future();
    std::thread holder([&] {
        auto lease = protocol.inbound_gate_.Acquire(old);
        held.set_value();
        released.wait();
    });
    held.get_future().wait();
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, old) == Result::Busy);
    assert(protocol.sent.empty());
    release.set_value();
    holder.join();
    protocol.inbound_gate_.FailCurrent();
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, old) == Result::Stale);
    const auto next = protocol.inbound_gate_.BeginConnection();
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, old) == Result::Stale);
    assert(protocol.sent.empty());
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, next) == Result::Busy);
    assert(protocol.sent.empty());
    protocol.websocket_connection_epoch_ = next;
    protocol.fail_send = true;
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, next) == Result::Failed);
    assert(protocol.inbound_gate_.HealthyEpoch() == 0);
    assert(protocol.SendConversationTtsDrainAckIfCurrent(id, next) == Result::Stale);
    assert(protocol.sent.empty());
}
