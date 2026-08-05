#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
#include "lesson_handler.h"
#endif
#include "json_payload_safety.h"
#include "esp_build_identity.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#include <cstdio>
#include <inttypes.h>
#include <memory>
#include <esp_random.h>
#include <esp_timer.h>

#define TAG "WS"

namespace {
struct ServerHelloSignal {
    ServerHelloSignal() {
        handle = xEventGroupCreate();
    }

    ~ServerHelloSignal() {
        if (handle != nullptr) {
            vEventGroupDelete(handle);
        }
    }

    EventGroupHandle_t handle = nullptr;
};
}  // namespace

static bool IsUrlUnreserved(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
}

static std::string UrlEncodeQueryValue(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char ch : value) {
        if (IsUrlUnreserved(static_cast<char>(ch))) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }
    return encoded;
}

static void AppendWebsocketQueryParam(std::string& url,
                                      const std::string& name,
                                      const std::string& value) {
    url.push_back(url.find('?') == std::string::npos ? '?' : '&');
    url += name;
    url.push_back('=');
    url += UrlEncodeQueryValue(value);
}

static std::string NewTraceParentHeader() {
    uint32_t trace0 = esp_random() | 0x1;
    uint32_t trace1 = esp_random();
    uint32_t trace2 = esp_random();
    uint32_t trace3 = esp_random();
    uint32_t span0 = esp_random() | 0x1;
    uint32_t span1 = esp_random();
    char buffer[56];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "00-%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "-%08" PRIx32 "%08" PRIx32 "-01",
        trace0,
        trace1,
        trace2,
        trace3,
        span0,
        span1
    );
    return std::string(buffer);
}

void WebsocketProtocol::RefreshSettings() {
    Settings settings("websocket", false);
    url_ = settings.GetString("url", CONFIG_WEBSOCKET_URL);
    token_ = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
}

bool WebsocketProtocol::IsAllowedUnclaimedPublicLessonMessage(const cJSON* root) const {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "mcp") != 0) {
        return false;
    }
    const cJSON* payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsObject(payload)) {
        return false;
    }
    const cJSON* jsonrpc = cJSON_GetObjectItem(payload, "jsonrpc");
    const cJSON* id = cJSON_GetObjectItem(payload, "id");
    const cJSON* method = cJSON_GetObjectItem(payload, "method");
    const cJSON* params = cJSON_GetObjectItem(payload, "params");
    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0 ||
        !cJSON_IsNumber(id) ||
        !cJSON_IsString(method) || strcmp(method->valuestring, "tools/call") != 0 ||
        !cJSON_IsObject(params)) {
        return false;
    }
    const cJSON* name = cJSON_GetObjectItem(params, "name");
    const cJSON* arguments = cJSON_GetObjectItem(params, "arguments");
    return cJSON_IsString(name) &&
           strcmp(name->valuestring, "self.lesson_assets.sync_to_sd") == 0 &&
           cJSON_IsObject(arguments);
#else
    (void)root;
    return false;
#endif
}

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
    RefreshSettings();
}

void WebsocketProtocol::SetUnclaimedPublicLessonOnly(bool enabled) {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    unclaimed_public_lesson_only_ = enabled;
#else
    (void)enabled;
    unclaimed_public_lesson_only_ = false;
#endif
}

