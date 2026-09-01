#include "wifi_manager.h"
#include "wifi_configuration_ap.h"

#include <esp_wifi.h>
#include <freertos/task.h>

#include <cassert>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool Ready(WifiManager::ExternalScanRadioResult result) {
    return result == WifiManager::ExternalScanRadioResult::kReady;
}

class RecoveryDriver final : public WifiRadioRecoveryRestorer::Driver {
public:
    esp_err_t Stop() override {
        const auto result = Call("stop");
        if (result == ESP_OK || result == ESP_ERR_WIFI_NOT_STARTED) {
            started_ = false;
        }
        return result;
    }
    esp_err_t SetMode(wifi_mode_t) override {
        return started_ ? ESP_ERR_WIFI_STATE : Call("mode");
    }
    esp_err_t SetConfig(wifi_interface_t, wifi_config_t*) override {
        return started_ ? ESP_ERR_WIFI_STATE : Call("config");
    }
    esp_err_t SetPowerSave(wifi_ps_type_t) override { return Call("ps"); }
    esp_err_t Start() override {
        const auto result = Call("start");
        if (result == ESP_OK) {
            started_ = true;
        }
        return result;
    }
    esp_err_t SetBandMode(wifi_band_mode_t) override { return Call("band"); }
    esp_err_t SetMaxTxPower(int8_t) override { return Call("max_tx"); }

    void FailOnceAt(const std::string& stage) {
        failures_[stage].push_back(ESP_FAIL);
    }
    void ClearCalls() { calls_.clear(); }
    const std::vector<std::string>& Calls() const { return calls_; }

private:
    esp_err_t Call(const std::string& name) {
        calls_.push_back(name);
        auto& failures = failures_[name];
        if (failures.empty()) {
            return ESP_OK;
        }
        const auto result = failures.front();
        failures.pop_front();
        return result;
    }

    bool started_ = false;
    std::map<std::string, std::deque<esp_err_t>> failures_;
    std::vector<std::string> calls_;
};

RecoveryDriver recovery_driver;
int wifi_init_calls = 0;
int task_create_calls = 0;
int task_notify_calls = 0;
bool fail_task_create_once = false;
bool fail_task_notify_once = false;
bool fail_executor_once = false;
WifiManager* lock_probe_manager = nullptr;
int lock_probe_calls = 0;
int scan_retry_poll_calls = 0;
std::vector<std::string> direct_wifi_calls;
wifi_mode_t direct_wifi_mode = WIFI_MODE_NULL;
esp_err_t direct_wifi_start_result = ESP_OK;
esp_err_t direct_wifi_inactive_time_result = ESP_ERR_WIFI_NOT_STARTED;
esp_err_t direct_wifi_get_config_result = ESP_OK;
WifiManager* direct_wifi_lifecycle_probe_manager = nullptr;
std::map<std::string, std::deque<esp_err_t>> direct_wifi_failures;

struct DelayAbort : std::runtime_error {
    DelayAbort() : std::runtime_error("retry") {}
};

void ProbeManagerLock() {
    if (lock_probe_manager != nullptr) {
        assert(lock_probe_manager->IsInitialized());
        ++lock_probe_calls;
    }
}

void RunUntilYield(WifiManager& manager) {
    try {
        manager.TestRunScanRecovery();
    } catch (const DelayAbort&) {
    }
}

void FailDirectOnceAt(const std::string& stage) {
    direct_wifi_failures[stage].push_back(ESP_FAIL);
}

void QueueDirectResult(const std::string& stage, esp_err_t result) {
    direct_wifi_failures[stage].push_back(result);
}

esp_err_t DirectResult(const std::string& stage,
                       esp_err_t fallback = ESP_OK) {
    auto& failures = direct_wifi_failures[stage];
    if (failures.empty()) {
        return fallback;
    }
    const auto result = failures.front();
    failures.pop_front();
    return result;
}

void RegisterBlockingUiRecovery(WifiManager& manager,
                                WifiScanLeaseCoordinator& coordinator,
                                bool& debt, int& retry_calls) {
    WifiManager::ScanRecoveryOwnerHooks hooks;
    hooks.claim = [&](const auto& lease)
            -> std::optional<WifiScanLeaseCoordinator::RecoveryDecision> {
        const auto recovery = coordinator.BeginRecovery(lease);
        return recovery.begun()
            ? std::optional<WifiScanLeaseCoordinator::RecoveryDecision>(
                  recovery)
            : std::nullopt;
    };
    hooks.has_debt = [&](const auto&) { return debt; };
    hooks.restore_radio = [](const auto&) { return true; };
    hooks.complete = [&](const auto& lease, const auto& proof) {
        if (!coordinator.CompleteRecovery(lease, proof)) {
            return false;
        }
        debt = false;
        return true;
    };
    hooks.retry = [&](const auto&) { ++retry_calls; };
    assert(manager.RegisterScanRecoveryOwner(
        WifiScanLeaseCoordinator::Owner::kBlockingUi, std::move(hooks)));
}

void AssertExternalRecoveryStillOwnsGlobalLease(
        WifiManager& manager, WifiScanLeaseCoordinator& coordinator,
        const WifiScanLeaseCoordinator::Lease& lease) {
    assert(manager.TestRecoveryActive());
    assert(manager.TestOwnsExternalScanToken(lease));
    manager.StartStation();
    assert(!manager.TestStationActive());
    assert(!manager.TestConfigActive());
    assert(!coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation).acquired);
}

void TaskCreationFailureRetriesOnlyFailedStage() {
    auto* manager = new WifiManager;
    const auto before_init = manager->ScanLeaseCoordinator().TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(before_init.acquired);
    assert(manager->ScanLeaseCoordinator()
        .CommitSubmission(before_init.lease, false).drain_required);
    assert(!manager->RequestScanRecovery(before_init.lease));
    fail_task_create_once = true;
    assert(!manager->Initialize());
    auto* station = manager->TestStation();
    assert(station != nullptr);
    assert(wifi_init_calls == 1);
    assert(manager->Initialize());
    assert(manager->RequestScanRecovery(before_init.lease));
    assert(manager->TestStation() == station);
    assert(wifi_init_calls == 1);
    assert(task_create_calls == 2);
}

