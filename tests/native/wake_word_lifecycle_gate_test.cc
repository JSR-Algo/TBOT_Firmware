#include "audio/wake_word_lifecycle_gate.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "wake word lifecycle gate test failed: " << message << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    WakeWordLifecycleGate gate;
    std::mutex rendezvous_mutex;
    std::condition_variable rendezvous_cv;
    bool prewarm_entered = false;
    bool allow_prewarm_finish = false;
    std::atomic<bool> release_attempted{false};
    std::atomic<bool> release_ran{false};

    std::thread prewarm([&]() {
        Require(gate.RunPrewarm([&]() {
            std::unique_lock<std::mutex> lock(rendezvous_mutex);
            prewarm_entered = true;
            rendezvous_cv.notify_all();
            rendezvous_cv.wait(lock, [&]() { return allow_prewarm_finish; });
        }), "initial prewarm acquires lifecycle ownership");
    });

    {
        std::unique_lock<std::mutex> lock(rendezvous_mutex);
        rendezvous_cv.wait(lock, [&]() { return prewarm_entered; });
    }

    std::thread release([&]() {
        release_attempted.store(true);
        gate.CancelPrewarmAndRunRelease([&]() { release_ran.store(true); });
    });

    while (!release_attempted.load()) {
        std::this_thread::yield();
    }
    Require(!release_ran.load(), "release waits for in-flight prewarm ownership");

    {
        std::lock_guard<std::mutex> lock(rendezvous_mutex);
        allow_prewarm_finish = true;
    }
    rendezvous_cv.notify_all();
    prewarm.join();
    release.join();

    Require(release_ran.load(), "release runs after prewarm exits");
    bool late_prewarm_ran = false;
    Require(!gate.RunPrewarm([&]() { late_prewarm_ran = true; }),
            "release cancellation rejects later prewarm");
    Require(!late_prewarm_ran, "rejected prewarm callback never touches wake-word state");

    WakeWordLifecycleGate cancelled_first;
    cancelled_first.CancelPrewarmAndRunRelease([]() {});
    Require(!cancelled_first.RunPrewarm([]() {}),
            "release-first interleaving remains fail-closed");

    std::cout << "wake word lifecycle gate test OK\n";
    return 0;
}
