#include "../../components/esp-wifi-connect/include/default_event_loop_barrier.h"
#include "../../components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"
#include "../../components/esp-wifi-connect/include/wifi_scan_recovery_executor.h"

#include "esp_wifi.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

enum class FailureStep : uint8_t {
    kNone,
    kScanStop,
    kWifiStop,
    kWifiDeinit,
    kBarrier,
    kWifiInit,
};

struct FakeDriver {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> calls;
    FailureStep failure = FailureStep::kNone;
    FailureStep not_initialized = FailureStep::kNone;
    FailureStep not_started = FailureStep::kNone;
    FailureStep invalid_state = FailureStep::kNone;
    bool block_scan_stop = false;
    bool scan_stop_entered = false;
    bool release_scan_stop = false;
    bool init_nvs_enabled = true;

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex);
        calls.clear();
        failure = FailureStep::kNone;
        not_initialized = FailureStep::kNone;
        not_started = FailureStep::kNone;
        invalid_state = FailureStep::kNone;
        block_scan_stop = false;
        scan_stop_entered = false;
        release_scan_stop = false;
        init_nvs_enabled = true;
    }

    esp_err_t Record(const char* call, FailureStep step) {
        std::unique_lock<std::mutex> lock(mutex);
        calls.emplace_back(call);
        if (step == FailureStep::kScanStop && block_scan_stop) {
            scan_stop_entered = true;
            condition.notify_all();
            condition.wait(lock, [this]() { return release_scan_stop; });
        }
        if (failure == step) {
            return ESP_FAIL;
        }
        if (not_initialized == step) {
            return ESP_ERR_WIFI_NOT_INIT;
        }
        if (not_started == step) {
            return ESP_ERR_WIFI_NOT_STARTED;
        }
        if (invalid_state == step) {
            return ESP_ERR_WIFI_STATE;
        }
        return ESP_OK;
    }
};

FakeDriver driver;

struct RecoveryFixture {
    WifiScanLeaseCoordinator coordinator;
    WifiScanLeaseCoordinator::Lease lease;
    WifiScanLeaseCoordinator::RecoveryDecision recovery;

    static WifiScanLeaseCoordinator::Lease Acquire(
            WifiScanLeaseCoordinator& coordinator) {
        const auto acquired = coordinator.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kStation);
        assert(acquired.acquired);
        return acquired.lease;
    }

    static WifiScanLeaseCoordinator::RecoveryDecision Begin(
            WifiScanLeaseCoordinator& coordinator,
            const WifiScanLeaseCoordinator::Lease& lease) {
        assert(coordinator.CommitSubmission(lease, false).drain_required);
        auto recovery = coordinator.BeginRecovery(lease);
        assert(recovery.begun());
        return recovery;
    }

    RecoveryFixture()
        : lease(Acquire(coordinator)), recovery(Begin(coordinator, lease)) {}
};

std::vector<std::string> Calls() {
    std::lock_guard<std::mutex> lock(driver.mutex);
    return driver.calls;
}