void OneShotStationDebtSurvivesWorkerNotificationFailure() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();
    fail_task_notify_once = true;
    station->PublishDebt();
    assert(manager->TestRecoveryActive());
    // The production worker's bounded wait reaches this path even though the
    // one-shot publisher never calls RequestScanRecovery again.
    manager->TestRunScanRecovery();
    assert(!manager->TestRecoveryActive());
    assert(station->RestoreCalls() == 1);
    assert(station->RetryCalls() == 1);
}

void ScanRetryPollerSurvivesNotificationFailure() {
    auto* manager = new WifiManager;
    assert(manager->RegisterScanLeaseRetryPoller(
        nullptr, [](void*) noexcept { ++scan_retry_poll_calls; }));
    assert(manager->Initialize());
    fail_task_notify_once = true;
    assert(!manager->RequestScanLeaseRetryPoll());
    manager->TestPollScanLeaseRetry();
    assert(scan_retry_poll_calls == 1);
}

void DuplicateDebtCoalescesAndProductionRestoreRetries() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();
    const auto lease = station->PublishDebt();
    manager->TestScheduleScanRecovery(lease);
    manager->TestScheduleScanRecovery(lease);
    assert(task_notify_calls >= 1);
    station->FailRestoreAt("max_tx");
    RunUntilYield(*manager);
    assert(manager->TestRecoveryActive());
    assert(station->RestoreCalls() == 1);
    assert(!station->ScansEnabled());
    recovery_driver.ClearCalls();
    manager->TestRunScanRecovery();
    assert((recovery_driver.Calls() == std::vector<std::string>{
        "stop", "mode", "start", "max_tx", "ps"}));
    assert(!manager->TestRecoveryActive());
    assert(station->RestoreCalls() == 2);
    assert(station->ScansEnabled());
    assert(station->RetryCalls() == 1);
}

void ExecutorFailureRetainsClaimAndRetriesWithoutReclaiming() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();
    station->PublishDebt();
    fail_executor_once = true;
    RunUntilYield(*manager);
    assert(manager->TestRecoveryActive());
    assert(station->RestoreCalls() == 0);
    manager->TestRunScanRecovery();
    assert(!manager->TestRecoveryActive());
    assert(station->RestoreCalls() == 1);
    assert(station->ScansEnabled());
}

void ProofCompletionFailureKeepsScansGatedAndRestoresAgain() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* config = manager->TestConfigAp();
    const auto lease = config->PublishDebt();
    manager->TestScheduleScanRecovery(lease);
    config->FailCompletionOnce();
    RunUntilYield(*manager);
    assert(config->RestoreCalls() == 1);
    assert(!config->ScansEnabled());
    config->FailRestoreAt("band");
    RunUntilYield(*manager);
    assert(config->RestoreCalls() == 2);
    assert(!config->ScansEnabled());
    recovery_driver.ClearCalls();
    manager->TestRunScanRecovery();
    assert((recovery_driver.Calls() == std::vector<std::string>{
        "stop", "mode", "config", "ps", "start", "band", "max_tx"}));
    assert(config->RestoreCalls() == 3);
    assert(config->ScansEnabled());
}

void CallbackBeforeClaimCancelsRecoveryWithoutRestore() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();
    const auto lease = station->PublishDebt();
    manager->TestScheduleScanRecovery(lease);
    station->CallbackWins();
    manager->TestRunScanRecovery();
    assert(!manager->TestRecoveryActive());
    assert(station->RestoreCalls() == 0);
}

void RegisteredBlufiOwnerUsesSharedRecoveryExecutor() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto acquired = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(acquired.acquired);
    assert(coordinator.CommitSubmission(acquired.lease, false).drain_required);

    bool debt = true;
    int restore_calls = 0;
    int retry_calls = 0;
    WifiManager::ScanRecoveryOwnerHooks hooks;
    hooks.claim = [&](const auto& lease)
                -> std::optional<WifiScanLeaseCoordinator::RecoveryDecision> {
                const auto recovery = coordinator.BeginRecovery(lease);
                return recovery.begun()
                    ? std::optional<WifiScanLeaseCoordinator::RecoveryDecision>(
                          recovery)
                    : std::nullopt;
            };
    hooks.has_debt = [&](const auto&) { return debt; };
    hooks.restore_radio = [&](const auto&) {
                ++restore_calls;
                ProbeManagerLock();
                return true;
            };
    hooks.complete = [&](const auto& lease, const auto& proof) {
                debt = false;
                return coordinator.CompleteRecovery(lease, proof);
            };
    hooks.retry = [&](const auto&) { ++retry_calls; };
    assert(manager->RegisterScanRecoveryOwner(
        WifiScanLeaseCoordinator::Owner::kBlufi, std::move(hooks)));
    lock_probe_manager = manager;
    manager->TestScheduleScanRecovery(acquired.lease);
    manager->TestRunScanRecovery();
    lock_probe_manager = nullptr;
    assert(!manager->TestRecoveryActive());
    assert(restore_calls == 1);
    assert(retry_calls == 1);
    assert(lock_probe_calls > 0);
    assert(coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation).acquired);
}

