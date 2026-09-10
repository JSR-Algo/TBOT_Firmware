#include "audio/chat_playback_reset.h"
#include "audio/audio_output_timing.h"
#include "audio/audio_output_drain.h"
#include <algorithm>
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
template<class... T> void Log(T&&...) {}
#define ESP_LOGD(...) Log(__VA_ARGS__)
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
const char* TAG="test";
constexpr int AUDIO_POWER_CHECK_INTERVAL_MS=1000;
bool ShouldLogAudioDiagnostic(unsigned) { return false; }
void esp_timer_stop(void*) {}
void esp_timer_start_periodic(void*, int) {}
int64_t esp_timer_get_time() { return 100; }
struct AudioTask {
    uint32_t chat_reset_token=0,response_generation=1,timestamp=0;
    bool conversation_audio=true;
    std::vector<int16_t> pcm{1};
};
struct Codec {
    std::function<void()> enable_hook;
    std::atomic<unsigned> writes{0};
    bool output_enabled() { return false; }
    int output_volume() { return 65; }
    void EnableOutput(bool) { if (enable_hook) enable_hook(); }
    void OutputData(std::vector<int16_t>&) { ++writes; }
    AudioOutputDrainSnapshot GetOutputDrainSnapshot() { return {1,AudioOutputDrainState::Drained}; }
};
struct AudioService {
    ChatPlaybackReset chat_playback_reset_;
    std::atomic<uint32_t> playback_generation_{1};
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::atomic<bool> service_stopped_{false};
    bool audio_playback_in_flight_=false;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    std::deque<uint32_t> timestamp_queue_;
    Codec codec;
    Codec* codec_=&codec;
    void* audio_power_timer_=nullptr;
    struct { unsigned playback_count=0; } debug_statistics_;
    struct { std::function<void(uint32_t,bool,uint32_t)> on_output_completed; } callbacks_;
    std::chrono::steady_clock::time_point last_output_time_;
    void AudioOutputTask();
};
// PRODUCTION_OUTPUT
int main() {
    for (int scenario : {0,1,2}) {
        const bool tagged=scenario != 1;
        const bool invalidate=scenario != 2;
        AudioService service;
        auto a=service.chat_playback_reset_.Request(); service.chat_playback_reset_.Complete(a);
        auto task=std::make_unique<AudioTask>(); task->chat_reset_token=tagged ? a : 0;
        task->timestamp=123;
        service.audio_playback_queue_.push_back(std::move(task));
        std::promise<void> entered,release;
        auto released=release.get_future().share();
        service.codec.enable_hook=[&]{entered.set_value();released.wait();};
        auto worker=std::async(std::launch::async,[&]{service.AudioOutputTask();});
        entered.get_future().wait();
        if (invalidate) {
            service.chat_playback_reset_.Request(); service.playback_generation_=2;
        }
        release.set_value();
        std::unique_lock<std::mutex> lock(service.audio_queue_mutex_);
        assert(service.audio_queue_cv_.wait_for(lock,std::chrono::seconds(2),[&]{return !service.audio_playback_in_flight_;}));
        service.service_stopped_=true; service.audio_queue_cv_.notify_all(); lock.unlock();
        worker.get();
        const bool expected_write=!tagged || !invalidate;
        assert(service.codec.writes==(expected_write ? 1U : 0U));
#if CONFIG_USE_SERVER_AEC
        assert(service.timestamp_queue_.size()==(expected_write ? 1U : 0U));
        if (expected_write) assert(service.timestamp_queue_.front()==123);
#endif
    }
}