void SuccessfulRecoveryUsesExactChoreography() {
    driver.Reset();
    RecoveryFixture fixture;
    WifiScanRecoveryExecutor executor;

    const auto proof = executor.Execute(fixture.recovery);

    assert((Calls() == std::vector<std::string>{
        "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}));
    assert(proof.Proves(fixture.recovery));
    assert(!driver.init_nvs_enabled);
    assert(fixture.coordinator.CompleteRecovery(fixture.lease, proof));
    assert(fixture.coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
}

void EveryFailureReturnsNoProofAndStopsChoreography() {
    const std::vector<std::pair<FailureStep, std::vector<std::string>>> cases = {
        {FailureStep::kScanStop, {"scan_stop"}},
        {FailureStep::kWifiStop, {"scan_stop", "wifi_stop"}},
        {FailureStep::kWifiDeinit,
         {"scan_stop", "wifi_stop", "wifi_deinit"}},
        {FailureStep::kBarrier,
         {"scan_stop", "wifi_stop", "wifi_deinit", "barrier"}},
        {FailureStep::kWifiInit,
         {"scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}},
    };

    for (const auto& test_case : cases) {
        driver.Reset();
        driver.failure = test_case.first;
        RecoveryFixture fixture;
        WifiScanRecoveryExecutor executor;

        const auto proof = executor.Execute(fixture.recovery);

        assert(Calls() == test_case.second);
        assert(!proof.Proves(fixture.recovery));
        assert(!fixture.coordinator.CompleteRecovery(fixture.lease, proof));
        assert(!fixture.coordinator.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
    }
}

void NotInitializedShutdownStatesAreBenign() {
    for (const auto step : {FailureStep::kScanStop, FailureStep::kWifiStop,
                            FailureStep::kWifiDeinit}) {
        driver.Reset();
        driver.not_initialized = step;
        RecoveryFixture fixture;
        WifiScanRecoveryExecutor executor;

        const auto proof = executor.Execute(fixture.recovery);

        assert(proof.Proves(fixture.recovery));
        assert((Calls() == std::vector<std::string>{
            "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}));
    }
}

void ScanStopTransientStatesContinueThroughDriverStop() {
    for (const bool still_connecting : {false, true}) {
        driver.Reset();
        if (still_connecting) {
            driver.invalid_state = FailureStep::kScanStop;
        } else {
            driver.not_started = FailureStep::kScanStop;
        }
        RecoveryFixture fixture;
        WifiScanRecoveryExecutor executor;

        const auto proof = executor.Execute(fixture.recovery);

        assert(proof.Proves(fixture.recovery));
        assert((Calls() == std::vector<std::string>{
            "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}));
    }
}

void ScanStopTransientStatesAreNotBenignForLaterStages() {
    for (const auto step : {FailureStep::kWifiStop,
                            FailureStep::kWifiDeinit}) {
        for (const bool wifi_state : {false, true}) {
            driver.Reset();
            if (wifi_state) {
                driver.invalid_state = step;
            } else {
                driver.not_started = step;
            }
            RecoveryFixture fixture;
            WifiScanRecoveryExecutor executor;

            const auto proof = executor.Execute(fixture.recovery);

            assert(!proof.Proves(fixture.recovery));
            const std::vector<std::string> expected =
                step == FailureStep::kWifiStop
                    ? std::vector<std::string>{"scan_stop", "wifi_stop"}
                    : std::vector<std::string>{
                          "scan_stop", "wifi_stop", "wifi_deinit"};
            assert(Calls() == expected);
            assert(!fixture.coordinator.TryAcquire(
                WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
        }
    }
}

void InvalidAndOverflowedRecoveryDecisionsNeverTouchTheDriver() {
    driver.Reset();
    WifiScanLeaseCoordinator coordinator(
        0, 1, std::numeric_limits<uint64_t>::max());
    const auto acquired = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation);
    assert(acquired.acquired);
    assert(coordinator.CommitSubmission(acquired.lease, false).drain_required);
    const auto overflowed = coordinator.BeginRecovery(acquired.lease);
    assert(!overflowed.begun());

    WifiScanRecoveryExecutor executor;
    const auto proof = executor.Execute(overflowed);

    assert(Calls().empty());
    assert(!proof.Proves(overflowed));
    assert(!coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
}

void ExecuteCallsAreSerialized() {
    driver.Reset();
    driver.block_scan_stop = true;
    RecoveryFixture first;
    RecoveryFixture second;
    WifiScanRecoveryExecutor first_executor;
    WifiScanRecoveryExecutor second_executor;
    std::optional<WifiScanLeaseCoordinator::RecoveryProof> first_proof;
    std::optional<WifiScanLeaseCoordinator::RecoveryProof> second_proof;

    std::thread first_thread([&]() {
        first_proof = first_executor.Execute(first.recovery);
    });
    {
        std::unique_lock<std::mutex> lock(driver.mutex);
        driver.condition.wait(lock, []() { return driver.scan_stop_entered; });
    }
    std::thread second_thread([&]() {
        second_proof = second_executor.Execute(second.recovery);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert((Calls() == std::vector<std::string>{"scan_stop"}));
    {
        std::lock_guard<std::mutex> lock(driver.mutex);
        driver.block_scan_stop = false;
        driver.release_scan_stop = true;
    }
    driver.condition.notify_all();
    first_thread.join();
    second_thread.join();

    assert(first_proof->Proves(first.recovery));
    assert(second_proof->Proves(second.recovery));
    assert((Calls() == std::vector<std::string>{
        "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init",
        "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}));
}

void ProofCannotCrossCoordinatorIdentityWithTheSameRecoveryId() {
    driver.Reset();
    RecoveryFixture first;
    RecoveryFixture second;
    assert(first.recovery.recovery_id() == second.recovery.recovery_id());
    WifiScanRecoveryExecutor executor;

    const auto first_proof = executor.Execute(first.recovery);

    assert(!first_proof.Proves(second.recovery));
    assert(!second.coordinator.CompleteRecovery(second.lease, first_proof));
    assert(!second.coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
    const auto second_proof = executor.Execute(second.recovery);
    assert(second.coordinator.CompleteRecovery(second.lease, second_proof));
}

class RecoverySchedulerModel {
public:
    RecoverySchedulerModel(WifiScanLeaseCoordinator::Owner owner,
                           bool submission_accepted) {
        const auto acquired = coordinator_.TryAcquire(owner);
        assert(acquired.acquired);
        lease_ = acquired.lease;
        const auto commit = coordinator_.CommitSubmission(
            lease_, submission_accepted);
        assert(submission_accepted || commit.drain_required);
    }

    void StopWithDebt() {
        assert(coordinator_.BeginDrain(lease_));
    }

    void Notify() {
        ++notification_count_;
        scheduled_ = true;
    }

    bool CallbackWinsBeforeClaim() {
        const auto callback = coordinator_.ObserveScanDone(lease_);
        assert(callback.consume_now);
        assert(coordinator_.FinishCompletion(lease_));
        scheduled_ = false;
        return true;
    }

    bool RunOnce() {
        assert(scheduled_);
        if (!recovery_.has_value()) {
            recovery_ = coordinator_.BeginRecovery(lease_);
            ++claim_count_;
        }
        const auto proof = executor_.Execute(*recovery_);
        if (!proof.Proves(*recovery_)) {
            return false;
        }
        if (!coordinator_.CompleteRecovery(lease_, proof)) {
            return false;
        }
        scheduled_ = false;
        return true;
    }

    bool CanAcquire(WifiScanLeaseCoordinator::Owner owner) {
        return coordinator_.TryAcquire(owner).acquired;
    }

    size_t notification_count() const { return notification_count_; }
    size_t claim_count() const { return claim_count_; }

private:
    WifiScanLeaseCoordinator coordinator_;
    WifiScanLeaseCoordinator::Lease lease_;
    WifiScanRecoveryExecutor executor_;
    std::optional<WifiScanLeaseCoordinator::RecoveryDecision> recovery_;
    bool scheduled_ = false;
    size_t notification_count_ = 0;
    size_t claim_count_ = 0;
};

void DuplicateNotificationsCoalesceIntoOneExactRecovery() {
    driver.Reset();
    RecoverySchedulerModel model(
        WifiScanLeaseCoordinator::Owner::kStation, false);
    model.Notify();
    model.Notify();

    assert(model.RunOnce());
    assert(model.notification_count() == 2);
    assert(model.claim_count() == 1);
    assert(model.CanAcquire(WifiScanLeaseCoordinator::Owner::kConfigAp));
}

void CallbackBeforeWorkerClaimCancelsRecoveryWithoutDriverReset() {
    driver.Reset();
    RecoverySchedulerModel model(
        WifiScanLeaseCoordinator::Owner::kConfigAp, true);
    model.StopWithDebt();
    model.Notify();

    assert(model.CallbackWinsBeforeClaim());
    assert(Calls().empty());
    assert(model.claim_count() == 0);
    assert(model.CanAcquire(WifiScanLeaseCoordinator::Owner::kStation));
}

void FailedRecoveryRetainsOneDecisionAcrossRetry() {
    driver.Reset();
    driver.failure = FailureStep::kBarrier;
    RecoverySchedulerModel model(
        WifiScanLeaseCoordinator::Owner::kStation, false);
    model.Notify();

    assert(!model.RunOnce());
    assert(model.claim_count() == 1);
    assert(!model.CanAcquire(WifiScanLeaseCoordinator::Owner::kConfigAp));
    driver.failure = FailureStep::kNone;
    assert(model.RunOnce());
    assert(model.claim_count() == 1);
    assert((Calls() == std::vector<std::string>{
        "scan_stop", "wifi_stop", "wifi_deinit", "barrier",
        "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}));
}

class PendingTransitionRecoveryModel {
public:
    explicit PendingTransitionRecoveryModel(
            WifiScanLeaseCoordinator::Owner source_owner) {
        const auto acquired = coordinator_.TryAcquire(source_owner);
        assert(acquired.acquired);
        lease_ = acquired.lease;
        assert(coordinator_.CommitSubmission(lease_, true).accepted);
        assert(coordinator_.BeginDrain(lease_));
        pending_generation_ = lifecycle_generation_;
    }

    void NotifyDebt() { scheduled_ = true; }

    bool RecoverAndResume() {
        assert(scheduled_);
        if (!recovery_.has_value()) {
            recovery_ = coordinator_.BeginRecovery(lease_);
            ++claim_count_;
        }
        const auto proof = executor_.Execute(*recovery_);
        if (!proof.Proves(*recovery_) ||
            !coordinator_.CompleteRecovery(lease_, proof)) {
            return false;
        }
        driver_ready_before_target_ =
            !Calls().empty() && Calls().back() == "wifi_init";
        scheduled_ = false;
        if (lifecycle_generation_ == pending_generation_) {
            ++target_start_count_;
        }
        pending_generation_ = 0;
        return true;
    }

    bool CallbackWinsAndResume() {
        assert(scheduled_);
        const auto callback = coordinator_.ObserveScanDone(lease_);
        assert(callback.consume_now);
        assert(coordinator_.FinishCompletion(lease_));
        scheduled_ = false;
        driver_ready_before_target_ = true;
        if (lifecycle_generation_ == pending_generation_) {
            ++target_start_count_;
        }
        pending_generation_ = 0;
        return true;
    }

    void MakePendingGenerationStale() { ++lifecycle_generation_; }
    size_t target_start_count() const { return target_start_count_; }
    size_t claim_count() const { return claim_count_; }
    bool driver_ready_before_target() const {
        return driver_ready_before_target_;
    }

private:
    WifiScanLeaseCoordinator coordinator_;
    WifiScanLeaseCoordinator::Lease lease_;
    WifiScanRecoveryExecutor executor_;
    std::optional<WifiScanLeaseCoordinator::RecoveryDecision> recovery_;
    uint64_t lifecycle_generation_ = 1;
    uint64_t pending_generation_ = 0;
    bool scheduled_ = false;
    bool driver_ready_before_target_ = false;
    size_t target_start_count_ = 0;
    size_t claim_count_ = 0;
};

void BothModeDirectionsStartTargetOnlyAfterExactProofAndDriverReady() {
    for (const auto source : {
             WifiScanLeaseCoordinator::Owner::kStation,
             WifiScanLeaseCoordinator::Owner::kConfigAp}) {
        driver.Reset();
        PendingTransitionRecoveryModel model(source);
        model.NotifyDebt();
        model.NotifyDebt();

        assert(model.target_start_count() == 0);
        assert(model.RecoverAndResume());
        assert(model.claim_count() == 1);
        assert(model.driver_ready_before_target());
        assert(model.target_start_count() == 1);
    }
}

void RecoveryFailureKeepsTargetDeferredAndRetryUsesSameClaim() {
    driver.Reset();
    driver.failure = FailureStep::kWifiInit;
    PendingTransitionRecoveryModel model(
        WifiScanLeaseCoordinator::Owner::kConfigAp);
    model.NotifyDebt();

    assert(!model.RecoverAndResume());
    assert(model.target_start_count() == 0);
    assert(model.claim_count() == 1);
    driver.failure = FailureStep::kNone;
    assert(model.RecoverAndResume());
    assert(model.target_start_count() == 1);
    assert(model.claim_count() == 1);
}

void StaleTransitionGenerationNeverStartsTarget() {
    driver.Reset();
    PendingTransitionRecoveryModel model(
        WifiScanLeaseCoordinator::Owner::kStation);
    model.NotifyDebt();
    model.MakePendingGenerationStale();

    assert(model.RecoverAndResume());
    assert(model.driver_ready_before_target());
    assert(model.target_start_count() == 0);
}

void CallbackBeforeClaimResumesBothPendingModeDirectionsExactlyOnce() {
    for (const auto source : {
             WifiScanLeaseCoordinator::Owner::kStation,
             WifiScanLeaseCoordinator::Owner::kConfigAp}) {
        driver.Reset();
        PendingTransitionRecoveryModel model(source);
        model.NotifyDebt();
        model.NotifyDebt();

        assert(model.CallbackWinsAndResume());
        assert(model.claim_count() == 0);
        assert(model.driver_ready_before_target());
        assert(model.target_start_count() == 1);
        assert(Calls().empty());
    }
}

void CallbackBeforeClaimDiscardsStalePendingGeneration() {
    driver.Reset();
    PendingTransitionRecoveryModel model(
        WifiScanLeaseCoordinator::Owner::kConfigAp);
    model.NotifyDebt();
    model.MakePendingGenerationStale();

    assert(model.CallbackWinsAndResume());
    assert(model.claim_count() == 0);
    assert(model.target_start_count() == 0);
    assert(Calls().empty());
}

static_assert(!std::is_copy_constructible<WifiScanRecoveryExecutor>::value);
static_assert(!std::is_copy_assignable<WifiScanRecoveryExecutor>::value);

}  // namespace

extern "C" esp_err_t esp_wifi_scan_stop() {
    return driver.Record("scan_stop", FailureStep::kScanStop);
}

extern "C" esp_err_t esp_wifi_stop() {
    return driver.Record("wifi_stop", FailureStep::kWifiStop);
}

extern "C" esp_err_t esp_wifi_deinit() {
    return driver.Record("wifi_deinit", FailureStep::kWifiDeinit);
}

extern "C" esp_err_t esp_wifi_init(const wifi_init_config_t* config) {
    {
        std::lock_guard<std::mutex> lock(driver.mutex);
        driver.init_nvs_enabled = config->nvs_enable;
    }
    return driver.Record("wifi_init", FailureStep::kWifiInit);
}

bool DrainDefaultEventLoop(std::chrono::milliseconds timeout) {
    assert(timeout == std::chrono::milliseconds(1000));
    return driver.Record("barrier", FailureStep::kBarrier) == ESP_OK;
}

int main() {
    SuccessfulRecoveryUsesExactChoreography();
    EveryFailureReturnsNoProofAndStopsChoreography();
    NotInitializedShutdownStatesAreBenign();
    ScanStopTransientStatesContinueThroughDriverStop();
    ScanStopTransientStatesAreNotBenignForLaterStages();
    InvalidAndOverflowedRecoveryDecisionsNeverTouchTheDriver();
    ExecuteCallsAreSerialized();
    ProofCannotCrossCoordinatorIdentityWithTheSameRecoveryId();
    DuplicateNotificationsCoalesceIntoOneExactRecovery();
    CallbackBeforeWorkerClaimCancelsRecoveryWithoutDriverReset();
    FailedRecoveryRetainsOneDecisionAcrossRetry();
    BothModeDirectionsStartTargetOnlyAfterExactProofAndDriverReady();
    RecoveryFailureKeepsTargetDeferredAndRetryUsesSameClaim();
    StaleTransitionGenerationNeverStartsTarget();
    CallbackBeforeClaimResumesBothPendingModeDirectionsExactlyOnce();
    CallbackBeforeClaimDiscardsStalePendingGeneration();
    std::cout << "wifi scan recovery executor host tests passed\n";
    return 0;
}