void BlockingUiRecoveryRestoresManagerStationRoleAndRetriesAfterProof() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    manager->StartStation();
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto acquired = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi);
    assert(acquired.acquired);
    assert(Ready(manager->PrepareExternalScanRadio(acquired.lease)));
    assert(coordinator.CommitSubmission(acquired.lease, false).drain_required);

    bool debt = true;
    int owner_retry_calls = 0;
    WifiManager::ScanRecoveryOwnerHooks hooks;
    hooks.claim = [&](const auto& lease)
                -> std::optional<WifiScanLeaseCoordinator::RecoveryDecision> {
                const auto recovery = coordinator.BeginRecovery(lease);
                return recovery.begun()
                    ? std::optional<WifiScanLeaseCoordinator::RecoveryDecision>(
                          recovery)
                    : std::nullopt;
            };
    hooks.has_debt = [&](const auto&) { return debt; };
    hooks.restore_radio = [](const auto&) { return true; };
    hooks.complete = [&](const auto& lease, const auto& proof) {
                assert(manager->TestStation()->ExternalReconnectCalls() == 0);
                if (!coordinator.CompleteRecovery(lease, proof)) {
                    return false;
                }
                debt = false;
                return true;
            };
    hooks.retry = [&](const auto& lease) {
                assert(lease.lease_id == acquired.lease.lease_id);
                ++owner_retry_calls;
            };
    assert(manager->RegisterScanRecoveryOwner(
        WifiScanLeaseCoordinator::Owner::kBlockingUi, std::move(hooks)));

    manager->TestScheduleScanRecovery(acquired.lease);
    recovery_driver.ClearCalls();
    manager->TestRunScanRecovery();
    assert((recovery_driver.Calls() == std::vector<std::string>{
        "stop", "mode", "config", "start", "band", "max_tx", "ps"}));
    assert(manager->TestStation()->ExternalRestoreCalls() == 1);
    assert(manager->TestStation()->ExternalReconnectCalls() == 1);
    assert(manager->TestStation()->RetryCalls() == 1);
    assert(owner_retry_calls == 1);
}

void BlockingUiRecoveryRestoresConfigApAndIdleRolesExactly() {
    auto* config_manager = new WifiManager;
    assert(config_manager->Initialize());
    config_manager->StartConfigAp();
    auto& config_coordinator = config_manager->ScanLeaseCoordinator();
    const auto config_lease = config_coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    assert(Ready(config_manager->PrepareExternalScanRadio(config_lease)));
    assert(config_manager->TestExternalRecoveryRole(config_lease) ==
           WifiManager::ExternalScanRecoveryRole::kConfigAp);
    assert(config_coordinator.CommitSubmission(config_lease, false)
               .drain_required);
    bool config_debt = true;
    WifiManager::ScanRecoveryOwnerHooks config_hooks;
    config_hooks.claim = [&](const auto& lease)
            -> std::optional<WifiScanLeaseCoordinator::RecoveryDecision> {
        const auto recovery = config_coordinator.BeginRecovery(lease);
        return recovery.begun()
            ? std::optional<WifiScanLeaseCoordinator::RecoveryDecision>(
                  recovery)
            : std::nullopt;
    };
    config_hooks.has_debt = [&](const auto&) { return config_debt; };
    config_hooks.restore_radio = [](const auto&) { return true; };
    config_hooks.complete = [&](const auto& lease, const auto& proof) {
        if (!config_coordinator.CompleteRecovery(lease, proof)) {
            return false;
        }
        config_debt = false;
        return true;
    };
    config_hooks.retry = [](const auto&) {};
    assert(config_manager->RegisterScanRecoveryOwner(
        WifiScanLeaseCoordinator::Owner::kBlockingUi,
        std::move(config_hooks)));
    config_manager->TestScheduleScanRecovery(config_lease);
    config_manager->TestRunScanRecovery();
    assert(config_manager->TestConfigAp()->ExternalRestoreCalls() == 1);
    assert(config_manager->TestConfigAp()->RetryCalls() == 1);

    auto* idle_manager = new WifiManager;
    assert(idle_manager->Initialize());
    auto& idle_coordinator = idle_manager->ScanLeaseCoordinator();
    const auto idle_lease = idle_coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    assert(Ready(idle_manager->PrepareExternalScanRadio(idle_lease)));
    assert(idle_manager->TestExternalRecoveryRole(idle_lease) ==
           WifiManager::ExternalScanRecoveryRole::kIdle);
    assert(idle_coordinator.CommitSubmission(idle_lease, false)
               .drain_required);
    bool idle_debt = true;
    int idle_retry_calls = 0;
    WifiManager::ScanRecoveryOwnerHooks idle_hooks;
    idle_hooks.claim = [&](const auto& lease)
            -> std::optional<WifiScanLeaseCoordinator::RecoveryDecision> {
        const auto recovery = idle_coordinator.BeginRecovery(lease);
        return recovery.begun()
            ? std::optional<WifiScanLeaseCoordinator::RecoveryDecision>(
                  recovery)
            : std::nullopt;
    };
    idle_hooks.has_debt = [&](const auto&) { return idle_debt; };
    idle_hooks.restore_radio = [](const auto&) { return false; };
    idle_hooks.complete = [&](const auto& lease, const auto& proof) {
        if (!idle_coordinator.CompleteRecovery(lease, proof)) {
            return false;
        }
        idle_debt = false;
        return true;
    };
    idle_hooks.retry = [&](const auto&) { ++idle_retry_calls; };
    assert(idle_manager->RegisterScanRecoveryOwner(
        WifiScanLeaseCoordinator::Owner::kBlockingUi,
        std::move(idle_hooks)));
    idle_manager->TestScheduleScanRecovery(idle_lease);
    direct_wifi_calls.clear();
    idle_manager->TestRunScanRecovery();
    assert((direct_wifi_calls == std::vector<std::string>{
        "stop", "set_mode_sta", "set_ps", "start", "set_band", "stop",
        "set_mode_restore"}));
    assert(idle_manager->TestStation()->ExternalRestoreCalls() == 0);
    assert(idle_manager->TestConfigAp()->ExternalRestoreCalls() == 0);
    assert(idle_retry_calls == 1);
}

void RecoveryDebtSurvivesTemporarilyUnavailableTask() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();
    manager->TestSetScanRecoveryTaskAvailable(false);
    const auto lease = station->PublishDebt();
    assert(lease.lease_id != 0);
    assert(manager->TestRecoveryActive());
    manager->TestSetScanRecoveryTaskAvailable(true);
    manager->TestRunScanRecovery();
    assert(!manager->TestRecoveryActive());
    assert(station->RestoreCalls() == 1);
}

