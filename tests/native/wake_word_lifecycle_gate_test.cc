#include "audio/wake_word_lifecycle_controller.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "wake word lifecycle controller test failed: " << message << "\n";
        std::exit(1);
    }
}

class FakeAsyncWakeWord {
public:
    void StartEncode() {
        std::lock_guard<std::mutex> lock(mutex_);
        encode_active_ = true;
    }

    bool Pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        pop_waiting_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&]() { return shutting_down_ || packet_ready_; });
        return packet_ready_;
    }

    void WaitUntilPopBlocked() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return pop_waiting_; });
    }

    void FinishFinalAccess() {
        std::lock_guard<std::mutex> lock(mutex_);
        final_access_complete_ = true;
        cv_.notify_all();
    }

    void PublishExitAck() {
        std::lock_guard<std::mutex> lock(mutex_);
        exit_ack_ = true;
        cv_.notify_all();
    }

    void Shutdown(const std::function<void()>& requested) {
        std::unique_lock<std::mutex> lock(mutex_);
        shutting_down_ = true;
        requested();
        cv_.notify_all();
        cv_.wait(lock, [&]() { return exit_ack_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool encode_active_ = false;
    bool final_access_complete_ = false;
    bool exit_ack_ = false;
    bool pop_waiting_ = false;
    bool packet_ready_ = false;
    bool shutting_down_ = false;
};
}  // namespace

int main() {
    WakeWordLifecycleController controller;
    const auto initial_generation = controller.CapturePrewarmToken().generation;
    Require(!controller.EndProvisioningAndRearm(),
            "rearm is a no-op when provisioning does not own the lifecycle");
    Require(controller.CapturePrewarmToken().generation == initial_generation,
            "no-op rearm does not advance generation");
    const auto boot_token = controller.CapturePrewarmToken();
    Require(boot_token.valid(), "boot activation receives a prewarm token");

    auto feed = controller.TryAcquireFeed();
    Require(!feed, "feed is rejected before running is published");
    Require(controller.SetRunning(true, controller.CapturePrewarmToken().generation),
            "running publication accepts the current generation");
    feed = controller.TryAcquireFeed();
    Require(static_cast<bool>(feed), "running feed acquires a lease");

    auto enabling = controller.TryAcquireAccess();
    std::mutex enable_race_mutex;
    std::condition_variable enable_race_cv;
    bool enable_quiescing = false;
    std::thread enable_release([&]() {
        controller.BeginProvisioningAndQuiesce([&]() {
            std::lock_guard<std::mutex> lock(enable_race_mutex);
            enable_quiescing = true;
            enable_race_cv.notify_all();
        });
    });
    {
        std::unique_lock<std::mutex> lock(enable_race_mutex);
        enable_race_cv.wait(lock, [&]() { return enable_quiescing; });
    }
    Require(!controller.SetRunning(true, enabling.generation()),
            "pre-quiesce enable lease cannot publish running after generation changes");
    enabling = {};
    feed = {};
    enable_release.join();
    controller.FinishProvisioningReset();
    Require(controller.EndProvisioningAndRearm(), "provisioning owner rearms once");
    const auto first_rearm_generation = controller.CapturePrewarmToken().generation;
    Require(!controller.EndProvisioningAndRearm(), "duplicate success rearm is ignored");
    Require(controller.CapturePrewarmToken().generation == first_rearm_generation,
            "duplicate success does not advance generation");
    Require(controller.SetRunning(true, controller.CapturePrewarmToken().generation),
            "rearmed generation can publish running");
    feed = controller.TryAcquireFeed();

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool quiescing = false;
    std::atomic<bool> quiesced{false};
    std::thread release([&]() {
        controller.BeginProvisioningAndQuiesce([&]() {
            std::lock_guard<std::mutex> lock(barrier_mutex);
            quiescing = true;
            barrier_cv.notify_all();
        });
        quiesced.store(true);
    });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_cv.wait(lock, [&]() { return quiescing; });
    }
    Require(!controller.TryAcquireFeed(), "stale running hint cannot acquire after quiesce");
    Require(!controller.TryAcquireAccess(), "accessors are rejected while quiescing");
    Require(!quiesced.load(), "release waits for the active feed acknowledgement");
    feed = {};
    release.join();
    Require(quiesced.load(), "release completes after feed lease exits");
    Require(!controller.TryAcquirePrewarm(boot_token), "old activation token stays invalid");

    controller.FinishProvisioningReset();
    Require(!controller.TryAcquireAccess(), "provisioning ownership remains fail-closed");
    Require(controller.EndProvisioningAndRearm(), "accessor cycle rearms");
    const auto rearmed_token = controller.CapturePrewarmToken();
    Require(rearmed_token.valid(), "successful terminal path issues a new token");
    Require(rearmed_token.generation != boot_token.generation, "rearm advances generation");
    Require(static_cast<bool>(controller.TryAcquirePrewarm(rearmed_token)),
            "new activation token can prewarm");
    Require(!controller.TryAcquirePrewarm(boot_token), "rearm never revives an old token");

    auto metrics_access = controller.TryAcquireAccess();
    std::atomic<bool> accessor_quiesced{false};
    std::mutex accessor_barrier_mutex;
    std::condition_variable accessor_barrier_cv;
    bool accessor_quiescing = false;
    std::thread accessor_release([&]() {
        controller.BeginProvisioningAndQuiesce([&]() {
            std::lock_guard<std::mutex> lock(accessor_barrier_mutex);
            accessor_quiescing = true;
            accessor_barrier_cv.notify_all();
        });
        accessor_quiesced.store(true);
    });
    {
        std::unique_lock<std::mutex> lock(accessor_barrier_mutex);
        accessor_barrier_cv.wait(lock, [&]() { return accessor_quiescing; });
    }
    Require(!accessor_quiesced.load(), "destroy waits for accessor/metrics lease");
    metrics_access = {};
    accessor_release.join();
    controller.FinishProvisioningReset();
    controller.EndProvisioningAndRearm();

    FakeAsyncWakeWord async;
    async.StartEncode();
    std::atomic<bool> pop_result{true};
    std::thread pop([&]() { pop_result.store(async.Pop()); });
    async.WaitUntilPopBlocked();
    std::mutex shutdown_barrier_mutex;
    std::condition_variable shutdown_barrier_cv;
    bool shutdown_requested = false;
    std::atomic<bool> shutdown_done{false};
    std::thread shutdown([&]() {
        async.Shutdown([&]() {
            std::lock_guard<std::mutex> lock(shutdown_barrier_mutex);
            shutdown_requested = true;
            shutdown_barrier_cv.notify_all();
        });
        shutdown_done.store(true);
    });
    {
        std::unique_lock<std::mutex> lock(shutdown_barrier_mutex);
        shutdown_barrier_cv.wait(lock, [&]() { return shutdown_requested; });
    }
    pop.join();
    Require(!pop_result.load(), "shutdown wakes blocked Pop with terminal false");
    Require(!shutdown_done.load(), "shutdown waits for async encode acknowledgement");
    async.FinishFinalAccess();
    Require(!shutdown_done.load(), "shutdown does not treat final access as exit acknowledgement");
    async.PublishExitAck();
    shutdown.join();
    Require(shutdown_done.load(), "shutdown completes after encode exit acknowledgement");

    for (int i = 0; i < 3; ++i) {
        controller.BeginProvisioningAndQuiesce([]() {});
        controller.FinishProvisioningReset();
        Require(controller.EndProvisioningAndRearm(), "repeated transition rearms once");
    }
    Require(controller.CapturePrewarmToken().valid(), "repeated transitions remain rearmable");

    std::cout << "wake word lifecycle controller test OK\n";
    return 0;
}
