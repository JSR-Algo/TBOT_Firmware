#include "audio/chat_playback_reset.h"
#include "audio/audio_output_timing.h"
#include "audio/audio_output_drain.h"
#include "lesson_audio_playout.h"
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
    std::function<void()> output_hook;
    std::atomic<unsigned> writes{0};
    bool enabled=false;
    AudioOutputDrainState drain_state=AudioOutputDrainState::Drained;
    bool output_enabled() { return enabled; }
    int output_volume() { return 65; }
    void EnableOutput(bool value) { enabled=value; if (enable_hook) enable_hook(); }
    void OutputData(std::vector<int16_t>&) { ++writes; if (output_hook) output_hook(); }
    AudioOutputDrainSnapshot GetOutputDrainSnapshot() { return {1,drain_state}; }
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
    struct {
        std::function<void(uint32_t,bool,uint32_t)> on_output_completed;
        std::function<void(uint32_t)> on_playback_failed;
    } callbacks_;
    std::chrono::steady_clock::time_point last_output_time_;
    void AudioOutputTask();
};
// PRODUCTION_OUTPUT
int main() {
    for (int scenario : {0,1,2,3,4,5,6,7,8,9,10,11}) {
        const bool tagged=scenario != 1;
        const bool invalidate=scenario < 2 || scenario == 3;
        AudioService service;
        LessonAudioPlayout playout;
        assert(playout.Begin(1,"0123456789abcdef0000000000000001",0));
        unsigned failures=0,starts=0;
        service.callbacks_.on_playback_failed=[&](uint32_t generation) {
            assert(generation==1); ++failures;
            playout.PublishFailure(generation);
        };
        service.callbacks_.on_output_completed=[&](uint32_t generation,bool conversation,uint32_t now_ms) {
            playout.PublishOutput(generation,conversation,now_ms);
            if (playout.TakeStart()) ++starts;
        };
        auto a=service.chat_playback_reset_.Request(); service.chat_playback_reset_.Complete(a);
        auto task=std::make_unique<AudioTask>(); task->chat_reset_token=tagged ? a : 0;
        if (scenario == 3) { task->conversation_audio=false; task->chat_reset_token=0; }
        if (scenario == 4) service.codec.drain_state=AudioOutputDrainState::Failed;
        if (scenario == 5) service.codec.drain_state=AudioOutputDrainState::Unsupported;
        if (scenario == 6) service.codec.drain_state=AudioOutputDrainState::Pending;
        if (scenario == 7 || scenario == 11) service.codec.output_hook=[&] {
            service.playback_generation_=2; service.chat_playback_reset_.Request();
            if (scenario == 11) service.codec.drain_state=AudioOutputDrainState::Failed;
        };
        if (scenario == 10) service.codec.output_hook=[&] {
            if (service.codec.writes == 2) service.codec.drain_state=AudioOutputDrainState::Failed;
        };
        if (scenario == 8) task->pcm.clear();
        task->timestamp=123;
        service.audio_playback_queue_.push_back(std::move(task));
        if (scenario == 9 || scenario == 10) {
            auto second=std::make_unique<AudioTask>(); second->chat_reset_token=a;
            second->timestamp=123;
            service.audio_playback_queue_.push_back(std::move(second));
        }
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
        assert(service.audio_queue_cv_.wait_for(lock,std::chrono::seconds(2),[&]{
            return !service.audio_playback_in_flight_ && service.audio_playback_queue_.empty();}));
        service.service_stopped_=true; service.audio_queue_cv_.notify_all(); lock.unlock();
        worker.get();
        const bool expected_write=!invalidate || scenario == 3;
        const unsigned expected_count=expected_write ? (scenario == 9 || scenario == 10 ? 2U : 1U) : 0U;
        assert(service.codec.writes==expected_count);
        const bool failed=scenario == 4 || scenario == 5 || scenario == 10;
        assert(failures==(failed ? 1U : 0U));
        assert(playout.Failed()==failed);
        const bool expected_start=scenario == 2 || scenario == 6 || scenario == 9 || scenario == 10;
        assert(starts==(expected_start ? 1U : 0U));
        playout.PublishFailure(2);
        assert(playout.Failed()==failed);
        playout.Cancel();
        assert(!playout.Failed());
        assert(playout.Begin(2,"0123456789abcdef0000000000000002",1));
        playout.PublishFailure(1);
        assert(!playout.Failed());
#if CONFIG_USE_SERVER_AEC
        assert(service.timestamp_queue_.size()==expected_count);
        if (expected_write) assert(service.timestamp_queue_.front()==123);
#endif
    }
}