void LifecycleTransitionCannotInvalidateCapturedExternalRole() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    manager->StartStation();
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    assert(Ready(manager->PrepareExternalScanRadio(lease)));
    manager->StopStation();
    assert(manager->TestStationActive());
    assert(manager->TestExternalRecoveryRole(lease) ==
           WifiManager::ExternalScanRecoveryRole::kStation);
    assert(Ready(manager->FinishExternalScanRadio(lease)));
    assert(manager->ReleaseExternalScanRadioToken(lease));
    assert(coordinator.AbandonUnsubmitted(lease));
}

void IdleBlockingUiPreparationStartsAndRestoresStoppedRadio() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    direct_wifi_calls.clear();
    assert(Ready(manager->PrepareExternalScanRadio(lease)));
    assert((direct_wifi_calls == std::vector<std::string>{
        "get_mode", "get_ps", "get_band", "set_mode_sta",
        "start"}));
    assert(Ready(manager->FinishExternalScanRadio(lease)));
    assert(manager->ReleaseExternalScanRadioToken(lease));
    assert((direct_wifi_calls == std::vector<std::string>{
        "get_mode", "get_ps", "get_band", "set_mode_sta",
        "start", "stop", "set_mode_restore"}));
    assert(coordinator.AbandonUnsubmitted(lease));
}

void IdleBlockingUiPreparationRollsBackModeWhenStartFails() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    direct_wifi_calls.clear();
    direct_wifi_mode = WIFI_MODE_NULL;
    direct_wifi_start_result = ESP_FAIL;
    assert(manager->PrepareExternalScanRadio(lease) ==
           WifiManager::ExternalScanRadioResult::kCleanFailure);
    direct_wifi_start_result = ESP_OK;
    assert((direct_wifi_calls == std::vector<std::string>{
        "get_mode", "get_ps", "get_band", "set_mode_sta",
        "start", "stop", "set_mode_restore"}));
    assert(coordinator.AbandonUnsubmitted(lease));
}

void IdlePreparationRollbackFailureRetainsExactLeaseUntilRecovery() {
    for (const auto& failure : {std::string("stop"),
                                std::string("set_mode_restore")}) {
        auto* manager = new WifiManager;
        assert(manager->Initialize());
        auto& coordinator = manager->ScanLeaseCoordinator();
        const auto lease = coordinator.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
        direct_wifi_calls.clear();
        direct_wifi_mode = WIFI_MODE_NULL;
        direct_wifi_start_result = ESP_FAIL;
        FailDirectOnceAt(failure);
        assert(manager->PrepareExternalScanRadio(lease) ==
               WifiManager::ExternalScanRadioResult::kRecoveryRequired);
        direct_wifi_start_result = ESP_OK;

        bool debt = true;
        int retry_calls = 0;
        RegisterBlockingUiRecovery(*manager, coordinator, debt, retry_calls);
        assert(coordinator.CommitSubmission(lease, false).drain_required);
        assert(manager->RequestScanRecovery(lease));
        AssertExternalRecoveryStillOwnsGlobalLease(
            *manager, coordinator, lease);
        manager->TestRunScanRecovery();
        assert(!manager->TestRecoveryActive());
        assert(!manager->TestOwnsExternalScanToken(lease));
        manager->StartStation();
        assert(manager->TestStationActive());
        assert(retry_calls == 1);
        assert(coordinator.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kStation).acquired);
    }
}

void IdleFinishRestartFailureRetainsExactLeaseUntilRecovery() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    direct_wifi_calls.clear();
    direct_wifi_mode = WIFI_MODE_AP;
    direct_wifi_inactive_time_result = ESP_OK;
    assert(Ready(manager->PrepareExternalScanRadio(lease)));
    FailDirectOnceAt("start");
    assert(manager->FinishExternalScanRadio(lease) ==
           WifiManager::ExternalScanRadioResult::kRecoveryRequired);

    bool debt = true;
    int retry_calls = 0;
    RegisterBlockingUiRecovery(*manager, coordinator, debt, retry_calls);
    assert(coordinator.CommitSubmission(lease, false).drain_required);
    assert(manager->RequestScanRecovery(lease));
    AssertExternalRecoveryStillOwnsGlobalLease(*manager, coordinator, lease);
    manager->TestRunScanRecovery();
    assert(!manager->TestRecoveryActive());
    assert(!manager->TestOwnsExternalScanToken(lease));
    manager->StartStation();
    assert(manager->TestStationActive());
    assert(retry_calls == 1);
    direct_wifi_mode = WIFI_MODE_NULL;
    direct_wifi_inactive_time_result = ESP_ERR_WIFI_NOT_STARTED;
}

void IdlePostResetRestoreFailuresKeepLifecycleAndLeaseBlocked() {
    for (const auto& failure : {
             std::string("set_mode_sta"), std::string("set_ps"),
             std::string("start"),
             std::string("set_band"), std::string("set_tx"),
             std::string("final_stop"),
             std::string("set_mode_restore")}) {
        auto* manager = new WifiManager;
        assert(manager->Initialize());
        auto& coordinator = manager->ScanLeaseCoordinator();
        const auto lease = coordinator.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
        direct_wifi_calls.clear();
        const bool restore_started_radio = failure == "set_tx";
        direct_wifi_mode = restore_started_radio ? WIFI_MODE_AP
                                                 : WIFI_MODE_NULL;
        direct_wifi_inactive_time_result = restore_started_radio
            ? ESP_OK : ESP_ERR_WIFI_NOT_STARTED;
        assert(Ready(manager->PrepareExternalScanRadio(lease)));

        bool debt = true;
        int retry_calls = 0;
        RegisterBlockingUiRecovery(*manager, coordinator, debt, retry_calls);
        assert(coordinator.CommitSubmission(lease, false).drain_required);
        assert(manager->RequestScanRecovery(lease));
        if (failure == "final_stop") {
            QueueDirectResult("stop", ESP_OK);
            FailDirectOnceAt("stop");
        } else {
            FailDirectOnceAt(failure);
        }
        RunUntilYield(*manager);
        AssertExternalRecoveryStillOwnsGlobalLease(
            *manager, coordinator, lease);
        manager->TestRunScanRecovery();
        assert(!manager->TestRecoveryActive());
        assert(!manager->TestOwnsExternalScanToken(lease));
        manager->StartStation();
        assert(manager->TestStationActive());
        assert(retry_calls == 1);
        assert(coordinator.TryAcquire(
            WifiScanLeaseCoordinator::Owner::kStation).acquired);
        direct_wifi_mode = WIFI_MODE_NULL;
        direct_wifi_inactive_time_result = ESP_ERR_WIFI_NOT_STARTED;
    }
}

