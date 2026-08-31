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
    assert(proof.Proves(fixture.recovery.recovery_id()));
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
        assert(!proof.Proves(fixture.recovery.recovery_id()));
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

        assert(proof.Proves(fixture.recovery.recovery_id()));
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

        assert(proof.Proves(fixture.recovery.recovery_id()));
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

            assert(!proof.Proves(fixture.recovery.recovery_id()));
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
    assert(!proof.Proves(std::numeric_limits<uint64_t>::max()));
    assert(!coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi).acquired);
}

void ExecuteCallsAreSerialized() {
    driver.Reset();
    driver.block_scan_stop = true;
    RecoveryFixture first;
    RecoveryFixture second;
    WifiScanRecoveryExecutor executor;
    std::optional<WifiScanLeaseCoordinator::RecoveryProof> first_proof;
    std::optional<WifiScanLeaseCoordinator::RecoveryProof> second_proof;

    std::thread first_thread([&]() {
        first_proof = executor.Execute(first.recovery);
    });
    {
        std::unique_lock<std::mutex> lock(driver.mutex);
        driver.condition.wait(lock, []() { return driver.scan_stop_entered; });
    }
    std::thread second_thread([&]() {
        second_proof = executor.Execute(second.recovery);
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

    assert(first_proof->Proves(first.recovery.recovery_id()));
    assert(second_proof->Proves(second.recovery.recovery_id()));
    assert((Calls() == std::vector<std::string>{
        "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init",
        "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}));
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
    std::cout << "wifi scan recovery executor host tests passed\n";
    return 0;
}
