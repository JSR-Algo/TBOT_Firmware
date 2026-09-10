#include "audio/audio_reset_epoch_publication.h"
#include "audio/chat_playback_reset.h"
#include "audio/audio_decode_fence.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

std::atomic<bool> decoder_entered{false};
std::atomic<bool> release_decoder{false};
void esp_opus_dec_reset(void*) {
    decoder_entered.store(true);
    while (!release_decoder.load()) std::this_thread::yield();
}

// Dependencies only are substituted; both service methods below are extracted
// verbatim from production and retain their actual fence/publication types.
class AudioService {
public:
    bool TryGetPlaybackResetEpoch(uint64_t& out) const;
    void ResetDecoder();
    std::mutex audio_queue_mutex_, decoder_mutex_;
    std::condition_variable audio_queue_cv_;
    AudioDecodeFence audio_decode_fence_;
    AudioResetEpochPublication audio_reset_epoch_publication_;
    ChatPlaybackReset chat_playback_reset_;
    void* opus_decoder_ = this;
    std::deque<int> timestamp_queue_, audio_decode_queue_, audio_playback_queue_, audio_testing_queue_;
};

// PRODUCTION_METHODS

int main() {
    AudioResetEpochPublication publication;
    uint64_t out = 99;
    assert(publication.TryRead(out) && out == 0);
    for (uint64_t epoch : {UINT64_C(0xffffffff), UINT64_C(0x100000000),
                           UINT64_C(0x100000001), UINT64_MAX}) {
        publication.BeginReset();
        const auto original = out;
        assert(!publication.TryRead(out) && out == original);
        publication.Publish(epoch);
        assert(publication.TryRead(out) && out == epoch);
    }
    AudioResetEpochPublication concurrent;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t n = 1; n <= 100000; ++n) {
            concurrent.BeginReset();
            concurrent.Publish((n << 32) | n);
        }
        done.store(true);
    });
    do {
        out = UINT64_MAX;
        if (concurrent.TryRead(out)) assert((out >> 32) == (out & UINT32_MAX));
        else assert(out == UINT64_MAX);
    } while (!done.load());
    writer.join();

    AudioService service;
    std::unique_lock<std::mutex> owner(service.audio_queue_mutex_);
    auto capture = std::async(std::launch::async, [&] {
        uint64_t epoch = 99;
        return service.TryGetPlaybackResetEpoch(epoch) && epoch == 0;
    });
    assert(capture.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready);
    assert(capture.get());
    owner.unlock();

    // Force ResetDecoder to stall on the actual decoder mutex, then inside
    // the decoder API. Identity must already change at both blocking points.
    std::unique_lock<std::mutex> decoder_owner(service.decoder_mutex_);
    std::thread reset([&] { service.ResetDecoder(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    } while (!service.TryGetPlaybackResetEpoch(out) || out != 1);
    assert(!decoder_entered.load());
    decoder_owner.unlock();
    while (!decoder_entered.load()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(service.TryGetPlaybackResetEpoch(out) && out == 1);
    release_decoder.store(true);
    reset.join();
    assert(service.audio_decode_fence_.ResetEpoch() == out);
}