WebsocketProtocol::~WebsocketProtocol() {
    {
        auto failure_mutation = inbound_gate_.BeginFailureMutation();
    }
    DetachAndResetWebsocket();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Warm the websocket session during activation so the first wake word does
    // not block in the cold TLS/WebSocket handshake path.
    ESP_LOGI(TAG, "Preconnecting websocket audio channel");
    RefreshSettings();
    bool opened = OpenAudioChannel();
    if (!opened) {
        ESP_LOGW(TAG, "Websocket preconnect failed; wake word will retry on demand");
    }
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened()) {
        ESP_LOGW(TAG, "Websocket SendAudio unavailable websocket=%d connected=%d payload_bytes=%u version=%d",
                 websocket_ != nullptr ? 1 : 0,
                 (websocket_ != nullptr && websocket_->IsConnected()) ? 1 : 0,
                 packet ? static_cast<unsigned>(packet->payload.size()) : 0,
                 version_);
        return false;
    }

    const size_t payload_size = packet->payload.size();
    bool sent = false;
    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        sent = websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        sent = websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        sent = websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
    if (sent) {
        last_incoming_time_ = std::chrono::steady_clock::now();
    }
    static uint32_t send_audio_count = 0;
    send_audio_count++;
    if (!sent || send_audio_count == 1 || send_audio_count % 25 == 0) {
        ESP_LOGI(TAG, "Websocket SendAudio count=%lu sent=%d payload_bytes=%u version=%d",
                 static_cast<unsigned long>(send_audio_count), sent ? 1 : 0,
                 static_cast<unsigned>(payload_size), version_);
    }
    return sent;
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (!IsAudioChannelOpened()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text frame bytes=%u", (unsigned)text.size());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

bool WebsocketProtocol::MaintainPassiveLiveness() {
    if (!IsAudioChannelOpened()) {
        return false;
    }

    const auto action = passive_liveness_.Poll(
        static_cast<uint32_t>(esp_timer_get_time() / 1000));
    if (action == PassiveWebsocketLiveness::Action::kTimedOut) {
        ESP_LOGW(TAG, "passive_ws_pong_timeout");
        inbound_gate_.FailCurrent();
        error_occurred_ = true;
        return false;
    }
    if (action == PassiveWebsocketLiveness::Action::kSendPing) {
        const bool sent = websocket_->Send("{\"type\":\"ping\"}");
        if (!sent) {
            inbound_gate_.FailCurrent();
            error_occurred_ = true;
        }
        ESP_LOGD(TAG, "passive_ws_ping sent=%d", sent ? 1 : 0);
        return sent;
    }
    return true;
}

void WebsocketProtocol::SetError(const std::string& message) {
    inbound_gate_.FailCurrent();
    Protocol::SetError(message);
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    if (inbound_gate_.CurrentThreadHasLease()) {
        // Never destroy the callback object from its own stack. Mark it stale;
        // the next replacement or protocol destruction reclaims it safely.
        inbound_gate_.FailCurrent();
        error_occurred_ = true;
        const uint32_t connection_epoch = inbound_gate_.CurrentEpoch();
        if (close_state_.MarkDeferred(connection_epoch)) {
            Application::GetInstance().ScheduleDeferredProtocolClose(this, connection_epoch);
        }
        return;
    }
    CompleteCloseAndNotify();
}

void WebsocketProtocol::CompleteDeferredClose(uint32_t connection_epoch) {
    {
        auto failure_mutation =
            inbound_gate_.BeginFailureMutationIfCurrent(connection_epoch);
        if (!failure_mutation.Matched()) {
            return;
        }
        if (!close_state_.TakeDeferred(connection_epoch)) {
            return;
        }
        error_occurred_ = true;
    }
    DetachAndResetWebsocket();
    NotifyAudioChannelClosedOnce();
}

void WebsocketProtocol::CompleteCloseAndNotify() {
    {
        auto failure_mutation = inbound_gate_.BeginFailureMutation();
        error_occurred_ = true;
    }
    DetachAndResetWebsocket();
    NotifyAudioChannelClosedOnce();
}

void WebsocketProtocol::DetachAndResetWebsocket() {
    if (websocket_ == nullptr) {
        return;
    }
    websocket_.reset();
}

void WebsocketProtocol::NotifyAudioChannelClosedOnce() {
    if (close_state_.TakeNotification() && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    if (inbound_gate_.CurrentThreadHasLease()) {
        ESP_LOGE(TAG, "websocket replacement rejected from callback context");
        return false;
    }
    RefreshSettings();
    std::string url = url_;
    std::string token = token_;
    session_mode_ = unclaimed_public_lesson_only_
        ? WebsocketSessionMode::kUnclaimedPublicLesson
        : WebsocketSessionMode::kAuthenticatedRealtime;
    if (session_mode_ == WebsocketSessionMode::kUnclaimedPublicLesson) {
        token.clear();
    }
    const std::string device_id = SystemInfo::GetMacAddress();
    const std::string client_id = Board::GetInstance().GetUuid();

    last_incoming_time_ = std::chrono::steady_clock::now();
    const std::uint64_t callback_transport_epoch = IncomingJsonTransportEpoch();

    auto network = Board::GetInstance().GetNetwork();
    auto replacement_websocket = network->CreateWebSocket(1);
    if (replacement_websocket == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
    }
    replacement_websocket->SetHeader("protocol-version", std::to_string(version_).c_str());
    EspBuildIdentity build_identity;
    std::string build_identity_error;
    if (ReadRunningEspBuildIdentity(&build_identity, &build_identity_error)) {
        for (const auto& header : EspBuildIdentityHeaders(build_identity)) {
            replacement_websocket->SetHeader(header.first.c_str(), header.second.c_str());
        }
    } else {
        ESP_LOGW(TAG, "Build identity unavailable reason=%s", build_identity_error.c_str());
    }
    std::string traceparent = NewTraceParentHeader();
    replacement_websocket->SetHeader("traceparent", traceparent.c_str());
    if (!token.empty()) {
        replacement_websocket->SetHeader("authorization", token.c_str());
    }

    std::string connect_url = url;
    AppendWebsocketQueryParam(connect_url, "device-id", device_id);
    AppendWebsocketQueryParam(connect_url, "client-id", client_id);
    ESP_LOGI(TAG, "Websocket auth identity: device_id_empty=%d client_id_empty=%d token_empty=%d",
             device_id.empty(), client_id.empty(), token.empty());

    auto hello_signal = std::make_shared<ServerHelloSignal>();
    if (hello_signal->handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket hello synchronization");
        return false;
    }

    uint32_t connection_epoch = 0;
    {
        auto connection_mutation = inbound_gate_.BeginConnectionMutation();
        connection_epoch = connection_mutation.epoch();
        error_occurred_ = false;
        close_state_.ResetForConnection();
    }

    WebSocket* candidate_websocket = replacement_websocket.get();
    candidate_websocket->OnData([this, connection_epoch, callback_transport_epoch, hello_signal](const char* data, size_t len, bool binary) {
        auto inbound_lease = inbound_gate_.Acquire(connection_epoch);
        if (!inbound_lease || error_occurred_) {
            ESP_LOGD(TAG, "ws_stale_inbound_dropped");
            return;
        }
        if (binary) {
            if (session_mode_ == WebsocketSessionMode::kUnclaimedPublicLesson) {
                ESP_LOGW(TAG, "unclaimed_public_ws_binary_frame_rejected");
                return;
            }
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    // Bounds-check the server-supplied frame before any deref: the
                    // header must fit, and payload_size must not exceed the bytes that
                    // actually arrived — else the vector copy over-reads the heap
                    // (deep-audit: payload_size is attacker/server-controlled).
                    if (len < sizeof(BinaryProtocol2)) {
                        ESP_LOGE(TAG, "binary v2 frame too short: %u", (unsigned)len);
                        return;
                    }
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    size_t bp2_avail = len - sizeof(BinaryProtocol2);
                    if (bp2->payload_size > bp2_avail) {
                        ESP_LOGE(TAG, "binary v2 payload_size %u > avail %u; dropping",
                                 (unsigned)bp2->payload_size, (unsigned)bp2_avail);
                        return;
                    }
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    if (len < sizeof(BinaryProtocol3)) {
                        ESP_LOGE(TAG, "binary v3 frame too short: %u", (unsigned)len);
                        return;
                    }
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    size_t bp3_avail = len - sizeof(BinaryProtocol3);
                    if (bp3->payload_size > bp3_avail) {
                        ESP_LOGE(TAG, "binary v3 payload_size %u > avail %u; dropping",
                                 (unsigned)bp3->payload_size, (unsigned)bp3_avail);
                        return;
                    }
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // Parse JSON data
            if (JsonHasForbiddenDecodedNull(data, len)) {
                ESP_LOGW(TAG, "Rejected JSON message containing decoded NUL");
                return;
            }
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    if (ParseServerHello(root)) {
                        xEventGroupSetBits(
                            hello_signal->handle,
                            WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
                    }
                } else if (strcmp(type->valuestring, "pong") == 0) {
                    passive_liveness_.OnPong(
                        static_cast<uint32_t>(esp_timer_get_time() / 1000));
                    ESP_LOGD(TAG, "passive_ws_pong_received");
                } else {
                    if (session_mode_ == WebsocketSessionMode::kUnclaimedPublicLesson) {
                        if (!IsAllowedUnclaimedPublicLessonMessage(root)) {
                            ESP_LOGW(TAG, "unclaimed_public_ws_frame_rejected");
                            cJSON_Delete(root);
                            return;
                        }
                        if (on_incoming_json_ != nullptr) {
                            on_incoming_json_(root, callback_transport_epoch);
                        }
                        cJSON_Delete(root);
                        return;
                    }
                    if (strncmp(type->valuestring, "lesson_", 7) == 0) {
                        auto sequence = cJSON_GetObjectItem(root, "sequence");
                        ESP_LOGI(TAG, "ws text lesson frame type=%s seq=%d bytes=%u",
                                 type->valuestring,
                                 cJSON_IsNumber(sequence) ? sequence->valueint : -1,
                                 (unsigned)len);
                    }
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root, callback_transport_epoch);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    candidate_websocket->OnDisconnected([this, connection_epoch, candidate_websocket]() {
        auto disconnect_lease = inbound_gate_.Acquire(connection_epoch);
        // A replaced socket may synchronously invoke this callback from its
        // destructor. Only the current epoch may dereference websocket_.
        const bool current_connection = disconnect_lease.IsCurrentEpoch();
        if (!current_connection) {
            ESP_LOGD(TAG, "stale_ws_disconnect_dropped");
            return;
        }
        int err_code = candidate_websocket != nullptr ? candidate_websocket->GetLastError() : -1;
        ESP_LOGW(TAG, "ws_disconnect err_code=%d idle_timeout=%d",
                 err_code, IsTimeout() ? 1 : 0);
        NotifyAudioChannelClosedOnce();
    });

    ESP_LOGI(TAG, "Connecting to websocket server with protocol version %d", version_);
    if (!replacement_websocket->Connect(connect_url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", replacement_websocket->GetLastError());
        if (connection_epoch != inbound_gate_.CurrentEpoch()) {
            return false;
        }
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!replacement_websocket->Send(message)) {
        ESP_LOGE(TAG, "Failed to send text frame bytes=%u", (unsigned)message.size());
        if (connection_epoch != inbound_gate_.CurrentEpoch()) {
            return false;
        }
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(hello_signal->handle, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        if (connection_epoch != inbound_gate_.CurrentEpoch()) {
            return false;
        }
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }
    if (connection_epoch != inbound_gate_.CurrentEpoch() || error_occurred_) {
        return false;
    }

    passive_liveness_.OnOpened(
        static_cast<uint32_t>(esp_timer_get_time() / 1000));

    websocket_ = std::move(replacement_websocket);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    // US-006 Slice-01 (D-CAP-FLAG, ADR 0013 §I): advertise lesson-render capability.
    // Absence == no support; the ESP Server MUST NOT send lesson_prepare to firmware
    // that did not advertise this. Purely additive — does not disturb aec/mcp/voice.
    cJSON_AddBoolToObject(features, "lesson", true);
    cJSON_AddStringToObject(features, "renderer", kLessonRendererName);
    AddLessonRendererFeatures(features);
#endif
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

bool WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    // cJSON_IsString guards both the missing-key (null node) and wrong-type
    // (number/null JSON -> valuestring==NULL) cases; strcmp(NULL, ...) faulted before
    // (deep-audit). Don't log the raw valuestring (it may be NULL).
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported or invalid transport in server hello");
        return false;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        // Validate before trusting: an out-of-range sample_rate flows into
        // AudioService::SetDecodeSampleRate -> decoder_frame_size_ (<=0) ->
        // vector::resize OOM/crash. Reject anything outside the Opus-legal set
        // and keep the safe default. (deep-audit: server-controlled decoder cfg)
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            if (IsValidOpusSampleRate(sample_rate->valueint)) {
                server_sample_rate_ = sample_rate->valueint;
            } else {
                ESP_LOGE(TAG, "Invalid sample_rate %d in server hello; keeping %d",
                         sample_rate->valueint, server_sample_rate_);
            }
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            if (IsValidOpusFrameDuration(frame_duration->valueint)) {
                server_frame_duration_ = frame_duration->valueint;
            } else {
                ESP_LOGE(TAG, "Invalid frame_duration %d in server hello; keeping %d",
                         frame_duration->valueint, server_frame_duration_);
            }
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
    return true;
}
