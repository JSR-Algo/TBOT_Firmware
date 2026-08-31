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

    void ReleaseThenPostvalidate() {
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        SignalGapAndWaitV2();
        std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
        release_postvalidated_v2_.store(true);
    }

    void CommitThenRunDeferredInit() {
        {
            std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
            claim_committed_v2_.store(true);
        }
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        init_after_release_v2_.store(release_postvalidated_v2_.load());
    }

    void RestartThenComplete() {
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        SignalGapAndWaitV2();
        std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
        restart_completed_v2_.store(true);
    }

    void CommitThenRunDeferredStop() {
        {
            std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
            claim_committed_v2_.store(true);
        }
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        stop_after_restart_v2_.store(restart_completed_v2_.load());
    }

    void WaitForGapV2() {
        std::unique_lock<std::mutex> lock(gap_mutex_v2_);
        gap_cv_v2_.wait(lock, [this]() { return gap_open_v2_; });
    }

    void WaitForClaimCommitV2() {
        while (!claim_committed_v2_.load()) {
            std::this_thread::yield();
        }
    }

    void FinishGapV2() {
        {
            std::lock_guard<std::mutex> lock(gap_mutex_v2_);
            finish_gap_v2_ = true;
        }
        gap_cv_v2_.notify_all();
    }

    bool InitAfterReleaseV2() const { return init_after_release_v2_.load(); }
    bool StopAfterRestartV2() const { return stop_after_restart_v2_.load(); }

    uint32_t CommitOriginalGenerationGate() {
        std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
        return setup_generation_v3_.load();
    }

    void RestartAfterOriginalGenerationGate() {
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
        setup_generation_v3_.fetch_add(1);
    }

    void RunDeferredDispatch(uint32_t expected_generation, bool confirmation) {
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        {
            std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
            if (expected_generation != setup_generation_v3_.load()) {
                return;
            }
        }
        if (confirmation) {
            confirmation_launches_v3_.fetch_add(1);
        } else {
            refresh_launches_v3_.fetch_add(1);
        }
    }

    int ConfirmationLaunchesV3() const { return confirmation_launches_v3_.load(); }
    int RefreshLaunchesV3() const { return refresh_launches_v3_.load(); }

    bool AttemptConfirmationDispatchFailure(uint32_t expected_generation) {
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        {
            std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
            if (expected_generation != setup_generation_v3_.load()) {
                return false;
            }
        }
        {
            std::unique_lock<std::mutex> lock(fallback_gap_mutex_v4_);
            confirmation_dispatch_failed_v4_ = true;
            fallback_gap_cv_v4_.notify_all();
            fallback_gap_cv_v4_.wait(lock, [this]() {
                return restart_attempted_v4_.load();
            });
        }
        StartClaimPollFallback();
        return false;
    }

    void WaitForConfirmationDispatchFailureV4() {
        std::unique_lock<std::mutex> lock(fallback_gap_mutex_v4_);
        fallback_gap_cv_v4_.wait(lock, [this]() {
            return confirmation_dispatch_failed_v4_;
        });
    }

    void RestartAtConfirmationFailureBoundaryV4() {
        restart_attempted_v4_.store(true);
        fallback_gap_cv_v4_.notify_all();
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
        restart_saw_committed_poll_v4_.store(claim_poll_starts_v4_.load() == 1);
        setup_generation_v3_.fetch_add(1);
    }

    bool AttemptFetchDispatchFailure(uint32_t expected_generation) {
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        {
            std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
            if (expected_generation != setup_generation_v3_.load()) {
                return false;
            }
        }
        return false;
    }

    void StartClaimPollFallback() { claim_poll_starts_v4_.fetch_add(1); }

    void RunFetchFallbackReservation(uint32_t expected_generation) {
        std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_v2_);
        {
            std::lock_guard<std::timed_mutex> finalization(finalization_mutex_);
            if (expected_generation != setup_generation_v3_.load()) {
                return;
            }
        }
        RestoreFetchFallback();
    }

    void RestoreFetchFallback() {
        fetch_substate_writes_v4_.fetch_add(1);
        fetch_renders_v4_.fetch_add(1);
        fetch_ensure_calls_v4_.fetch_add(1);
        claim_poll_starts_v4_.fetch_add(1);
    }

    int ClaimPollStartsV4() const { return claim_poll_starts_v4_.load(); }
    int FetchSubstateWritesV4() const { return fetch_substate_writes_v4_.load(); }
    int FetchRendersV4() const { return fetch_renders_v4_.load(); }
    int FetchEnsureCallsV4() const { return fetch_ensure_calls_v4_.load(); }
    bool RestartSawCommittedPollV4() const {
        return restart_saw_committed_poll_v4_.load();
    }

