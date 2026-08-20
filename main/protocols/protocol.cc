#include "protocol.h"

#include <esp_log.h>

#define TAG "Protocol"

void Protocol::OnIncomingJson(
    std::function<void(const cJSON* root, std::uint64_t transport_epoch)> callback
) {
    on_incoming_json_ = callback;
}

void Protocol::SetIncomingJsonTransportEpoch(std::uint64_t transport_epoch) {
    incoming_json_transport_epoch_.store(transport_epoch, std::memory_order_release);
}

std::uint64_t Protocol::IncomingJsonTransportEpoch() const {
    return incoming_json_transport_epoch_.load(std::memory_order_acquire);
}

void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

void Protocol::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ + 
                      "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}";
    bool sent = SendText(json);
    ESP_LOGI(TAG, "SendWakeWordDetected sent=%d", sent ? 1 : 0);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    bool sent = SendText(message);
    ESP_LOGI(TAG, "SendStartListening mode=%d sent=%d", static_cast<int>(mode), sent ? 1 : 0);
}

void Protocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

void Protocol::SendTtsDrainAck(const std::string& drain_id) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return;
    cJSON_AddStringToObject(root, "type", "tts_ack");
    cJSON_AddStringToObject(root, "state", "stop");
    cJSON_AddStringToObject(root, "drainId", drain_id.c_str());
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    char* encoded = cJSON_PrintUnformatted(root);
    if (encoded != nullptr) {
        SendText(encoded);
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
}

bool Protocol::SendLessonFrame(const std::string& frame) {
    // US-006 Slice-01: the lesson frame is already a complete envelope (built by
    // lesson_handler.cc); send it verbatim. Additive — does not touch the voice/MCP
    // send paths; reuses the existing protected SendText.
    cJSON* root = cJSON_Parse(frame.c_str());
    const cJSON* type = root ? cJSON_GetObjectItem(root, "type") : nullptr;
    const cJSON* sequence = root ? cJSON_GetObjectItem(root, "sequence") : nullptr;
    bool sent = SendText(frame);
    ESP_LOGI(TAG, "send lesson frame type=%s seq=%d bytes=%u sent=%d",
             cJSON_IsString(type) ? type->valuestring : "(parse-failed)",
             cJSON_IsNumber(sequence) ? sequence->valueint : -1,
             (unsigned)frame.size(),
             sent ? 1 : 0);
    if (root) cJSON_Delete(root);
    return sent;
}

bool Protocol::IsTimeout() const {
    // WSS-3: server keeps WebSocket sessions open for 61 minutes. Keep the
    // firmware threshold aligned so long lessons/conversations do not self-drop.
    const int kTimeoutSeconds = 61 * 60;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
