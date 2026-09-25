#include "audio/chat_uplink_authorization.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <vector>

constexpr int PROCESSOR_RUNNING = 1, ESP_FAIL = -1, VAD_SPEECH = 1, VAD_SILENCE = 0;
int pdMS_TO_TICKS(int ms) { return ms; }
struct Codec { int input_channels() { return 1; } bool input_reference() { return false; } };
int xEventGroupGetBits(std::atomic<int>* bits) { return bits->load(); }
void xEventGroupClearBits(std::atomic<int>* bits, int mask) { bits->fetch_and(~mask); }
struct Result { int ret_value = 0, vad_state = 0, data_size = 0; int16_t* data = nullptr; };
struct Sdk {
    std::vector<int16_t> ring;
    Result result;
    std::function<void()> fetch_hook;
    int get_feed_chunksize(void*) { return 2; }
    void feed(void*, int16_t* samples) { ring.insert(ring.end(), samples, samples + 2); }
    void reset_buffer(void*) { ring.clear(); }
    Result* fetch_with_delay(void*, int delay) {
        assert(delay == 100);
        if (fetch_hook) fetch_hook();
        if (ring.empty()) return nullptr;
        result.data = ring.data();
        result.data_size = static_cast<int>(ring.size() * sizeof(int16_t));
        return &result;
    }
};
using Output = std::function<void(std::vector<int16_t>&&, ChatCaptureTag)>;
class NoAudioProcessor {
public:
    void Feed(std::vector<int16_t>&& data, ChatCaptureTag tag = {});
    void Stop();
    void PrepareCapture(ChatCaptureTag tag);
    Codec codec;
    Codec* codec_ = &codec;
    int frame_samples_ = 4;
    std::vector<int16_t> output_buffer_;
    Output output_callback_;
    std::atomic<bool> is_running_{true};
    std::mutex buffer_mutex_;
    std::mutex callback_mutex_;
    ChatCaptureTag capture_tag_{};
};
class AfeAudioProcessor {
public:
    void Feed(std::vector<int16_t>&& data, ChatCaptureTag tag = {});
    void Stop();
    void PrepareCapture(ChatCaptureTag tag);
    void FetchAudio();
    bool IsRunning() { return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING; }
    std::vector<int16_t> SelectDominantMonoChannel(const std::vector<int16_t>& data, int) { return data; }
    Codec codec;
    Codec* codec_ = &codec;
    Sdk sdk;
    Sdk* afe_iface_ = &sdk;
    void* afe_data_ = this;
    std::atomic<int> bits{PROCESSOR_RUNNING};
    std::atomic<int>* event_group_ = &bits;
    int frame_samples_ = 4, codec_input_channels_ = 1, afe_feed_channels_ = 1;
    bool is_speaking_ = false;
    std::vector<int16_t> input_buffer_, output_buffer_;
    Output output_callback_;
    std::function<void(bool)> vad_state_change_callback_;
    std::mutex input_buffer_mutex_;
    std::mutex callback_mutex_;
    std::mutex fetch_mutex_;
    ChatCaptureTag capture_tag_{};
};

// PRODUCTION_METHODS

int main() {
    const ChatCaptureTag a{6, true}, b{10, true};
    NoAudioProcessor no;
    std::vector<ChatCaptureTag> tags;
    no.output_callback_ = [&](std::vector<int16_t>&& samples, ChatCaptureTag tag) {
        assert(no.buffer_mutex_.try_lock());
        no.buffer_mutex_.unlock();
        for (auto sample : samples) assert(sample == 2);
        tags.push_back(tag);
    };
    no.PrepareCapture(a);
    no.Feed({1, 1}, a);
    no.PrepareCapture(b);
    no.Feed({1, 1}, a);
    no.Feed({2, 2, 2, 2}, b);
    assert(tags.size() == 1 && tags[0] == b);

    AfeAudioProcessor afe;
    tags.clear();
    afe.output_callback_ = [&](std::vector<int16_t>&& samples, ChatCaptureTag tag) {
        assert(afe.input_buffer_mutex_.try_lock());
        afe.input_buffer_mutex_.unlock();
        for (auto sample : samples) assert(sample == 2);
        tags.push_back(tag);
    };
    afe.PrepareCapture(a);
    afe.Feed({1, 1, 1}, a); // SDK ring and owned partial input.
    afe.FetchAudio(); // Partial output must also be reset.
    afe.PrepareCapture(b);
    afe.Feed({1}, a);
    afe.Feed({2, 2, 2, 2}, b);
    afe.FetchAudio();
    assert(tags.size() == 1 && tags[0] == b);
    afe.sdk.ring.clear();

    std::promise<void> entered, release;
    auto released = release.get_future().share();
    afe.sdk.fetch_hook = [&] { entered.set_value(); released.wait(); };
    auto fetch = std::async(std::launch::async, [&] { afe.FetchAudio(); });
    entered.get_future().wait();
    auto feed = std::async(std::launch::async, [&] { afe.Feed({2, 2}, b); });
    assert(feed.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    feed.get();
    auto reset = std::async(std::launch::async, [&] { afe.PrepareCapture(a); });
    assert(reset.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    release.set_value();
    fetch.get();
    reset.get();

    afe.sdk.fetch_hook = {};
    std::promise<void> callback_entered, callback_release;
    auto callback_released = callback_release.get_future().share();
    afe.output_callback_ = [&](std::vector<int16_t>&&, ChatCaptureTag tag) {
        assert(tag == a);
        callback_entered.set_value();
        callback_released.wait();
    };
    afe.Feed({1, 1, 1, 1}, a);
    auto callback = std::async(std::launch::async, [&] { afe.FetchAudio(); });
    callback_entered.get_future().wait();
    auto prepare = std::async(std::launch::async, [&] { afe.PrepareCapture(b); });
    assert(prepare.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    callback_release.set_value();
    callback.get(); prepare.get();
    assert(afe.capture_tag_ == b && afe.output_buffer_.empty());
}
