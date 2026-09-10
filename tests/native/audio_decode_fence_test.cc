#include "audio/audio_decode_fence.h"
#include "audio/audio_playback_refill_policy.h"
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>

// The decoder barrier represents work outside the production queue mutex.
void CheckDecode(bool reset, bool generation_changes, bool succeeds) {
    AudioDecodeFence fence;
    std::mutex queue_mutex;
    std::condition_variable cv;
    bool decode_blocked = false;
    bool release_decode = false;
    bool completed = false;
    unsigned playback_queued = 0;
    unsigned generation = 4;
    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(queue_mutex);
        const auto epoch = fence.Begin();
        const auto packet_generation = generation;
        decode_blocked = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_decode; });
        const bool current = fence.Complete(epoch, packet_generation, generation);
        if (succeeds && current) {
            ++playback_queued;
        }
        AudioPlaybackRefillPolicy policy;
        if (!current || !succeeds) {
            assert(!policy.DeferEncode(succeeds && current, true, true, 0, 2));
        }
        completed = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        cv.wait(lock, [&] { return decode_blocked; });
        assert(fence.InFlight()); // Empty queues alone cannot report drained.
        assert(playback_queued == 0);
        if (reset) {
            fence.Reset();
            fence.Reset(); // Consecutive resets must retain worker ownership.
            assert(fence.InFlight());
        }
        if (generation_changes) {
            ++generation;
        }
        release_decode = true;
        cv.notify_all();
        cv.wait(lock, [&] { return completed; });
        assert(!fence.InFlight()); // Success and failure both release the claim.
        assert(playback_queued == (succeeds && !reset && !generation_changes ? 1u : 0u));
        const auto next = fence.Begin();
        assert(fence.Complete(next, generation, generation));
        assert(!fence.InFlight());
    }
    worker.join();
}

void CheckResetDuringDecoderCall() {
    AudioDecodeFence fence;
    std::mutex queue_mutex;
    std::mutex decoder_mutex;
    std::mutex barrier_mutex;
    std::condition_variable barrier;
    bool decoding = false;
    bool reset_waiting = false;
    bool release_decode = false;
    bool enqueued = false;
    std::thread worker([&] {
        std::unique_lock<std::mutex> queue_lock(queue_mutex);
        const auto epoch = fence.Begin();
        queue_lock.unlock();
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex);
        {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            decoding = true;
            barrier.notify_all();
            barrier.wait(lock, [&] { return release_decode; });
        }
        decoder_lock.unlock();
        queue_lock.lock();
        enqueued = fence.Complete(epoch, 4, 4);
    });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier.wait(lock, [&] { return decoding; });
    }
    std::thread resetter([&] {
        std::lock_guard<std::mutex> queue_lock(queue_mutex);
        fence.Reset();
        {
            std::lock_guard<std::mutex> lock(barrier_mutex);
            reset_waiting = true;
            barrier.notify_all();
        }
        std::lock_guard<std::mutex> decoder_lock(decoder_mutex);
        assert(fence.InFlight());
    });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier.wait(lock, [&] { return reset_waiting; });
        release_decode = true;
        barrier.notify_all();
    }
    resetter.join();
    worker.join();
    assert(!enqueued);
    assert(!fence.InFlight());
}

int main() {
    CheckDecode(false, false, true);
    CheckDecode(true, false, true);
    CheckDecode(false, true, true);
    CheckDecode(true, true, true);
    CheckDecode(false, false, false);
    CheckDecode(true, false, false);
    CheckResetDuringDecoderCall();
}
