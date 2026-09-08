#include "wifi_recovery_timer_gate.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

void ReconnectCannotSlipBetweenRetryValidationAndTimerStart() {
    WifiRecoveryTimerGate gate;
    std::atomic<uint32_t> generation{7};
    std::atomic<int64_t> deadline_us{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> timer_active{false};
    std::mutex barrier_mutex;
    std::condition_variable barrier;
    bool arm_entered = false;
    bool allow_arm = false;

    std::thread retry([&]() {
        gate.RunArmTransaction(
            [&]() { return generation.load() == 7 && !connected.load(); },
            [&]() {
                {
                    std::lock_guard<std::mutex> lock(barrier_mutex);
                    arm_entered = true;
                }
                barrier.notify_all();
                std::unique_lock<std::mutex> lock(barrier_mutex);
                barrier.wait(lock, [&]() { return allow_arm; });
                deadline_us.store(60'000'000);
                timer_active.store(true);
            });
    });

    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier.wait(lock, [&]() { return arm_entered; });
    }
    connected.store(true);
    std::thread reconnect([&]() {
        gate.RunInvalidationTransaction([&]() {
            deadline_us.store(0);
            generation.fetch_add(1);
            timer_active.store(false);
        });
    });

    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        allow_arm = true;
    }
    barrier.notify_all();
    retry.join();
    reconnect.join();

    assert(generation.load() == 8);
    assert(deadline_us.load() == 0);
    assert(!timer_active.load());

    connected.store(false);
    const bool fresh_outage_armed = gate.RunArmTransaction(
        [&]() { return generation.load() == 8 && !connected.load(); },
        [&]() {
            deadline_us.store(120'000'000);
            timer_active.store(true);
        });
    assert(fresh_outage_armed);
    assert(deadline_us.load() == 120'000'000);
    assert(timer_active.load());
}

void StaleConditionalRetryIsRejectedInsideTheGate() {
    WifiRecoveryTimerGate gate;
    std::atomic<uint32_t> generation{4};
    std::atomic<bool> connected{true};
    bool armed = false;

    const bool accepted = gate.RunArmTransaction(
        [&]() { return generation.load() == 3 && !connected.load(); },
        [&]() { armed = true; });

    assert(!accepted);
    assert(!armed);
}

}  // namespace

int main() {
    ReconnectCannotSlipBetweenRetryValidationAndTimerStart();
    StaleConditionalRetryIsRejectedInsideTheGate();
    std::cout << "wifi_recovery_timer_gate_host_test: PASS\n";
    return 0;
}
