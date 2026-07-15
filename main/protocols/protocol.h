#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cJSON.h>
#include <atomic>
#include <string>
#include <functional>
#include <chrono>
#include <vector>

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    uint32_t generation = 0;   // response/turn generation, stamped at intake for barge-in gen-gating
    std::vector<uint8_t> payload;
};

struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;          // Message type (0: OPUS, 1: JSON)
    uint32_t reserved;      // Reserved for future use
    uint32_t timestamp;     // Timestamp in milliseconds (used for server-side AEC)
    uint32_t payload_size;  // Payload size in bytes
    uint8_t payload[];      // Payload data
} __attribute__((packed));

struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

enum AbortReason {
    kAbortReasonNone,
    kAbortReasonWakeWordDetected
};

enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime // 需要 AEC 支持
};

// Validate a server-advertised (sample_rate, frame_duration) pair from the
// server hello before it is trusted downstream. A hostile/buggy server could
// send a negative or zero sample_rate, which propagates into
// AudioService::SetDecodeSampleRate where decoder_frame_size_ = rate/1000*dur
// goes <= 0 and then vector::resize(decoder_frame_size_) either OOMs (huge
// unsigned cast) or crashes. Restrict to the Opus-legal set the firmware
// actually supports; reject everything else so the caller keeps the safe
// default. Free function (not a member) so both transport implementations and
// any future caller share one source of truth.
inline bool IsValidOpusSampleRate(int sample_rate) {
    return sample_rate == 8000 || sample_rate == 16000 ||
           sample_rate == 24000 || sample_rate == 48000;
}
inline bool IsValidOpusFrameDuration(int frame_duration) {
    return frame_duration == 10 || frame_duration == 20 ||
           frame_duration == 40 || frame_duration == 60;
}

class Protocol {
public:
    virtual ~Protocol() = default;

    inline int server_sample_rate() const {
        return server_sample_rate_;
    }
    inline int server_frame_duration() const {
        return server_frame_duration_;
    }
    inline const std::string& session_id() const {
        return session_id_;
    }

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    // Maintains a passive WebSocket without starting voice or HTTP heartbeat
    // intent. Non-WebSocket transports have no passive liveness work.
    virtual bool MaintainPassiveLiveness() { return true; }
    virtual void CompleteDeferredClose(uint32_t connection_epoch) {
        (void)connection_epoch;
    }
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    virtual void SendWakeWordDetected(const std::string& wake_word);
    virtual void SendStartListening(ListeningMode mode);
    virtual void SendStopListening();
    virtual void SendAbortSpeaking(AbortReason reason);
    virtual void SendMcpMessage(const std::string& message);
    // US-006 Slice-01: send a pre-built lesson_* control frame (a complete envelope)
    // over the realtime channel. Additive PUBLIC sender — SendText is protected.
    // Inherited unchanged by both the WebSocket and MQTT transports.
    bool SendLessonFrame(const std::string& frame);

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    std::atomic<bool> error_occurred_{false};
    std::string session_id_;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    virtual bool SendText(const std::string& text) = 0;
    virtual void SetError(const std::string& message);
    virtual bool IsTimeout() const;
};

#endif // PROTOCOL_H
