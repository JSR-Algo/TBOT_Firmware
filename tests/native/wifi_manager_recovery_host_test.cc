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
esp_err_t esp_wifi_stop() { return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t) { return ESP_OK; }
esp_err_t esp_wifi_set_config(wifi_interface_t, wifi_config_t*) { return ESP_OK; }
esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { return ESP_OK; }
esp_err_t esp_wifi_start() { return ESP_OK; }
esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t) { return ESP_OK; }
esp_err_t esp_wifi_set_max_tx_power(int8_t) { return ESP_OK; }
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
void WifiStation::Start() {
    ++starts_;
    ++scan_session_id_;
    scans_enabled_ = true;
}
void WifiStation::Stop() {
    ++stops_;
    ++scan_session_id_;
    scans_enabled_ = false;
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
    NewUnclaimedDebtRetargetsButClaimedDebtRejectsReplacement();
    RecoveryHooksRunWithoutManagerMutex();
    BothPendingTransitionsResumeOnlyAfterRecovery();
    StalePendingGenerationDoesNotStartTarget();
}
