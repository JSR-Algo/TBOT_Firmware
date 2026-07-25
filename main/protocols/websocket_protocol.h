#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"
#include "passive_websocket_liveness.h"
#include "connection_inbound_gate.h"
#include "connection_close_state.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

enum class WebsocketSessionMode {
    kAuthenticatedRealtime,
    kUnclaimedPublicLesson,
};

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool MaintainPassiveLiveness() override;
    void CompleteDeferredClose(uint32_t connection_epoch) override;

private:
    EventGroupHandle_t event_group_handle_;
    // Must outlive websocket_: socket destruction can synchronously invoke a
    // callback that acquires the lifecycle gate.
    ConnectionInboundGate inbound_gate_;
    std::unique_ptr<WebSocket> websocket_;
    std::string url_;
    std::string token_;
    int version_ = 1;
    WebsocketSessionMode session_mode_ = WebsocketSessionMode::kUnclaimedPublicLesson;
    PassiveWebsocketLiveness passive_liveness_;
    ConnectionCloseState close_state_;

    void RefreshSettings();
    void ParseServerHello(const cJSON* root);
    bool IsAllowedUnclaimedPublicLessonMessage(const cJSON* root) const;
    bool SendText(const std::string& text) override;
    void SetError(const std::string& message) override;
    void DetachAndResetWebsocket();
    void NotifyAudioChannelClosedOnce();
    void CompleteCloseAndNotify();
    std::string GetHelloMessage();
};

#endif