void IdleBlockingUiPreparationLeavesRunningStaUndisrupted() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    direct_wifi_calls.clear();
    direct_wifi_mode = WIFI_MODE_STA;
    direct_wifi_inactive_time_result = ESP_OK;
    assert(Ready(manager->PrepareExternalScanRadio(lease)));
    assert(Ready(manager->FinishExternalScanRadio(lease)));
    assert(manager->ReleaseExternalScanRadioToken(lease));
    assert((direct_wifi_calls == std::vector<std::string>{
        "get_mode", "get_sta", "get_ps", "get_band",
        "get_inactive_sta", "get_tx"}));
    direct_wifi_mode = WIFI_MODE_NULL;
    direct_wifi_inactive_time_result = ESP_ERR_WIFI_NOT_STARTED;
    assert(coordinator.AbandonUnsubmitted(lease));
}

void IdleBlockingUiPreparationRestoresRunningApMode() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    direct_wifi_calls.clear();
    direct_wifi_mode = WIFI_MODE_AP;
    direct_wifi_inactive_time_result = ESP_OK;
    assert(Ready(manager->PrepareExternalScanRadio(lease)));
    assert(Ready(manager->FinishExternalScanRadio(lease)));
    assert(manager->ReleaseExternalScanRadioToken(lease));
    assert((direct_wifi_calls == std::vector<std::string>{
        "get_mode", "get_ap", "get_ps", "get_band",
        "get_inactive_ap", "get_tx", "set_mode_apsta", "stop", "set_mode_ap",
        "start"}));
    direct_wifi_mode = WIFI_MODE_NULL;
    direct_wifi_inactive_time_result = ESP_ERR_WIFI_NOT_STARTED;
    assert(coordinator.AbandonUnsubmitted(lease));
}

void IdlePreparationReservationBlocksLifecycleTransitions() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    direct_wifi_calls.clear();
    direct_wifi_lifecycle_probe_manager = manager;
    assert(Ready(manager->PrepareExternalScanRadio(lease)));
    direct_wifi_lifecycle_probe_manager = nullptr;
    assert(!manager->TestStationActive());
    assert(Ready(manager->FinishExternalScanRadio(lease)));
    assert(manager->ReleaseExternalScanRadioToken(lease));
    assert(coordinator.AbandonUnsubmitted(lease));
}

void IdlePreparationRejectsIncompleteInterfaceSnapshot() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto lease = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlockingUi).lease;
    direct_wifi_calls.clear();
    direct_wifi_mode = WIFI_MODE_STA;
    direct_wifi_get_config_result = ESP_FAIL;
    assert(manager->PrepareExternalScanRadio(lease) ==
           WifiManager::ExternalScanRadioResult::kCleanFailure);
    direct_wifi_mode = WIFI_MODE_NULL;
    direct_wifi_get_config_result = ESP_OK;
    assert((direct_wifi_calls == std::vector<std::string>{
        "get_mode", "get_sta"}));
    manager->StartStation();
    assert(manager->TestStationActive());
    assert(coordinator.AbandonUnsubmitted(lease));
}

void NewUnclaimedDebtRetargetsButClaimedDebtRejectsReplacement() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();
    auto* config = manager->TestConfigAp();
    station->PublishDebt();
    station->CallbackWins();
    config->PublishDebt();
    manager->TestRunScanRecovery();
    assert(station->RestoreCalls() == 0);
    assert(config->RestoreCalls() == 1);

    const auto lease = station->PublishDebt();
    station->FailRestoreAt("ps");
    RunUntilYield(*manager);
    WifiScanLeaseCoordinator::Lease conflicting{
        WifiScanLeaseCoordinator::Owner::kConfigAp,
        lease.lease_id + 100, lease.driver_incarnation};
    manager->TestScheduleScanRecovery(conflicting);
    manager->TestRunScanRecovery();
    assert(station->RestoreCalls() == 2);
    assert(config->RestoreCalls() == 1);
}

void RecoveryHooksRunWithoutManagerMutex() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    lock_probe_manager = manager;
    lock_probe_calls = 0;
    manager->TestStation()->PublishDebt();
    manager->TestRunScanRecovery();
    lock_probe_manager = nullptr;
    assert(lock_probe_calls == 3);
}

void BothPendingTransitionsResumeOnlyAfterRecovery() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    manager->StartStation();
    assert(manager->TestStationActive());
    auto* station = manager->TestStation();
    station->PublishDebtOnStop();
    manager->StartConfigAp();
    assert(!manager->TestStationActive() && !manager->TestConfigActive());
    manager->TestRunScanRecovery();
    assert(manager->TestConfigActive());

    auto* config = manager->TestConfigAp();
    config->PublishDebtOnStop();
    manager->StartStation();
    assert(!manager->TestStationActive() && !manager->TestConfigActive());
    manager->TestRunScanRecovery();
    assert(manager->TestStationActive());
}

void StalePendingGenerationDoesNotStartTarget() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    manager->StartStation();
    auto* station = manager->TestStation();
    station->PublishDebtOnStop();
    manager->StartConfigAp();
    manager->TestAdvanceLifecycleGeneration();
    manager->TestRunScanRecovery();
    assert(!manager->TestStationActive());
    assert(!manager->TestConfigActive());
}

