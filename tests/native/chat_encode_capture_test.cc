#include "audio/chat_uplink_authorization.h"
#include <cassert>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int OPUS_FRAME_DURATION_MS = 60, ESP_AUDIO_ERR_OK = 0;
enum AudioTaskType { kAudioTaskTypeEncodeToSendQueue, kAudioTaskTypeEncodeToTestingQueue };
struct AudioTask { AudioTaskType type; std::vector<int16_t> pcm; uint32_t timestamp = 0; ChatCaptureTag capture_tag; };
struct AudioStreamPacket {
    int frame_duration = 0, sample_rate = 0;
    uint32_t timestamp = 0;
    ChatCaptureTag capture_tag;
    std::vector<uint8_t> payload;
};
struct esp_audio_enc_in_frame_t { uint8_t* buffer; uint32_t len; };
struct esp_audio_enc_out_frame_t { uint8_t* buffer; uint32_t len; uint32_t encoded_bytes; };
std::function<void()> encode_hook;
int esp_opus_enc_process(void*, esp_audio_enc_in_frame_t*, esp_audio_enc_out_frame_t* out) {
    if (encode_hook) encode_hook();
    out->buffer[0] = 42;
    out->encoded_bytes = 1;
    return ESP_AUDIO_ERR_OK;
}
class AudioService {
public:
    ChatUplinkAuthorization chat_uplink_authorization_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_, audio_testing_queue_;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    struct { std::function<void()> on_send_queue_available; } callbacks_;
    struct { int encode_count = 0; } debug_statistics_;
    size_t encoder_frame_size_ = 2;
    int encoder_outbuf_size_ = 10;
    void* opus_encoder_ = this;
    void EncodeOne() {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        // PRODUCTION_ENCODE_BLOCK
    }
    void Queue(ChatCaptureTag tag, AudioTaskType type = kAudioTaskTypeEncodeToSendQueue) {
        auto task = std::make_unique<AudioTask>();
        task->type = type; task->capture_tag = tag; task->pcm = {1, 1};
        audio_encode_queue_.push_back(std::move(task));
    }
};
int main() {
    AudioService service;
    auto a = service.chat_uplink_authorization_.Revoke();
    service.chat_uplink_authorization_.AcknowledgePrepared(a, true);
    assert(service.chat_uplink_authorization_.Arm(a));
    auto tag_a = service.chat_uplink_authorization_.Capture();
    service.Queue(tag_a);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    encode_hook = [&] { entered.set_value(); released.wait(); };
    auto encoding = std::async(std::launch::async, [&] { service.EncodeOne(); });
    entered.get_future().wait();
    auto b = service.chat_uplink_authorization_.Revoke();
    service.chat_uplink_authorization_.AcknowledgePrepared(b, true);
    assert(service.chat_uplink_authorization_.Arm(b));
    release.set_value(); encoding.get();
    assert(service.audio_send_queue_.empty());
    encode_hook = {};
    auto tag_b = service.chat_uplink_authorization_.Capture();
    service.Queue(tag_b); service.EncodeOne();
    assert(service.audio_send_queue_.size() == 1);
    assert(service.audio_send_queue_.front()->capture_tag == tag_b);
    service.chat_uplink_authorization_.Revoke();
    assert(!service.chat_uplink_authorization_.IsCurrentChat(service.audio_send_queue_.front()->capture_tag));
    service.Queue({}, kAudioTaskTypeEncodeToTestingQueue); service.EncodeOne();
    assert(service.audio_testing_queue_.size() == 1);
}
