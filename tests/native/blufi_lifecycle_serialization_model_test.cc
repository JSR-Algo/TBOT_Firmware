#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

class LifecycleModel {
public:
    void ReleaseAcrossDrain() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        primitive_deinit_calls_.fetch_add(1);
        SignalGapAndWait();
        release_postvalidated_.store(true);
    }

    void RestartAcrossGap() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        primitive_deinit_calls_.fetch_add(1);
        SignalGapAndWait();
        primitive_init_calls_.fetch_add(1);
        restart_completed_.store(true);
    }

    void PublicInit() {
        public_attempted_.store(true);
        gap_cv_.notify_all();
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        init_entered_after_transaction_.store(
            release_postvalidated_.load() || restart_completed_.load());
        primitive_init_calls_.fetch_add(1);
    }

    void PublicDeinit() {
        public_attempted_.store(true);
        gap_cv_.notify_all();
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        deinit_entered_after_restart_.store(restart_completed_.load());
        primitive_deinit_calls_.fetch_add(1);
    }

    void WaitForGap() {
        std::unique_lock<std::mutex> lock(gap_mutex_);
        gap_cv_.wait(lock, [this]() { return gap_open_; });
    }

    void WaitForPublicAttempt() {
        std::unique_lock<std::mutex> lock(gap_mutex_);
        gap_cv_.wait(lock, [this]() { return public_attempted_.load(); });
    }

    void FinishGap() {
        {
            std::lock_guard<std::mutex> lock(gap_mutex_);
            finish_gap_ = true;
        }
        gap_cv_.notify_all();
    }

    bool InitEnteredAfterTransaction() const {
        return init_entered_after_transaction_.load();
    }

    bool DeinitEnteredAfterRestart() const {
        return deinit_entered_after_restart_.load();
    }

private:
    void SignalGapAndWait() {
        std::unique_lock<std::mutex> lock(gap_mutex_);
        gap_open_ = true;
        gap_cv_.notify_all();
        gap_cv_.wait(lock, [this]() { return finish_gap_; });
    }

    std::mutex lifecycle_mutex_;
    std::mutex gap_mutex_;
    std::condition_variable gap_cv_;
    bool gap_open_ = false;
    bool finish_gap_ = false;
    std::atomic<int> primitive_init_calls_{0};
    std::atomic<int> primitive_deinit_calls_{0};
    std::atomic<bool> release_postvalidated_{false};
    std::atomic<bool> restart_completed_{false};
    std::atomic<bool> public_attempted_{false};
    std::atomic<bool> init_entered_after_transaction_{false};
    std::atomic<bool> deinit_entered_after_restart_{false};
};

void ReleaseDrainBlocksConcurrentPublicInit() {
    LifecycleModel model;
    std::thread release([&model]() { model.ReleaseAcrossDrain(); });
    model.WaitForGap();
    std::thread init([&model]() { model.PublicInit(); });
    model.WaitForPublicAttempt();
    model.FinishGap();
    release.join();
    init.join();
    assert(model.InitEnteredAfterTransaction());
}

void RestartGapBlocksConcurrentPublicDeinit() {
    LifecycleModel model;
    std::thread restart([&model]() { model.RestartAcrossGap(); });
    model.WaitForGap();
    std::thread deinit([&model]() { model.PublicDeinit(); });
    model.WaitForPublicAttempt();
    model.FinishGap();
    restart.join();
    deinit.join();
    assert(model.DeinitEnteredAfterRestart());
}

}  // namespace

int main() {
    ReleaseDrainBlocksConcurrentPublicInit();
    RestartGapBlocksConcurrentPublicDeinit();
    return 0;
}