void LifecycleReservationStartsOnlyFreshGenerationAndReleasesExactly() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto& coordinator = manager->ScanLeaseCoordinator();
    const auto scan = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(scan.acquired);
    assert(manager->StartStationIfScanIdle() ==
           WifiManager::StationStartResult::kBusyOrFailed);
    assert(coordinator.AbandonUnsubmitted(scan.lease));

    assert(manager->StartStationIfScanIdle() ==
           WifiManager::StationStartResult::kStartedNow);
    assert(manager->TestStationActive());
    const auto active_scan = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(active_scan.acquired);
    assert(manager->StartStationIfScanIdle() ==
           WifiManager::StationStartResult::kBusyOrFailed);
    assert(coordinator.AbandonUnsubmitted(active_scan.lease));
    const auto after_start = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(after_start.acquired);
    assert(coordinator.AbandonUnsubmitted(after_start.lease));

    assert(manager->StartStationIfScanIdle() ==
           WifiManager::StationStartResult::kAlreadyActive);
    const auto after_stale = coordinator.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(after_stale.acquired);
}

void LifecycleReservationRejectsAnotherCallersGeneration() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    manager->TestSetBeforeReservedStationStartHook([manager]() {
        manager->StartStation();
    });
    assert(manager->StartStationIfScanIdle() ==
           WifiManager::StationStartResult::kAlreadyActive);
    assert(manager->TestStationActive());
    const auto after_foreign_start = manager->ScanLeaseCoordinator().TryAcquire(
        WifiScanLeaseCoordinator::Owner::kBlufi);
    assert(after_foreign_start.acquired);
}

void ExactCredentialsRestartActiveStationUnderLifecycleLease() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    assert(manager->StartStationIfScanIdle() ==
           WifiManager::StationStartResult::kStartedNow);
    auto* station = manager->TestStation();
    const int starts_before = station->Starts();
    const int stops_before = station->Stops();
    assert(manager->StartStationWithCredentialsIfScanIdle(
               "target", "corrected-password") ==
           WifiManager::StationStartResult::kStartedNow);
    assert(station->Stops() == stops_before + 1);
    assert(station->Starts() == starts_before + 1);
    assert(station->ExactModeStarts() == 1);
    assert(station->ExactCredentialStarts() == 1);
    assert(station->ExactSsid() == "target");
    assert(station->ExactPassword() == "corrected-password");
    assert(!station->AutomaticScansEnabled());
    manager->EnableStationAutomaticScans();
    assert(station->AutomaticScansEnabled());
}

void InvalidExactCredentialsAreRejectedBeforeStationStart() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();

    assert(manager->StartStationWithCredentialsIfScanIdle(
               std::string(33, 's'), "password") ==
           WifiManager::StationStartResult::kInvalidCredentials);
    assert(manager->StartStationWithCredentialsIfScanIdle(
               "target", std::string(64, 'p')) ==
           WifiManager::StationStartResult::kInvalidCredentials);
    assert(station->Starts() == 0);
    assert(station->ExactModeStarts() == 0);
    assert(station->ExactCredentialStarts() == 0);
    assert(station->Stops() == 0);
    assert(!manager->TestStationActive());
}

void ExactConnectionFailureStopsPartiallyStartedStationExactlyOnce() {
    auto* manager = new WifiManager;
    assert(manager->Initialize());
    auto* station = manager->TestStation();
    station->FailExactConnectionOnce();

    assert(manager->StartStationWithCredentialsIfScanIdle(
               "target", "password") ==
           WifiManager::StationStartResult::kBusyOrFailed);
    assert(station->ExactModeStarts() == 1);
    assert(station->ExactCredentialStarts() == 1);
    assert(station->Stops() == 1);
    assert(!manager->TestStationActive());
}

}  // namespace

WifiRadioRecoveryRestorer::Driver& TestWifiRecoveryDriver() {
    return recovery_driver;
}

BaseType_t xTaskCreate(TaskFunction_t, const char*, uint32_t, void*, int,
                       TaskHandle_t* handle) {
    ++task_create_calls;
    if (fail_task_create_once) {
        fail_task_create_once = false;
        return 0;
    }
    *handle = reinterpret_cast<void*>(0x1);
    return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t) {
    ++task_notify_calls;
    if (fail_task_notify_once) {
        fail_task_notify_once = false;
        return 0;
    }
    return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 1; }
void vTaskDelay(TickType_t) { throw DelayAbort(); }
esp_err_t nvs_flash_init() { return ESP_OK; }
esp_err_t nvs_flash_erase() { return ESP_OK; }
esp_err_t esp_netif_init() { return ESP_OK; }
esp_err_t esp_event_loop_create_default() { return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t*) {
    ++wifi_init_calls;
    return ESP_OK;
}
esp_err_t esp_wifi_get_mode(wifi_mode_t* mode) {
    direct_wifi_calls.push_back("get_mode");
    if (direct_wifi_lifecycle_probe_manager != nullptr) {
        direct_wifi_lifecycle_probe_manager->StartStation();
    }
    *mode = direct_wifi_mode;
    return ESP_OK;
}
esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t*) {
    direct_wifi_calls.push_back(
        interface == WIFI_IF_STA ? "get_sta" : "get_ap");
    return direct_wifi_get_config_result;
}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t* ps) {
    direct_wifi_calls.push_back("get_ps");
    *ps = WIFI_PS_MIN_MODEM;
    return ESP_OK;
}
esp_err_t esp_wifi_get_max_tx_power(int8_t* power) {
    direct_wifi_calls.push_back("get_tx");
    if (direct_wifi_inactive_time_result == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_ERR_WIFI_NOT_STARTED;
    }
    *power = 72;
    return ESP_OK;
}
esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t* band) {
    direct_wifi_calls.push_back("get_band");
    *band = WIFI_BAND_MODE_2G_ONLY;
    return ESP_OK;
}
esp_err_t esp_wifi_get_inactive_time(wifi_interface_t interface,
                                     uint16_t* seconds) {
    direct_wifi_calls.push_back(
        interface == WIFI_IF_STA ? "get_inactive_sta" : "get_inactive_ap");
    *seconds = 300;
    return direct_wifi_inactive_time_result;
}
esp_err_t esp_wifi_stop() {
    direct_wifi_calls.push_back("stop");
    return DirectResult("stop");
}
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) {
    const char* stage = "set_mode_restore";
    if (mode == WIFI_MODE_STA) {
        direct_wifi_calls.push_back("set_mode_sta");
        stage = "set_mode_sta";
    } else if (mode == WIFI_MODE_AP) {
        direct_wifi_calls.push_back("set_mode_ap");
        stage = "set_mode_ap";
    } else if (mode == WIFI_MODE_APSTA) {
        direct_wifi_calls.push_back("set_mode_apsta");
        stage = "set_mode_apsta";
    } else {
        direct_wifi_calls.push_back("set_mode_restore");
    }
    return DirectResult(stage);
}
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t*) {
    direct_wifi_calls.push_back(
        interface == WIFI_IF_STA ? "set_sta" : "set_ap");
    return ESP_OK;
}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t) {
    direct_wifi_calls.push_back("set_ps");
    return DirectResult("set_ps");
}
esp_err_t esp_wifi_start() {
    direct_wifi_calls.push_back("start");
    return DirectResult("start", direct_wifi_start_result);
}
esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t) {
    direct_wifi_calls.push_back("set_band");
    return DirectResult("set_band");
}
esp_err_t esp_wifi_set_max_tx_power(int8_t) {
    direct_wifi_calls.push_back("set_tx");
    return DirectResult("set_tx");
}
esp_err_t esp_read_mac(uint8_t*, int) { return ESP_FAIL; }
const char* esp_err_to_name(esp_err_t) { return "host"; }

