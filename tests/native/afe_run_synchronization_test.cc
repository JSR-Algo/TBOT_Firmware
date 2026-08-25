#include "audio/wake_words/afe_run_synchronization.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "AFE run synchronization test failed: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    AfeRunSynchronization sync;
    std::atomic<uint32_t> generation{0};
    std::atomic<uint32_t> acknowledged_generation{0};

    const uint32_t first_run = sync.BeginStart(generation);
    Require(first_run == 1, "start advances the run generation");
    bool stale_event_cleared = false;
    bool running_cleared = false;
    const uint32_t first_stop = sync.BeginStopAndClear(
        generation,
        [&]() { stale_event_cleared = true; },
        [&]() { running_cleared = true; });
    Require(first_stop == 2, "stop invalidates the active generation");
    Require(stale_event_cleared && running_cleared,
            "stop clears stale acknowledgement and running after choosing generation");
    Require(!sync.IsStopAcknowledged(acknowledged_generation, first_stop),
            "an acknowledgement from an older generation is rejected");
    sync.PublishStopped(acknowledged_generation, first_stop);
    Require(sync.IsStopAcknowledged(acknowledged_generation, first_stop),
            "the exact stopped generation is accepted");

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool stop_waiting = false;
    bool start_attempted = false;
    std::atomic<bool> start_completed{false};
    uint32_t stopped_generation = 0;
    uint32_t restarted_generation = 0;

    std::thread stop_thread([&]() {
        auto transition_lock = sync.AcquireTransition();
        stopped_generation = sync.BeginStopAndClear(generation, []() {}, []() {});
        {
            std::lock_guard<std::mutex> barrier_lock(barrier_mutex);
            stop_waiting = true;
            barrier_cv.notify_all();
        }
        const auto start = std::chrono::steady_clock::now();
        Require(sync.WaitForStopAcknowledgement(
                    acknowledged_generation, stopped_generation, 500,
                    [&]() {
                        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count());
                    },
                    [&](uint32_t) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }),
                "exact generation acknowledgement completes bounded wait");
    });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_cv.wait(lock, [&]() { return stop_waiting; });
    }
    std::thread start_thread([&]() {
        {
            std::lock_guard<std::mutex> barrier_lock(barrier_mutex);
            start_attempted = true;
            barrier_cv.notify_all();
        }
        auto transition_lock = sync.AcquireTransition();
        restarted_generation = sync.BeginStart(generation);
        start_completed = true;
    });

    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_cv.wait(lock, [&]() { return start_attempted; });
    }
    Require(!start_completed, "start cannot pass a stop transition awaiting acknowledgement");
    sync.PublishStopped(acknowledged_generation, stopped_generation);
    stop_thread.join();
    start_thread.join();
    Require(restarted_generation > stopped_generation,
            "the restarted run begins only after the stopped generation is acknowledged");

    const uint32_t stale_fetch_generation = restarted_generation;
    const uint32_t timeout_stop = sync.BeginStopAndClear(generation, []() {}, []() {});
    Require(!sync.IsCurrent(generation, stale_fetch_generation),
            "a fetch from a superseded run is rejected");
    const bool timeout_acknowledged =
        sync.IsStopAcknowledged(acknowledged_generation, timeout_stop);
    Require(!timeout_acknowledged,
            "timeout path cannot treat a prior acknowledgement as current");
    bool reset_called = false;
    if (timeout_acknowledged) {
        reset_called = true;
    }
    Require(!reset_called, "timeout skips AFE reset");

    sync.PublishStopped(acknowledged_generation, timeout_stop - 1);
    uint32_t elapsed_ms = 0;
    int wait_calls = 0;
    Require(!sync.WaitForStopAcknowledgement(
                acknowledged_generation, timeout_stop, 5,
                [&]() { return elapsed_ms; },
                [&](uint32_t remaining_ms) {
                    ++wait_calls;
                    elapsed_ms += remaining_ms > 1 ? 1 : remaining_ms;
                }),
            "late old acknowledgement does not satisfy exact-generation wait");
    Require(wait_calls > 1, "stale wakeups are retried until the bounded deadline");

    const uint32_t self_stop = sync.BeginStopAndClear(generation, []() {}, []() {});
    sync.PublishStopped(acknowledged_generation, self_stop);
    Require(sync.IsStopAcknowledged(acknowledged_generation, self_stop),
            "detection-task self-stop acknowledges without waiting");

    std::cout << "AFE run synchronization test OK\n";
    return 0;
}