private:
    void SignalGapAndWait() {
        std::unique_lock<std::mutex> lock(gap_mutex_);
        gap_open_ = true;
        gap_cv_.notify_all();
        gap_cv_.wait(lock, [this]() { return finish_gap_; });
    }

    void SignalGapAndWaitV2() {
        std::unique_lock<std::mutex> lock(gap_mutex_v2_);
        gap_open_v2_ = true;
        gap_cv_v2_.notify_all();
        gap_cv_v2_.wait(lock, [this]() { return finish_gap_v2_; });
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
    std::timed_mutex lifecycle_mutex_v2_;
    std::timed_mutex finalization_mutex_;
    std::mutex gap_mutex_v2_;
    std::condition_variable gap_cv_v2_;
    bool gap_open_v2_ = false;
    bool finish_gap_v2_ = false;
    std::atomic<bool> claim_committed_v2_{false};
    std::atomic<bool> release_postvalidated_v2_{false};
    std::atomic<bool> restart_completed_v2_{false};
    std::atomic<bool> init_after_release_v2_{false};
    std::atomic<bool> stop_after_restart_v2_{false};
    std::atomic<uint32_t> setup_generation_v3_{1};
    std::atomic<int> confirmation_launches_v3_{0};
    std::atomic<int> refresh_launches_v3_{0};
    std::atomic<int> claim_poll_starts_v4_{0};
    std::atomic<int> fetch_substate_writes_v4_{0};
    std::atomic<int> fetch_renders_v4_{0};
    std::atomic<int> fetch_ensure_calls_v4_{0};
    std::mutex fallback_gap_mutex_v4_;
    std::condition_variable fallback_gap_cv_v4_;
    bool confirmation_dispatch_failed_v4_ = false;
    std::atomic<bool> restart_attempted_v4_{false};
    std::atomic<bool> restart_saw_committed_poll_v4_{false};
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

void ReleaseFinalizationWaitsForCommitBeforeDeferredInit() {
    LifecycleModel model;
    std::thread release([&model]() { model.ReleaseThenPostvalidate(); });
    model.WaitForGapV2();
    std::thread claim([&model]() { model.CommitThenRunDeferredInit(); });
    model.WaitForClaimCommitV2();
    model.FinishGapV2();
    release.join();
    claim.join();
    assert(model.InitAfterReleaseV2());
}

void RestartFinalizationWaitsForCommitBeforeDeferredStop() {
    LifecycleModel model;
    std::thread restart([&model]() { model.RestartThenComplete(); });
    model.WaitForGapV2();
    std::thread claim([&model]() { model.CommitThenRunDeferredStop(); });
    model.WaitForClaimCommitV2();
    model.FinishGapV2();
    restart.join();
    claim.join();
    assert(model.StopAfterRestartV2());
}

void RestartAfterOriginalGateSuppressesDeferredConfirmationLaunch() {
    LifecycleModel model;
    const uint32_t generation = model.CommitOriginalGenerationGate();
    model.RestartAfterOriginalGenerationGate();
    model.RunDeferredDispatch(generation, true);
    assert(model.ConfirmationLaunchesV3() == 0);
}

void RestartAfterOriginalGateSuppressesDeferredRefreshLaunch() {
    LifecycleModel model;
    const uint32_t generation = model.CommitOriginalGenerationGate();
    model.RestartAfterOriginalGenerationGate();
    model.RunDeferredDispatch(generation, false);
    assert(model.RefreshLaunchesV3() == 0);
}

void FailedConfirmationDispatchCommitsPollBeforeRestartCanAdvanceGeneration() {
    LifecycleModel model;
    const uint32_t generation = model.CommitOriginalGenerationGate();
    std::thread failure([&model, generation]() {
        model.AttemptConfirmationDispatchFailure(generation);
    });
    model.WaitForConfirmationDispatchFailureV4();
    std::thread restart([&model]() {
        model.RestartAtConfirmationFailureBoundaryV4();
    });
    failure.join();
    restart.join();
    assert(model.ClaimPollStartsV4() == 1);
    assert(model.RestartSawCommittedPollV4());
}

void RestartAfterFailedFetchDispatchSuppressesStandbyFallback() {
    LifecycleModel model;
    const uint32_t generation = model.CommitOriginalGenerationGate();
    model.AttemptFetchDispatchFailure(generation);
    model.RestartAfterOriginalGenerationGate();
    model.RunFetchFallbackReservation(generation);
    assert(model.FetchSubstateWritesV4() == 0);
    assert(model.FetchRendersV4() == 0);
    assert(model.FetchEnsureCallsV4() == 0);
    assert(model.ClaimPollStartsV4() == 0);
}

}  // namespace

int main() {
    ReleaseDrainBlocksConcurrentPublicInit();
    RestartGapBlocksConcurrentPublicDeinit();
    ReleaseFinalizationWaitsForCommitBeforeDeferredInit();
    RestartFinalizationWaitsForCommitBeforeDeferredStop();
    RestartAfterOriginalGateSuppressesDeferredConfirmationLaunch();
    RestartAfterOriginalGateSuppressesDeferredRefreshLaunch();
    FailedConfirmationDispatchCommitsPollBeforeRestartCanAdvanceGeneration();
    RestartAfterFailedFetchDispatchSuppressesStandbyFallback();
    return 0;
}