WifiStation::WifiStation(WifiScanLeaseCoordinator& coordinator)
    : coordinator_(coordinator), restorer_(TestWifiRecoveryDriver()) {}
void WifiStation::OnScanRecoveryNeeded(
        std::function<void(const WifiScanLeaseCoordinator::Lease&)> cb) {
    recovery_cb_ = std::move(cb);
}
WifiScanLeaseCoordinator::Lease WifiStation::PublishDebt(bool enabled) {
    const auto acquired = coordinator_.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kStation);
    assert(acquired.acquired);
    assert(coordinator_.CommitSubmission(acquired.lease, false).drain_required);
    scan_lease_ = acquired.lease;
    recovery_lease_ = acquired.lease;
    scans_enabled_ = enabled;
    recovery_cb_(acquired.lease);
    return acquired.lease;
}
std::optional<WifiStation::ScanRecoveryClaim> WifiStation::ClaimScanRecovery(
        const WifiScanLeaseCoordinator::Lease& lease) {
    ProbeManagerLock();
    return WifiScanRecoveryGate::TryClaim(
        coordinator_, lease, scan_lease_, recovery_lease_, scan_session_id_,
        scans_enabled_, restore_state_);
}
bool WifiStation::HasScanRecoveryDebt(
        const WifiScanLeaseCoordinator::Lease& lease) const {
    return WifiScanRecoveryGate::HasDebt(lease, recovery_lease_);
}
bool WifiStation::RestoreRadioAfterRecovery(const ScanRecoveryClaim& claim) {
    ProbeManagerLock();
    ++restore_calls_;
    const bool restored = restorer_.RestoreStation(WIFI_PS_MIN_MODEM, 72);
    return WifiScanRecoveryGate::MarkRestored(
        claim, scan_session_id_, restored, restore_state_);
}
bool WifiStation::CompleteScanRecovery(
        const ScanRecoveryClaim& claim,
        const WifiScanLeaseCoordinator::RecoveryProof& proof) {
    ProbeManagerLock();
    if (fail_completion_once_) {
        fail_completion_once_ = false;
        return false;
    }
    return WifiScanRecoveryGate::Complete(
        coordinator_, claim, proof, scan_lease_, recovery_lease_,
        scan_session_id_, true, scans_enabled_, restore_state_);
}
void WifiStation::RetryScanAfterRecovery() { ++retry_calls_; }
bool WifiStation::RestoreRadioAfterExternalScanRecovery() {
    ++external_restore_calls_;
    wifi_config_t config = {};
    return restorer_.RestoreStationRuntime(
        &config, WIFI_BAND_MODE_2G_ONLY, WIFI_PS_MIN_MODEM, 72);
}
void WifiStation::RetryAfterExternalScanRecovery(bool reconnect) {
    if (reconnect) {
        ++external_reconnect_calls_;
    }
    RetryScanAfterRecovery();
}
void WifiStation::Start() {
    ++starts_;
    ++scan_session_id_;
    scans_enabled_ = true;
    connected_ = true;
}
void WifiStation::StartForExactConnection() {
    ++exact_mode_starts_;
    Start();
}
void WifiStation::Stop() {
    ++stops_;
    ++scan_session_id_;
    scans_enabled_ = false;
    connected_ = false;
    if (publish_debt_on_stop_) {
        publish_debt_on_stop_ = false;
        PublishDebt(false);
    }
}
void WifiStation::FailCompletionOnce() { fail_completion_once_ = true; }
void WifiStation::FailRestoreAt(const std::string& stage) {
    recovery_driver.FailOnceAt(stage);
}
void WifiStation::PublishDebtOnStop() { publish_debt_on_stop_ = true; }
int WifiStation::RestoreCalls() const { return restore_calls_; }
int WifiStation::RetryCalls() const { return retry_calls_; }
void WifiStation::CallbackWins() {
    assert(scan_lease_.has_value());
    const auto lease = *scan_lease_;
    assert(coordinator_.ObserveScanDone(lease).consume_now);
    assert(coordinator_.FinishCompletion(lease));
    scan_lease_.reset();
    recovery_lease_.reset();
}

WifiConfigurationAp::WifiConfigurationAp(WifiScanLeaseCoordinator& coordinator)
    : coordinator_(coordinator), restorer_(TestWifiRecoveryDriver()) {}
