#include "boards/common/blufi_transition_gate.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "blufi transition gate test failed: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    constexpr int kReentrantFailure = -7;
    BlufiTransitionGate gate(kReentrantFailure);

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool owner_started = false;
    bool release_owner = false;
    std::atomic<int> owners{0};
    int observed_result = 0;

    std::thread first([&]() {
        auto turn = gate.Acquire(BlufiTransitionGate::Operation::kDeinit, 1);
        Require(turn.owner(), "first deinit caller owns the transition");
        ++owners;
        {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            owner_started = true;
            barrier_cv.notify_all();
            barrier_cv.wait(lock, [&]() { return release_owner; });
        }
        gate.Complete(turn, 37);
    });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_cv.wait(lock, [&]() { return owner_started; });
    }
    std::thread second([&]() {
        auto turn = gate.Acquire(BlufiTransitionGate::Operation::kDeinit, 2);
        Require(!turn.owner(), "concurrent same-operation caller observes owner result");
        observed_result = turn.result();
    });
    while (gate.WaitingSameOperationCallers() == 0) {
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_owner = true;
        barrier_cv.notify_all();
    }
    first.join();
    second.join();
    Require(owners.load() == 1, "same operation executes one SDK owner");
    Require(observed_result == 37, "same operation returns the exact owner result");

    auto outer = gate.Acquire(BlufiTransitionGate::Operation::kDeinit, 3);
    Require(outer.owner(), "outer callback transition owns gate");
    auto reentrant = gate.Acquire(BlufiTransitionGate::Operation::kDeinit, 3);
    Require(!reentrant.owner() && reentrant.result() == kReentrantFailure,
            "same-task callback reentry fails closed");
    gate.Complete(outer, 0);

    std::mutex order_mutex;
    std::condition_variable order_cv;
    bool deinit_active = false;
    bool finish_deinit = false;
    std::atomic<int> phase{0};
    std::thread deinit([&]() {
        auto turn = gate.Acquire(BlufiTransitionGate::Operation::kDeinit, 4);
        Require(turn.owner(), "deinit owns conflicting transition first");
        phase.store(1);
        {
            std::unique_lock<std::mutex> lock(order_mutex);
            deinit_active = true;
            order_cv.notify_all();
            order_cv.wait(lock, [&]() { return finish_deinit; });
        }
        gate.Complete(turn, 0);
    });
    {
        std::unique_lock<std::mutex> lock(order_mutex);
        order_cv.wait(lock, [&]() { return deinit_active; });
    }
    std::thread init([&]() {
        auto turn = gate.Acquire(BlufiTransitionGate::Operation::kInit, 5);
        Require(turn.owner(), "conflicting init becomes next owner");
        Require(phase.load() == 1, "init waits for deinit completion");
        phase.store(2);
        gate.Complete(turn, 0);
    });
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        finish_deinit = true;
        order_cv.notify_all();
    }
    deinit.join();
    init.join();
    Require(phase.load() == 2, "conflicting operations execute serially");

    std::cout << "blufi transition gate test OK\n";
    return 0;
}
