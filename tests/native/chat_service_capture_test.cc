#include "audio/chat_uplink_authorization.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

#define ESP_LOGW(...) ((void)0)
constexpr int AUDIO_POWER_CHECK_INTERVAL_MS = 1000, MAX_TIMESTAMPS_IN_QUEUE = 3;
constexpr int MAX_ENCODE_TASKS_IN_QUEUE = 4;
constexpr int AS_EVENT_WAKE_WORD_RUNNING = 2, AS_EVENT_AUDIO_PROCESSOR_RUNNING = 4;
int pdMS_TO_TICKS(int ms) { return ms; }
void vTaskDelay(int) {}
std::function<void()> queue_attempt_hook;
std::function<void()> queue_wait_hook;
void esp_timer_stop(void*) {}
void esp_timer_start_periodic(void*, int) {}
using esp_ae_sample_t = int16_t*;
struct Resampler { int16_t cached = 0; };
void esp_ae_rate_cvt_reset(Resampler* p) { p->cached = 0; }
void esp_ae_rate_cvt_get_max_out_sample_num(Resampler*, uint32_t in, uint32_t* out) { *out = in; }
void esp_ae_rate_cvt_process(Resampler* p, int16_t* in, uint32_t count, int16_t* out, uint32_t*) {
    for (uint32_t n = 0; n < count; ++n) out[n] = in[n];
    if (count) { out[0] += p->cached; p->cached = in[count - 1]; }
}
struct Codec {
    std::function<void()> read_hook;
    bool input_enabled() { return true; }
    void EnableInput(bool) {}
    int input_sample_rate() { return 48000; }
    int input_channels() { return 1; }
    bool InputData(std::vector<int16_t>& data) {
        if (read_hook) read_hook();
        for (auto& sample : data) sample = 2;
        return true;
    }
};
struct Processor {
    ChatCaptureTag tag;
    int feed_count = 0;
    std::function<void()> prepare_hook;
    void PrepareCapture(ChatCaptureTag value) { if (prepare_hook) prepare_hook(); tag = value; }
    void Feed(std::vector<int16_t>&&, ChatCaptureTag value) { ++feed_count; assert(value == tag); }
};
enum AudioTaskType { kAudioTaskTypeEncodeToSendQueue, kAudioTaskTypeEncodeToTestingQueue };
struct AudioTask { AudioTaskType type; ChatCaptureTag capture_tag; std::vector<int16_t> pcm; uint32_t timestamp = 0; };
class AudioService {
public:
    bool ReadAudioData(std::vector<int16_t>&, int, int, const ChatCaptureTag* = nullptr);
    void PushTaskToEncodeQueue(AudioTaskType, std::vector<int16_t>&&, ChatCaptureTag = {});
    bool PrepareChatUplink(uint32_t, bool);
    void InputOnce(int);
    int wake_count = 0;
    void FeedWakeWord(const std::vector<int16_t>&) { ++wake_count; }
    Codec codec;
    Codec* codec_ = &codec;
    Processor processor;
    Processor* audio_processor_ = &processor;
    bool audio_processor_initialized_ = true;
    ChatUplinkAuthorization chat_uplink_authorization_;
    ChatCaptureTag input_resampler_tag_{};
    std::mutex chat_prepare_mutex_, input_resampler_mutex_, audio_queue_mutex_;
    uint32_t chat_prepared_revoked_ = 0;
    bool chat_prepared_scope_ = false;
    std::condition_variable audio_queue_cv_;
    Resampler resampler;
    Resampler* input_resampler_ = &resampler;
    void* audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    struct { int input_count = 0, encode_drop_count = 0; } debug_statistics_;
    std::deque<uint32_t> timestamp_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    bool service_stopped_ = false;
};
// PRODUCTION_METHODS

