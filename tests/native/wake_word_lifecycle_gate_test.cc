#include "audio/wake_word_lifecycle_controller.h"
#include "audio/provisioning_session_binding.h"

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
    Require(!controller.EndProvisioningAndRearm({}),
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
    WakeWordLifecycleController::ProvisioningToken enable_token;
    std::thread enable_release([&]() {
        enable_token = controller.BeginProvisioningAndQuiesce([&]() {
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
    Require(controller.FinishProvisioningReset(enable_token), "current begin token finishes reset");
    Require(controller.EndProvisioningAndRearm(enable_token), "provisioning owner rearms once");
    const auto first_rearm_generation = controller.CapturePrewarmToken().generation;
    Require(!controller.EndProvisioningAndRearm(enable_token), "duplicate success rearm is ignored");
    Require(controller.CapturePrewarmToken().generation == first_rearm_generation,
            "duplicate success does not advance generation");
    Require(controller.SetRunning(true, controller.CapturePrewarmToken().generation),
            "rearmed generation can publish running");
    feed = controller.TryAcquireFeed();

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool quiescing = false;
    std::atomic<bool> quiesced{false};
    WakeWordLifecycleController::ProvisioningToken release_token;
    std::thread release([&]() {
        release_token = controller.BeginProvisioningAndQuiesce([&]() {
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

    Require(controller.FinishProvisioningReset(release_token), "release token finishes reset");
    Require(!controller.TryAcquireAccess(), "provisioning ownership remains fail-closed");
    Require(controller.EndProvisioningAndRearm(release_token), "accessor cycle rearms");
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
    WakeWordLifecycleController::ProvisioningToken accessor_token;
    std::thread accessor_release([&]() {
        accessor_token = controller.BeginProvisioningAndQuiesce([&]() {
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
    Require(controller.FinishProvisioningReset(accessor_token), "accessor token finishes reset");
    Require(controller.EndProvisioningAndRearm(accessor_token), "accessor token rearms");

    const auto old_success = controller.BeginProvisioningAndQuiesce([]() {});
    Require(controller.FinishProvisioningReset(old_success), "old session reaches teardown pause");
    std::mutex overlap_mutex;
    std::condition_variable overlap_cv;
    bool old_deinit_complete = false;
    bool release_old_success = false;
    std::atomic<bool> old_rearm_result{true};
    std::thread old_completion([&]() {
        {
            std::unique_lock<std::mutex> lock(overlap_mutex);
            old_deinit_complete = true;
            overlap_cv.notify_all();
            overlap_cv.wait(lock, [&]() { return release_old_success; });
        }
        old_rearm_result.store(controller.EndProvisioningAndRearm(old_success));
    });
    {
        std::unique_lock<std::mutex> lock(overlap_mutex);
        overlap_cv.wait(lock, [&]() { return old_deinit_complete; });
    }
    const auto new_session = controller.BeginProvisioningAndQuiesce([]() {});
    Require(controller.FinishProvisioningReset(new_session), "new session owns provisioning");
    {
        std::lock_guard<std::mutex> lock(overlap_mutex);
        release_old_success = true;
        overlap_cv.notify_all();
    }
    old_completion.join();
    Require(!old_rearm_result.load(),
            "old success cannot clear newer provisioning ownership");
    Require(!controller.TryAcquireAccess(),
            "new session remains provisioning-owned after stale success");
    Require(controller.EndProvisioningAndRearm(new_session),
            "current session can complete after stale success rejection");

    WakeWordLifecycleController binding_controller;
    ProvisioningSessionBinding binding;
    const auto session_a = binding_controller.BeginProvisioningAndQuiesce([]() {});
    Require(binding_controller.FinishProvisioningReset(session_a), "session A owns reset");
    Require(binding.Bind(session_a), "session A binds");
    const auto delayed_callback_a = binding.Capture();
    const auto session_b = binding_controller.BeginProvisioningAndQuiesce([]() {});
    Require(binding_controller.FinishProvisioningReset(session_b), "session B owns reset");
    {
        auto completion_a = binding.Claim(session_a);
        Require(static_cast<bool>(completion_a), "session A atomically claims completion");
        Require(!binding.Bind(session_b), "session B cannot replace binding during A completion");
        Require(binding.Matches(session_a), "A binding remains held while completion is active");
    }
    Require(binding.Matches(session_a), "failed A completion retains binding for retry");
    Require(binding.Bind(session_b), "session B binds after A completion releases");
    int deinit_calls = 0;
    const auto complete = [&](WakeWordLifecycleController::ProvisioningToken token) {
        auto completion = binding.Claim(token);
        if (!completion) {
            return false;
        }
        ++deinit_calls;
        if (!binding_controller.EndProvisioningAndRearm(token)) {
            return false;
        }
        completion.ConsumeSuccess();
        return true;
    };
    Require(!complete(delayed_callback_a),
            "delayed session A callback is rejected after session B binds");
    Require(deinit_calls == 0, "stale session is rejected before BLE deinit");
    Require(!binding_controller.TryAcquireAccess(),
            "session B remains provisioning-owned after stale integration callback");
    Require(complete(session_b), "bound session B completes successfully");
    Require(deinit_calls == 1, "only current session owns BLE deinit");
    Require(!binding.Capture().valid(), "successful completion consumes binding once");

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
        const auto token = controller.BeginProvisioningAndQuiesce([]() {});
        Require(controller.FinishProvisioningReset(token), "repeated token finishes reset");
        Require(controller.EndProvisioningAndRearm(token), "repeated transition rearms once");
    }
    Require(controller.CapturePrewarmToken().valid(), "repeated transitions remain rearmable");

    std::cout << "wake word lifecycle controller test OK\n";
    return 0;
}
