#include "audio/audio_decode_fence.h"
#include "audio/chat_playback_reset.h"
#include "audio/audio_playback_drain_snapshot.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

struct FakeCodec {
    AudioOutputDrainSnapshot snapshot;
    mutable unsigned calls = 0;
    AudioOutputDrainSnapshot GetOutputDrainSnapshot() const {
        ++calls;
        return snapshot;
    }
};

// Only the service's dependencies are substituted; the method is extracted
// verbatim from audio_service.cc and executes with the production state types.
class AudioService {
public:
    bool TryGetPlaybackDrainSnapshot(PlaybackDrainSnapshot& out);
    std::mutex audio_queue_mutex_;
    std::deque<int> audio_decode_queue_;
    std::deque<int> audio_playback_queue_;
    AudioDecodeFence audio_decode_fence_;
    ChatPlaybackReset chat_playback_reset_;
    bool audio_playback_in_flight_ = false;
    std::atomic<bool> service_stopped_{false};
    std::atomic<uint32_t> playback_generation_{0};
    FakeCodec* codec_ = nullptr;
};

// PRODUCTION_SNAPSHOT

int main() {
    AudioService service;
    FakeCodec codec{{77, AudioOutputDrainState::Drained}};
    service.codec_ = &codec;
    PlaybackDrainSnapshot out;
    out.reset_epoch = 99;
    out.playback_generation = 42;
    out.stopped = true;
    out.decode_queue_size = 13;
    out.playback_queue_size = 17;
    out.decode_in_flight = true;
    out.output_in_flight = true;
    out.codec = {88, AudioOutputDrainState::Failed};
    unsigned char original[sizeof(out)];
    std::memcpy(original, &out, sizeof(out));

    // A real contender must finish while the owner still holds the mutex.
    std::unique_lock<std::mutex> owner(service.audio_queue_mutex_);
    std::promise<bool> completion;
    auto result = completion.get_future();
    std::thread contender([&] {
        completion.set_value(service.TryGetPlaybackDrainSnapshot(out));
    });
    assert(result.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready);
    assert(!result.get());
    contender.join();
    assert(std::memcmp(original, &out, sizeof(out)) == 0);
    assert(codec.calls == 0);
    owner.unlock();

    service.playback_generation_.store(1234);
    assert(service.TryGetPlaybackDrainSnapshot(out));
    assert(out.playback_generation == 1234 && out.reset_epoch == 0);
    assert(!out.stopped && !out.decode_in_flight && !out.output_in_flight);
    assert(out.decode_queue_size == 0 && out.playback_queue_size == 0);
    assert(out.codec.state == AudioOutputDrainState::Drained && out.codec.epoch == 77);
    auto chat_reset = service.chat_playback_reset_.Request();
    std::memcpy(original, &out, sizeof(out));
    assert(!service.TryGetPlaybackDrainSnapshot(out));
    assert(std::memcmp(original, &out, sizeof(out)) == 0);
    assert(service.chat_playback_reset_.Complete(chat_reset));

    uint64_t decode_epoch;
    {
        std::lock_guard<std::mutex> lock(service.audio_queue_mutex_);
        service.audio_decode_queue_.push_back(1);
        service.audio_decode_queue_.pop_front();
        decode_epoch = service.audio_decode_fence_.Begin();
    }
    assert(service.TryGetPlaybackDrainSnapshot(out));
    assert(out.decode_queue_size == 0 && out.decode_in_flight);
    {
        std::lock_guard<std::mutex> lock(service.audio_queue_mutex_);
        service.audio_decode_fence_.Reset();
    }
    assert(service.TryGetPlaybackDrainSnapshot(out));
    assert(out.reset_epoch == decode_epoch + 1 && out.decode_in_flight);
    {
        std::lock_guard<std::mutex> lock(service.audio_queue_mutex_);
        assert(!service.audio_decode_fence_.Complete(decode_epoch, 1234, 1234));
        service.audio_decode_queue_ = {1, 2};
        service.audio_playback_queue_ = {3, 4, 5};
        service.audio_playback_in_flight_ = true;
    }
    for (auto state : {AudioOutputDrainState::Unsupported, AudioOutputDrainState::Busy,
                       AudioOutputDrainState::Pending, AudioOutputDrainState::Failed,
                       AudioOutputDrainState::Drained}) {
        codec.snapshot = {91, state};
        assert(service.TryGetPlaybackDrainSnapshot(out));
        assert(out.decode_queue_size == 2 && out.playback_queue_size == 3);
        assert(!out.decode_in_flight && out.output_in_flight);
        assert(out.codec.epoch == 91 && out.codec.state == state);
    }
    service.codec_ = nullptr;
    service.service_stopped_.store(true);
    service.playback_generation_.store(5678);
    assert(service.TryGetPlaybackDrainSnapshot(out));
    assert(out.stopped && out.playback_generation == 5678);
    assert(out.codec.state == AudioOutputDrainState::Unsupported && out.codec.epoch == 0);
    assert(out.output_in_flight && out.decode_queue_size == 2 && out.playback_queue_size == 3);
}
