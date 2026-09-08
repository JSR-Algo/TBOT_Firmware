#include "wifi_config_intent_state.h"

#include <cassert>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

int main() {
    WifiConfigIntentState state;
    state.Request(WifiConfigIntentState::kConditional, 11);
    const auto old = state.Snapshot();

    std::mutex mutex;
    std::condition_variable condition;
    bool newer_requested = false;
    std::thread newer([&]() {
        state.Request(WifiConfigIntentState::kConditional, 12);
        {
            std::lock_guard<std::mutex> lock(mutex);
            newer_requested = true;
        }
        condition.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&]() { return newer_requested; });
    }

    assert(!state.ConsumeIfCurrent(old.request_generation,
                                   WifiConfigIntentState::kConditional));
    const auto current = state.Snapshot();
    assert((current.flags & WifiConfigIntentState::kConditional) != 0);
    assert(current.recovery_generation == 12);
    assert(current.request_generation != old.request_generation);
    newer.join();

    std::cout << "wifi_config_intent_state_host_test: PASS\n";
    return 0;
}
