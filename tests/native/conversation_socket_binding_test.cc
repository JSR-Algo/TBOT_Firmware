#include "protocols/connection_inbound_gate.h"
#include "protocols/connection_source.h"
#include <cassert>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
using namespace std::chrono_literals;
uint64_t esp_timer_get_time() { return 0; }
struct WebSocket {
    std::function<void()> on_destroy;
    ~WebSocket() { if (on_destroy) on_destroy(); }
};
class WebsocketProtocol {
public:
    ConnectionInboundGate inbound_gate_;
    std::unique_ptr<WebSocket> websocket_;
    uint32_t websocket_connection_epoch_ = 0;
    ConnectionSource websocket_source_;
    bool error_occurred_ = false;
    struct Liveness { void OnOpened(uint32_t) {} } passive_liveness_;
    std::function<void()> on_audio_channel_opened_;
    int notifications = 0;
    void NotifyAudioChannelClosedOnce(ConnectionSource) { ++notifications; }
    void DeliverAudioChannelOpened(ConnectionSource source,uint64_t deadline) { assert(deadline==250100 && source.connection_epoch==websocket_connection_epoch_); if(on_audio_channel_opened_) on_audio_channel_opened_(); }
    void DetachAndResetWebsocket(uint32_t expected_epoch, bool notify, ConnectionSource = {});
    bool Publish(uint32_t connection_epoch, std::unique_ptr<WebSocket> replacement_websocket)
    // PRODUCTION_PUBLICATION
};
// PRODUCTION_DETACH

int main() {
    WebsocketProtocol protocol;
    auto old = protocol.inbound_gate_.BeginConnection();
    auto first = std::make_unique<WebSocket>();
    first->on_destroy = [&] { assert(!protocol.inbound_gate_.CurrentThreadHasLease()); };
    assert(protocol.Publish(old, std::move(first)));
    auto next = protocol.inbound_gate_.BeginConnection();
    assert(protocol.websocket_connection_epoch_ == old);
    auto replacement = std::make_unique<WebSocket>();
    auto* expected = replacement.get();
    std::future<bool> publisher;
    std::promise<void> attempting;
    {
        auto send_lease = protocol.inbound_gate_.Acquire(next);
        publisher = std::async(std::launch::async, [&] {
            attempting.set_value();
            return protocol.Publish(next, std::move(replacement));
        });
        attempting.get_future().wait();
        assert(publisher.wait_for(30ms) == std::future_status::timeout);
        assert(protocol.websocket_connection_epoch_ == old);
    }
    assert(publisher.get());
    assert(protocol.websocket_.get() == expected && protocol.websocket_connection_epoch_ == next);
    protocol.DetachAndResetWebsocket(old, true);
    assert(protocol.websocket_.get() == expected && protocol.notifications == 0);
    protocol.inbound_gate_.FailCurrent();
    assert(!protocol.Publish(next, std::make_unique<WebSocket>()));
    assert(protocol.websocket_.get() == expected);
    auto failure = [&] { auto mutation = protocol.inbound_gate_.BeginFailureMutation(); return mutation.epoch(); }();
    uint32_t reopened = 0;
    expected->on_destroy = [&] {
        assert(!protocol.inbound_gate_.CurrentThreadHasLease());
        reopened = protocol.inbound_gate_.BeginConnection();
        assert(protocol.Publish(reopened, std::make_unique<WebSocket>()));
    };
    protocol.DetachAndResetWebsocket(failure, true);
    assert(protocol.websocket_ && protocol.websocket_connection_epoch_ == reopened);
    assert(protocol.notifications == 0);
    auto last_failure = [&] { auto mutation = protocol.inbound_gate_.BeginFailureMutation(); return mutation.epoch(); }();
    protocol.DetachAndResetWebsocket(last_failure, true);
    assert(!protocol.websocket_ && protocol.websocket_connection_epoch_ == 0);
    assert(protocol.notifications == 1);
}