void WifiConfigurationAp::OnScanRecoveryNeeded(
        std::function<void(const WifiScanLeaseCoordinator::Lease&)> cb) {
    recovery_cb_ = std::move(cb);
}
WifiScanLeaseCoordinator::Lease WifiConfigurationAp::PublishDebt(bool enabled) {
    const auto acquired = coordinator_.TryAcquire(
        WifiScanLeaseCoordinator::Owner::kConfigAp);
    assert(acquired.acquired);
    assert(coordinator_.CommitSubmission(acquired.lease, false).drain_required);
    scan_lease_ = acquired.lease;
    recovery_lease_ = acquired.lease;
    scans_enabled_ = enabled;
    recovery_cb_(acquired.lease);
    return acquired.lease;
}
std::optional<WifiConfigurationAp::ScanRecoveryClaim>
WifiConfigurationAp::ClaimScanRecovery(
        const WifiScanLeaseCoordinator::Lease& lease) {
    ProbeManagerLock();
    return WifiScanRecoveryGate::TryClaim(
        coordinator_, lease, scan_lease_, recovery_lease_, scan_session_id_,
        scans_enabled_, restore_state_);
}
bool WifiConfigurationAp::HasScanRecoveryDebt(
        const WifiScanLeaseCoordinator::Lease& lease) const {
    return WifiScanRecoveryGate::HasDebt(lease, recovery_lease_);
}
bool WifiConfigurationAp::RestoreRadioAfterRecovery(
        const ScanRecoveryClaim& claim) {
    ProbeManagerLock();
    ++restore_calls_;
    wifi_config_t config = {};
    const bool restored = restorer_.RestoreConfigAp(
        config, WIFI_BAND_MODE_2G_ONLY, 72);
    return WifiScanRecoveryGate::MarkRestored(
        claim, scan_session_id_, restored, restore_state_);
}
bool WifiConfigurationAp::CompleteScanRecovery(
        const ScanRecoveryClaim& claim,
        const WifiScanLeaseCoordinator::RecoveryProof& proof) {
    ProbeManagerLock();
    if (fail_completion_once_) {
        fail_completion_once_ = false;
        return false;
    }
    return WifiScanRecoveryGate::Complete(
        coordinator_, claim, proof, scan_lease_, recovery_lease_,
        scan_session_id_, true, scans_enabled_, restore_state_);
}
void WifiConfigurationAp::RetryScanAfterRecovery() { ++retry_calls_; }
bool WifiConfigurationAp::RestoreRadioAfterExternalScanRecovery() {
    ++external_restore_calls_;
    wifi_config_t config = {};
    return restorer_.RestoreConfigAp(
        config, WIFI_BAND_MODE_2G_ONLY, 72);
}
void WifiConfigurationAp::RetryAfterExternalScanRecovery() {
    RetryScanAfterRecovery();
}
void WifiConfigurationAp::Start() {
    ++starts_;
    ++scan_session_id_;
    scans_enabled_ = true;
}
bool WifiConfigurationAp::Stop() {
    ++stops_;
    ++scan_session_id_;
    scans_enabled_ = false;
    if (publish_debt_on_stop_) {
        publish_debt_on_stop_ = false;
        PublishDebt(false);
    }
    return true;
}
void WifiConfigurationAp::FailCompletionOnce() { fail_completion_once_ = true; }
void WifiConfigurationAp::FailRestoreAt(const std::string& stage) {
    recovery_driver.FailOnceAt(stage);
}
void WifiConfigurationAp::PublishDebtOnStop() { publish_debt_on_stop_ = true; }
int WifiConfigurationAp::RestoreCalls() const { return restore_calls_; }
int WifiConfigurationAp::RetryCalls() const { return retry_calls_; }
void WifiConfigurationAp::CallbackWins() {
    assert(scan_lease_.has_value());
    const auto lease = *scan_lease_;
    assert(coordinator_.ObserveScanDone(lease).consume_now);
    assert(coordinator_.FinishCompletion(lease));
    scan_lease_.reset();
    recovery_lease_.reset();
}

WifiScanLeaseCoordinator::RecoveryProof WifiScanRecoveryExecutor::Execute(
        const WifiScanLeaseCoordinator::RecoveryDecision& recovery) {
    if (fail_executor_once) {
        fail_executor_once = false;
        return WifiScanLeaseCoordinator::RecoveryProof{};
    }
    return WifiScanLeaseCoordinator::RecoveryProof{
        recovery.recovery_id(), recovery.coordinator_identity_, true, true};
}

int main() {
    TaskCreationFailureRetriesOnlyFailedStage();
    OneShotStationDebtSurvivesWorkerNotificationFailure();
    ScanRetryPollerSurvivesNotificationFailure();
    DuplicateDebtCoalescesAndProductionRestoreRetries();
    ExecutorFailureRetainsClaimAndRetriesWithoutReclaiming();
    ProofCompletionFailureKeepsScansGatedAndRestoresAgain();
    CallbackBeforeClaimCancelsRecoveryWithoutRestore();
    RegisteredBlufiOwnerUsesSharedRecoveryExecutor();
    BlockingUiRecoveryRestoresManagerStationRoleAndRetriesAfterProof();
    BlockingUiRecoveryRestoresConfigApAndIdleRolesExactly();
    RecoveryDebtSurvivesTemporarilyUnavailableTask();
    LifecycleTransitionCannotInvalidateCapturedExternalRole();
    IdleBlockingUiPreparationStartsAndRestoresStoppedRadio();
    IdleBlockingUiPreparationRollsBackModeWhenStartFails();
    IdlePreparationRollbackFailureRetainsExactLeaseUntilRecovery();
    IdleFinishRestartFailureRetainsExactLeaseUntilRecovery();
    IdlePostResetRestoreFailuresKeepLifecycleAndLeaseBlocked();
    IdleBlockingUiPreparationLeavesRunningStaUndisrupted();
    IdleBlockingUiPreparationRestoresRunningApMode();
    IdlePreparationReservationBlocksLifecycleTransitions();
    IdlePreparationRejectsIncompleteInterfaceSnapshot();
    NewUnclaimedDebtRetargetsButClaimedDebtRejectsReplacement();
    RecoveryHooksRunWithoutManagerMutex();
    BothPendingTransitionsResumeOnlyAfterRecovery();
    StalePendingGenerationDoesNotStartTarget();
    LifecycleReservationStartsOnlyFreshGenerationAndReleasesExactly();
    LifecycleReservationRejectsAnotherCallersGeneration();
    ExactCredentialsRestartActiveStationUnderLifecycleLease();
    InvalidExactCredentialsAreRejectedBeforeStationStart();
    ExactConnectionFailureStopsPartiallyStartedStationExactlyOnce();
}
