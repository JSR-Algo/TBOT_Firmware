#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#include "lesson_handler.h"  // US-006 Slice-01: kLessonRendererName (D-CAP-FLAG)

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

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

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Warm the websocket session during activation so the first wake word does
    // not block in the cold TLS/WebSocket handshake path.
    ESP_LOGI(TAG, "Preconnecting websocket audio channel");
    bool opened = OpenAudioChannel();
    if (!opened) {
        ESP_LOGW(TAG, "Websocket preconnect failed; wake word will retry on demand");
    }
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
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
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
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

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url", CONFIG_WEBSOCKET_URL);
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
    const std::string device_id = SystemInfo::GetMacAddress();
    const std::string client_id = Board::GetInstance().GetUuid();

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
    }
    websocket_->SetHeader("protocol-version", std::to_string(version_).c_str());

    std::string connect_url = url;
    AppendWebsocketQueryParam(connect_url, "device-id", device_id);
    AppendWebsocketQueryParam(connect_url, "client-id", client_id);
    if (!token.empty()) {
        AppendWebsocketQueryParam(connect_url, "authorization", token);
    }
    ESP_LOGI(TAG, "Websocket auth identity: device-id=%s client-id=%s token_empty=%d",
             device_id.c_str(), client_id.c_str(), token.empty());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
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
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (strncmp(type->valuestring, "lesson_", 7) == 0) {
                        auto sequence = cJSON_GetObjectItem(root, "sequence");
                        ESP_LOGI(TAG, "ws text lesson frame type=%s seq=%d bytes=%u",
                                 type->valuestring,
                                 cJSON_IsNumber(sequence) ? sequence->valueint : -1,
                                 (unsigned)len);
                    }
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        // OBS-1: log WHY the channel dropped (last error code + whether the
        // app-level idle timeout tripped) so reconnect storms are debuggable.
        int err_code = websocket_ != nullptr ? websocket_->GetLastError() : -1;
        ESP_LOGW(TAG, "ws_disconnect err_code=%d idle_timeout=%d", err_code, IsTimeout() ? 1 : 0);
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(connect_url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

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
    // US-006 Slice-01 (D-CAP-FLAG, ADR 0013 §I): advertise lesson-render capability.
    // Absence == no support; the ESP Server MUST NOT send lesson_prepare to firmware
    // that did not advertise this. Purely additive — does not disturb aec/mcp/voice.
    cJSON_AddBoolToObject(features, "lesson", true);
    cJSON_AddStringToObject(features, "renderer", kLessonRendererName);
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

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    // cJSON_IsString guards both the missing-key (null node) and wrong-type
    // (number/null JSON -> valuestring==NULL) cases; strcmp(NULL, ...) faulted before
    // (deep-audit). Don't log the raw valuestring (it may be NULL).
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported or invalid transport in server hello");
        return;
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
}