int main() {
    AudioService duplicate;
    const auto duplicate_token = duplicate.chat_uplink_authorization_.Revoke();
    assert(duplicate.PrepareChatUplink(duplicate_token, true));
    bool prepared_again = false;
    duplicate.processor.prepare_hook = [&] {
        prepared_again = true;
        assert(duplicate.chat_uplink_authorization_.Arm(duplicate_token, true));
    };
    assert(!duplicate.PrepareChatUplink(duplicate_token, false));
    assert(!prepared_again);
    assert(duplicate.PrepareChatUplink(duplicate_token, true));
    assert(!prepared_again);
    assert(duplicate.chat_uplink_authorization_.Arm(duplicate_token, true));

    AudioService stale;
    auto stale_a = stale.chat_uplink_authorization_.Revoke();
    std::promise<void> prepare_entered, prepare_release;
    auto prepare_released = prepare_release.get_future().share();
    std::atomic<bool> first_prepare{true};
    stale.processor.prepare_hook = [&] {
        if (first_prepare.exchange(false)) {
            prepare_entered.set_value();
            prepare_released.wait();
        }
    };
    auto prepare_a = std::async(std::launch::async, [&] { return stale.PrepareChatUplink(stale_a, true); });
    prepare_entered.get_future().wait();
    auto stale_b = stale.chat_uplink_authorization_.Revoke();
    assert(!stale.chat_uplink_authorization_.Arm(stale_b));
    auto prepare_b = std::async(std::launch::async, [&] { return stale.PrepareChatUplink(stale_b, true); });
    prepare_release.set_value();
    assert(!prepare_a.get());
    assert(prepare_b.get());
    assert(!stale.chat_uplink_authorization_.Arm(stale_a));
    assert(stale.chat_uplink_authorization_.Arm(stale_b));
    assert(stale.processor.tag == stale.chat_uplink_authorization_.Capture());

    for (int i = 0; i < MAX_ENCODE_TASKS_IN_QUEUE; ++i)
        stale.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, {1, 1});
    const auto waiting_tag = stale.chat_uplink_authorization_.Capture();
    std::promise<void> wait_entered;
    std::atomic<bool> first_wait{true};
    queue_wait_hook = [&] {
        if (first_wait.exchange(false)) wait_entered.set_value();
    };
    auto waiting = std::async(std::launch::async, [&] {
        stale.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, {1, 1}, waiting_tag);
    });
    wait_entered.get_future().wait();
    {
        // Acquiring the actual queue mutex proves wait_for released it after
        // its false predicate, not merely that the enqueue thread started.
        std::lock_guard<std::mutex> lock(stale.audio_queue_mutex_);
        assert(stale.audio_encode_queue_.size() == MAX_ENCODE_TASKS_IN_QUEUE);
        auto c = stale.chat_uplink_authorization_.Revoke();
        assert(stale.PrepareChatUplink(c, true));
        assert(stale.chat_uplink_authorization_.Arm(c));
        stale.audio_encode_queue_.clear();
    }
    stale.audio_queue_cv_.notify_all();
    waiting.get();
    queue_wait_hook = {};
    assert(stale.audio_encode_queue_.empty());

    AudioService service;
    auto a = service.chat_uplink_authorization_.Revoke();
    assert(service.PrepareChatUplink(a, true));
    assert(service.chat_uplink_authorization_.Arm(a));
    auto tag_a = service.chat_uplink_authorization_.Capture();
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    service.codec.read_hook = [&] { entered.set_value(); released.wait(); };
    auto read = std::async(std::launch::async, [&] {
        std::vector<int16_t> pcm;
        return service.ReadAudioData(pcm, 16000, 2, &tag_a);
    });
    entered.get_future().wait();
    auto b = service.chat_uplink_authorization_.Revoke();
    service.resampler.cached = 99;
    assert(service.PrepareChatUplink(b, true));
    assert(service.chat_uplink_authorization_.Arm(b));
    release.set_value();
    assert(!read.get());
    service.codec.read_hook = {};
    auto tag_b = service.chat_uplink_authorization_.Capture();
    std::vector<int16_t> pcm;
    assert(service.ReadAudioData(pcm, 16000, 2, &tag_b));
    assert(pcm[0] == 2);
    std::unique_lock<std::mutex> queue_owner(service.audio_queue_mutex_);
    std::promise<void> queue_attempt;
    queue_attempt_hook = [&] { queue_attempt.set_value(); };
    auto pending = std::async(std::launch::async, [&] {
        service.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, {2, 2}, tag_b);
    });
    queue_attempt.get_future().wait();
    service.chat_uplink_authorization_.Revoke();
    queue_owner.unlock();
    pending.get();
    queue_attempt_hook = {};
    assert(service.audio_encode_queue_.empty());
    service.InputOnce(AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    assert(service.wake_count == 1 && service.processor.feed_count == 0);
    service.InputOnce(AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    assert(service.wake_count == 1 && service.processor.feed_count == 0);
    service.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, {1, 1});
    assert(service.audio_encode_queue_.size() == 1);
}
