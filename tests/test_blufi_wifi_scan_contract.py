import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    candidate = ROOT / path
    if not candidate.exists() and path.startswith("managed_components/78__esp-wifi-connect/"):
        candidate = ROOT / path.replace(
            "managed_components/78__esp-wifi-connect",
            "components/esp-wifi-connect",
            1,
        )
    return candidate.read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


def test_default_event_loop_barrier_has_one_bounded_public_api():
    header = read(
        "components/esp-wifi-connect/include/default_event_loop_barrier.h"
    )

    assert "bool DrainDefaultEventLoop(std::chrono::milliseconds timeout);" in header
    assert "DefaultEventLoopScanDrainExecutor" not in header
    assert "WifiScanLeaseCoordinator" not in header
    assert "std::function" not in header
    assert "template <" not in header
    assert "bool barrier_drained" not in header


def test_default_event_loop_barrier_registers_one_process_lifetime_handler():
    source = read("components/esp-wifi-connect/default_event_loop_barrier.cc")
    body = function_body(source, "bool DrainDefaultEventLoop")

    state = source[
        source.index("class DefaultEventLoopBarrier") :
        source.index("bool DrainDefaultEventLoop")
    ]
    assert "xSemaphoreCreateBinary" in state
    assert "esp_event_handler_instance_register" in state
    assert "esp_event_handler_instance_unregister" not in source
    assert "static DefaultEventLoopBarrier* barrier = nullptr" in source
    assert "static std::mutex initialization_mutex" in source
    assert "std::lock_guard<std::mutex> initialization_lock" in source
    assert "if (barrier == nullptr)" in source
    assert "barrier = new (std::nothrow) DefaultEventLoopBarrier" in source
    assert "xSemaphoreCreateBinary" not in body
    assert "esp_event_handler_instance_register" not in body
    assert "vSemaphoreDelete" not in body
    assert state.count("vSemaphoreDelete") == 1
    register_failure = state.index("register_result != ESP_OK")
    assert register_failure < state.index("vSemaphoreDelete", register_failure)
    assert "ESP_EVENT_DEFINE_BASE" in source
    assert "ESP_EVENT_ANY_BASE" not in source
    assert "ESP_EVENT_ANY_ID" not in source
    assert "kMaximumBarrierWait{1000}" in source
    assert "std::min(timeout, kMaximumBarrierWait)" in source
    assert "portMAX_DELAY" not in source


def test_default_event_loop_barrier_serializes_and_rejects_late_generations():
    source = read("components/esp-wifi-connect/default_event_loop_barrier.cc")
    state = source[source.index("class DefaultEventLoopBarrier") :]

    assert "std::lock_guard<std::mutex> lock(mutex_)" in state
    assert state.count("std::lock_guard<std::mutex> handler_lock(handler_mutex_)") >= 2
    assert "std::atomic<uint64_t> active_barrier_id_" in state
    assert "posted_barrier_id == self->active_barrier_id_.load" in state
    assert "active_barrier_id_.store(0" in state
    assert "esp_event_post" in state
    assert "xSemaphoreTake" in state


def test_default_event_loop_barrier_has_no_scan_executor_or_driver_actions():
    header = read(
        "components/esp-wifi-connect/include/default_event_loop_barrier.h"
    )
    source = read("components/esp-wifi-connect/default_event_loop_barrier.cc")

    combined = header + source
    assert "DefaultEventLoopScanDrainExecutor" not in combined
    assert "WifiScanLeaseCoordinator" not in combined
    assert "DrainProof" not in combined
    assert "esp_wifi_" not in combined


def test_default_event_loop_barrier_is_built_by_wifi_component():
    cmake = read("components/esp-wifi-connect/CMakeLists.txt")

    assert cmake.count('"default_event_loop_barrier.cc"') == 2


def test_default_event_loop_barrier_host_cleanup_model():
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_default_event_loop_barrier_test.sh")],
        cwd=ROOT,
        check=True,
    )


def test_shared_scan_recovery_executor_is_the_only_proof_factory():
    header = read(
        "components/esp-wifi-connect/include/wifi_scan_recovery_executor.h"
    )
    source = read("components/esp-wifi-connect/wifi_scan_recovery_executor.cc")
    coordinator = read(
        "components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"
    )
    cmake = read("components/esp-wifi-connect/CMakeLists.txt")

    assert "class WifiScanRecoveryExecutor" in header
    assert "WifiScanLeaseCoordinator::RecoveryProof Execute(" in header
    assert "WifiScanRecoveryExecutor(const WifiScanRecoveryExecutor&) = delete" in header
    assert "static std::mutex& ProcessMutex();" in header
    assert "std::mutex mutex_;" not in header
    assert "friend class WifiScanRecoveryExecutor;" in coordinator
    assert coordinator.count("friend class WifiScanRecoveryExecutor;") == 2
    assert "const WifiScanLeaseCoordinator* coordinator_identity_" in coordinator
    assert source.count(
        "recovery.recovery_id(), recovery.coordinator_identity_, true, true"
    ) == 1
    assert '"wifi_scan_recovery_executor.cc"' in cmake


def test_shared_scan_recovery_executor_has_exact_fail_closed_choreography():
    source = read("components/esp-wifi-connect/wifi_scan_recovery_executor.cc")
    execute = function_body(
        source, "WifiScanRecoveryExecutor::Execute"
    )

    operations = (
        "esp_wifi_scan_stop()",
        "esp_wifi_stop()",
        "esp_wifi_deinit()",
        "DrainDefaultEventLoop(std::chrono::milliseconds(1000))",
        "esp_wifi_init(&cfg)",
    )
    positions = [execute.index(operation) for operation in operations]
    assert positions == sorted(positions)
    assert "cfg.nvs_enable = false;" in execute
    assert "ESP_ERR_WIFI_NOT_INIT" in source
    assert "ESP_ERR_WIFI_NOT_STARTED" in source
    assert "ESP_ERR_WIFI_STATE" in source
    assert "ProcessMutex()" in source
    assert "esp_netif_init" not in source
    assert "esp_event_loop_create_default" not in source
    assert "nvs_flash" not in source


def test_wifi_manager_exposes_process_lifetime_scan_coordinator_without_locking():
    header = read("components/esp-wifi-connect/include/wifi_manager.h")
    source = read("components/esp-wifi-connect/wifi_manager.cc")

    assert '#include "wifi_scan_lease_coordinator.h"' in header
    assert "WifiScanLeaseCoordinator& ScanLeaseCoordinator()" in header
    accessor = function_body(header, "WifiScanLeaseCoordinator& ScanLeaseCoordinator()")
    assert "return scan_lease_coordinator_;" in accessor
    assert "mutex_" not in accessor
    assert "WifiScanLeaseCoordinator scan_lease_coordinator_;" in header
    assert "static WifiManager* instance = new WifiManager" in source
    assert "std::make_unique<WifiStation>(scan_lease_coordinator_)" in source
    assert "std::make_unique<WifiConfigurationAp>(scan_lease_coordinator_)" in source


def test_station_scan_lease_uses_exact_callback_or_recovery_ownership():
    header = read("components/esp-wifi-connect/include/wifi_station.h")
    source = read("components/esp-wifi-connect/wifi_station.cc")
    start = function_body(source, "bool WifiStation::StartOwnedScan")
    handler = function_body(source, "void WifiStation::WifiEventHandler")
    stop = function_body(source, "void WifiStation::Stop")
    completion = function_body(source, "void WifiStation::CompleteOwnedScan")

    assert "std::mutex scan_mutex_;" in header
    assert "std::optional<WifiScanLeaseCoordinator::Lease> scan_lease_;" in header
    assert "scan_in_progress_" not in header + source
    assert "TryAcquire(WifiScanLeaseCoordinator::Owner::kStation)" in start
    assert start.index("TryAcquire(") < start.index("esp_wifi_scan_start(")
    assert start.index("esp_wifi_scan_start(") < start.index("CommitSubmission(")
    assert "commit.consume_latched" in start
    assert "CompleteOwnedScan(lease)" in start
    assert handler.index("ObserveScanDone(lease)") < handler.index(
        "CompleteOwnedScan(lease)"
    )
    assert completion.index("esp_wifi_scan_get_ap_num") < completion.index(
        "FinishCompletion(lease)"
    )
    assert completion.index("esp_wifi_clear_ap_list") < completion.index(
        "FinishCompletion(lease)"
    )
    assert stop.index("esp_timer_stop") < stop.index("BeginDrain(lease)")
    assert stop.index("BeginDrain(lease)") < stop.index("esp_wifi_scan_stop")
    assert "esp_event_handler_instance_unregister(WIFI_EVENT" not in stop
    assert "scan_lease_.reset()" not in stop


def test_config_ap_scan_lease_uses_one_owned_start_and_retained_handler():
    header = read("components/esp-wifi-connect/include/wifi_configuration_ap.h")
    source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    start_mode = function_body(source, "void WifiConfigurationAp::Start")
    start_scan = function_body(source, "bool WifiConfigurationAp::StartOwnedScan")
    handler = function_body(source, "void WifiConfigurationAp::WifiEventHandler")
    stop = function_body(source, "bool WifiConfigurationAp::Stop")
    completion = function_body(source, "void WifiConfigurationAp::CompleteOwnedScan")

    assert "std::mutex scan_mutex_;" in header
    assert "std::optional<WifiScanLeaseCoordinator::Lease> scan_lease_;" in header
    assert source.count("esp_wifi_scan_start(") == 1
    assert "StartOwnedScan();" in start_mode
    assert "StartOwnedScan();" in source[source.index(".callback =") : source.index(".arg = this")]
    assert "TryAcquire(WifiScanLeaseCoordinator::Owner::kConfigAp)" in start_scan
    assert start_scan.index("TryAcquire(") < start_scan.index("esp_wifi_scan_start(")
    assert start_scan.index("esp_wifi_scan_start(") < start_scan.index(
        "CommitSubmission("
    )
    assert "commit.consume_latched" in start_scan
    assert handler.index("ObserveScanDone(lease)") < handler.index(
        "CompleteOwnedScan(lease)"
    )
    assert completion.index("esp_wifi_scan_get_ap_num") < completion.index(
        "FinishCompletion(lease)"
    )
    assert completion.index("esp_wifi_clear_ap_list") < completion.index(
        "FinishCompletion(lease)"
    )
    assert stop.index("esp_timer_stop") < stop.index("BeginDrain(lease)")
    assert stop.index("BeginDrain(lease)") < stop.index("esp_wifi_scan_stop")
    assert "esp_event_handler_instance_unregister(WIFI_EVENT" not in stop
    assert "scan_lease_.reset()" not in stop


def test_station_and_config_stop_never_cancel_a_foreign_scan():
    station = read("components/esp-wifi-connect/wifi_station.cc")
    config = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    station_stop = function_body(station, "void WifiStation::Stop")
    config_stop = function_body(config, "bool WifiConfigurationAp::Stop")
    config_connect = function_body(config, "bool WifiConfigurationAp::ConnectToWifi")

    for body in (station_stop, config_stop, config_connect):
        owned = body.index("if (scan_lease_.has_value())")
        scan_stop = body.index("esp_wifi_scan_stop()")
        assert owned < scan_stop
        guarded = body[owned:scan_stop]
        assert "const auto lease = *scan_lease_" in guarded
        assert "BeginDrain(lease)" in guarded


def test_restarted_station_and_config_retry_while_old_callback_debt_drains():
    station = function_body(
        read("components/esp-wifi-connect/wifi_station.cc"),
        "bool WifiStation::StartOwnedScan",
    )
    config = function_body(
        read("components/esp-wifi-connect/wifi_configuration_ap.cc"),
        "bool WifiConfigurationAp::StartOwnedScan",
    )

    for body in (station, config):
        assert "local_callback_debt = scan_lease_.has_value()" in body
        assert body.index("local_callback_debt = scan_lease_.has_value()") < body.index(
            "esp_timer_start_once("
        )


def test_station_submission_stop_and_completion_share_one_lifecycle_lock():
    source = read("components/esp-wifi-connect/wifi_station.cc")
    start = function_body(source, "bool WifiStation::StartOwnedScan")
    stop = function_body(source, "void WifiStation::Stop")
    completion = function_body(source, "void WifiStation::CompleteOwnedScan")

    assert "std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_)" in start
    assert start.index("lifecycle_lock") < start.index("TryAcquire(")
    assert start.index("TryAcquire(") < start.index("esp_wifi_scan_start(")
    assert start.index("esp_wifi_scan_start(") < start.index("CommitSubmission(")
    assert start.index("CommitSubmission(") < start.index("lifecycle_lock.unlock()")

    assert "std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_)" in stop
    assert stop.index("lifecycle_lock") < stop.index("scans_enabled_ = false")
    assert stop.index("scans_enabled_ = false") < stop.index("BeginDrain(")
    assert stop.index("BeginDrain(") < stop.index("esp_wifi_scan_stop(")
    assert stop.index("esp_wifi_scan_stop(") < stop.index("esp_wifi_stop(")
    assert stop.index("esp_wifi_stop(") < stop.index("lifecycle_lock.unlock()")

    assert "std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_)" in completion
    assert completion.index("lifecycle_lock") < completion.index("esp_wifi_scan_get_ap_num")
    assert "HandleScanResultLocked(" in completion
    assert completion.index("lifecycle_lock.unlock()") < completion.index(
        "HandleScanResultLocked("
    )


def test_config_submission_connect_stop_and_publish_share_lifecycle_lock():
    source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    start = function_body(source, "bool WifiConfigurationAp::StartOwnedScan")
    connect = function_body(source, "bool WifiConfigurationAp::ConnectToWifi")
    stop = function_body(source, "bool WifiConfigurationAp::Stop")
    completion = function_body(source, "void WifiConfigurationAp::CompleteOwnedScan")

    assert "std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_)" in start
    assert start.index("lifecycle_lock") < start.index("TryAcquire(")
    assert start.index("TryAcquire(") < start.index("esp_wifi_scan_start(")
    assert start.index("CommitSubmission(") < start.index("lifecycle_lock.unlock()")

    assert "std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_)" in connect
    assert connect.index("lifecycle_lock") < connect.index("BeginDrain(")
    assert connect.index("BeginDrain(") < connect.index("esp_wifi_scan_stop(")
    assert connect.index("esp_wifi_scan_stop(") < connect.index(
        "lifecycle_lock.unlock()"
    )

    assert "std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_)" in stop
    assert stop.index("lifecycle_lock") < stop.index("scans_enabled_ = false")
    assert stop.index("BeginDrain(") < stop.index("esp_wifi_scan_stop(")
    assert stop.index("lifecycle_lock.unlock()") < stop.index(
        "FinishConnectionAttemptBoundary(0, 0, true)"
    )
    assert "ap_records_.clear()" in stop

    assert "std::unique_lock<std::mutex> lifecycle_lock(scan_mutex_)" in completion
    assert completion.index("lifecycle_lock") < completion.index("esp_wifi_scan_get_ap_num")
    assert completion.index("ap_records_.swap(scanned_records)") < completion.index(
        "lifecycle_lock.unlock()"
    )


def test_station_uses_session_operation_permits_for_all_emitting_handlers():
    header = read("components/esp-wifi-connect/include/wifi_station.h")
    source = read("components/esp-wifi-connect/wifi_station.cc")
    completion = function_body(source, "void WifiStation::CompleteOwnedScan")
    wifi_handler = function_body(source, "void WifiStation::WifiEventHandler")
    ip_handler = function_body(source, "void WifiStation::IpEventHandler")
    stop = function_body(source, "void WifiStation::Stop")

    assert "std::condition_variable session_operations_drained_;" in header
    assert "size_t in_flight_session_operations_ = 0;" in header
    assert "bool TryBeginSessionOperationLocked(uint64_t session_id);" in header
    assert "void FinishSessionOperation();" in header

    assert "expected_session = lease_session_id_" in completion
    assert "TryBeginSessionOperationLocked(expected_session)" in completion
    assert completion.index("TryBeginSessionOperationLocked") < completion.index(
        "lifecycle_lock.unlock()"
    )
    assert completion.index("lifecycle_lock.unlock()") < completion.index(
        "StartConnectForSession("
    )
    assert completion.index("StartConnectForSession(") < completion.index(
        "FinishSessionOperation()"
    )

    disconnected = wifi_handler[
        wifi_handler.index("WIFI_EVENT_STA_DISCONNECTED") :
    ]
    assert "TryBeginSessionOperationLocked(session_id)" in disconnected
    assert disconnected.index("TryBeginSessionOperationLocked") < disconnected.index(
        "esp_wifi_connect()"
    )
    assert disconnected.index("FinishSessionOperation()") < disconnected.index(
        "on_disconnected_("
    )

    assert "TryBeginSessionOperationLocked(session_id)" in ip_handler
    assert ip_handler.index("TryBeginSessionOperationLocked") < ip_handler.index(
        "FinishSessionOperation()"
    )
    assert ip_handler.index("FinishSessionOperation()") < ip_handler.index(
        "on_connected_("
    )
    disable = stop.index("scans_enabled_ = false")
    invalidate = stop.index("++scan_session_id_")
    wait = stop.index("session_operations_drained_.wait(")
    clear = stop.index("connect_queue_.clear()")
    assert disable < invalidate < wait < clear


def test_wifi_manager_never_waits_for_station_stop_under_manager_mutex():
    source = read("components/esp-wifi-connect/wifi_manager.cc")
    header = read("components/esp-wifi-connect/include/wifi_manager.h")

    assert "uint64_t lifecycle_generation_ = 0;" in header
    assert "bool lifecycle_transition_in_progress_ = false;" in header
    for signature in (
        "void WifiManager::StopStation",
        "void WifiManager::StartConfigAp",
        "bool WifiManager::StopRadio",
    ):
        body = function_body(source, signature)
        stop = body.index("station->Stop()") if "station->Stop()" in body else body.index(
            "station_to_stop->Stop()"
        )
        assert body.rfind("lock.unlock()", 0, stop) != -1
        assert "transition_generation" in body


def test_station_external_callbacks_use_reentrant_session_gate_after_permit_finish():
    header = read("components/esp-wifi-connect/include/wifi_station.h")
    source = read("components/esp-wifi-connect/wifi_station.cc")
    stop = function_body(source, "void WifiStation::Stop")
    completion = function_body(source, "void WifiStation::CompleteOwnedScan")
    wifi_handler = function_body(source, "void WifiStation::WifiEventHandler")
    ip_handler = function_body(source, "void WifiStation::IpEventHandler")
    dispatch = function_body(source, "void WifiStation::DispatchSessionCallback")

    assert "std::recursive_mutex session_callback_mutex_;" in header
    assert "void DispatchSessionCallback(" in header
    assert stop.index("session_callback_mutex_") < stop.index("scan_mutex_")
    assert dispatch.index("session_callback_mutex_") < dispatch.index("scan_mutex_")
    assert "scans_enabled_ && expected_session == scan_session_id_" in dispatch

    assert completion.index("FinishSessionOperation()") < completion.index(
        "DispatchSessionCallback("
    )
    sta_start = wifi_handler[
        wifi_handler.index("WIFI_EVENT_STA_START") :
        wifi_handler.index("WIFI_EVENT_SCAN_DONE")
    ]
    assert sta_start.index("FinishSessionOperation()") < sta_start.index(
        "DispatchSessionCallback("
    )
    disconnected = wifi_handler[wifi_handler.index("WIFI_EVENT_STA_DISCONNECTED") :]
    assert disconnected.index("FinishSessionOperation()") < disconnected.index(
        "DispatchSessionCallback("
    )
    assert ip_handler.index("FinishSessionOperation()") < ip_handler.index(
        "DispatchSessionCallback("
    )

    for callback in ("on_scan_begin_", "on_connect_", "on_connected_", "on_disconnected_"):
        assert f"DispatchSessionCallback" in source
        uses = [m.start() for m in re.finditer(re.escape(callback + "("), source)]
        assert uses
        for use in uses:
            prefix = source[max(0, use - 500) : use]
            assert "DispatchSessionCallback(" in prefix


def test_station_retry_is_post_permit_and_exact_session_scoped():
    header = read("components/esp-wifi-connect/include/wifi_station.h")
    source = read("components/esp-wifi-connect/wifi_station.cc")
    completion = function_body(source, "void WifiStation::CompleteOwnedScan")
    handler = function_body(source, "void WifiStation::WifiEventHandler")
    retry = function_body(source, "void WifiStation::ScheduleScanRetry")

    assert "void ScheduleScanRetry(uint64_t expected_session" in header
    assert "expected_session == scan_session_id_" in retry
    assert completion.index("FinishSessionOperation()") < completion.index(
        "ScheduleScanRetry("
    )
    disconnected = handler[handler.index("WIFI_EVENT_STA_DISCONNECTED") :]
    assert disconnected.index("FinishSessionOperation()") < disconnected.index(
        "ScheduleScanRetry("
    )

    scan_result = function_body(source, "WifiStation::HandleScanResultLocked")
    assert "ScheduleScanRetry(" not in scan_result
    assert "scan_mutex_" not in scan_result


def test_config_connect_waiter_is_exact_attempt_scoped_and_cancelled_by_stop():
    header = read("components/esp-wifi-connect/include/wifi_configuration_ap.h")
    source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    connect = function_body(source, "bool WifiConfigurationAp::ConnectToWifi")
    stop = function_body(source, "bool WifiConfigurationAp::Stop")
    wifi_handler = function_body(source, "void WifiConfigurationAp::WifiEventHandler")
    ip_handler = function_body(source, "void WifiConfigurationAp::IpEventHandler")

    assert "WIFI_CANCEL_BIT" in source
    assert "uint64_t connection_attempt_id_ = 0;" in header
    assert "uint64_t active_connection_attempt_id_ = 0;" in header
    assert "uint64_t active_connection_session_id_ = 0;" in header
    assert "std::condition_variable connection_waiter_drained_;" in header
    assert "WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_CANCEL_BIT" in connect
    assert "attempt_id == active_connection_attempt_id_" in connect
    assert "attempt_session == active_connection_session_id_" in connect
    assert "ScheduleScanRetry(attempt_session" in connect
    assert "xEventGroupSetBits(event_group_, WIFI_CANCEL_BIT)" in stop
    assert "connection_waiter_drained_.wait(" in stop
    assert "FinishConnectionAttemptBoundary(" in connect
    assert "if (!connection_boundary_ready_)" in connect
    boundary = function_body(
        source, "bool WifiConfigurationAp::FinishConnectionAttemptBoundary"
    )
    drains = [m.start() for m in re.finditer("DrainDefaultEventLoop\\(", boundary)]
    assert len(drains) == 2
    clear = boundary.index("xEventGroupClearBits(")
    disconnect = boundary.index("esp_wifi_disconnect()")
    wait = boundary.index("xEventGroupWaitBits(")
    assert drains[0] < clear < disconnect < wait < drains[1]
    assert "WIFI_ATTEMPT_BOUNDARY_BIT" in boundary
    assert "active_connection_attempt_id_ = 0" in boundary
    assert "attempt_id != active_connection_attempt_id_" in boundary
    assert "attempt_session != active_connection_session_id_" in boundary
    assert boundary.index("active_connection_attempt_id_ = 0") < boundary.index(
        "esp_wifi_disconnect()"
    )
    assert "connection_boundary_ready_ = predrained" in boundary
    for handler in (wifi_handler, ip_handler):
        assert "active_connection_attempt_id_ != 0" in handler
        assert "active_connection_session_id_ == self->scan_session_id_" in handler


def test_config_stop_runs_closed_two_sided_connection_boundary():
    source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    stop = function_body(source, "bool WifiConfigurationAp::Stop")
    boundary = function_body(
        source, "bool WifiConfigurationAp::FinishConnectionAttemptBoundary"
    )

    assert "FinishConnectionAttemptBoundary(" in stop
    assert "FinishConnectionAttemptBoundary(0, 0, true)" in stop
    assert "if (!boundary_closed)" in stop
    assert "return false" in stop
    assert "connection_boundary_ready_ = true" not in stop
    assert "if (stop_wifi)" in boundary
    assert boundary.index("esp_wifi_disconnect()") < boundary.index(
        "esp_wifi_stop()"
    )
    post_drain = boundary.rindex("DrainDefaultEventLoop(")
    assert boundary.index("esp_wifi_stop()") < post_drain
    assert post_drain < boundary.index(
        "connection_boundary_ready_ = predrained"
    )
    assert "WIFI_EVENT_STA_STOP" in function_body(
        source, "void WifiConfigurationAp::WifiEventHandler"
    )
    waiter_drain = stop.index("connection_waiter_drained_.wait(")
    clear_cancel = stop.index("xEventGroupClearBits(event_group_, WIFI_CANCEL_BIT)")
    boundary_call = stop.index("FinishConnectionAttemptBoundary(0, 0, true)")
    assert waiter_drain < clear_cancel < boundary_call

    stop_wait = boundary[boundary.index("bool stop_terminal") :]
    assert "WIFI_ATTEMPT_BOUNDARY_BIT | WIFI_CANCEL_BIT" not in stop_wait
    assert "event_group_, WIFI_ATTEMPT_BOUNDARY_BIT," in stop_wait

    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    start_station = function_body(manager, "void WifiManager::StartStation")
    assert "if (!config_ap_to_stop->Stop())" in start_station
    assert start_station.index("if (!config_ap_to_stop->Stop())") < start_station.index(
        "StartStationTarget("
    )
    stop_config = function_body(manager, "void WifiManager::StopConfigAp")
    assert "if (!config_ap->Stop())" in stop_config
    assert "wifi_teardown_faulted_ = true" in stop_config
    assert "config_mode_active_ = true" not in stop_config


def test_failed_config_boundary_retains_resources_and_blocks_manager_transitions():
    header = read("components/esp-wifi-connect/include/wifi_configuration_ap.h")
    source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    manager_header = read("components/esp-wifi-connect/include/wifi_manager.h")
    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    stop = function_body(source, "bool WifiConfigurationAp::Stop")

    assert "bool teardown_faulted_ = false;" in header
    assert "std::atomic<bool> stopped_{false};" in header
    assert "if (stopped_.load())" in stop
    assert "if (!boundary_closed)" in stop
    failure = stop[stop.index("if (!boundary_closed)") :]
    for teardown in (
        "esp_timer_delete(",
        "esp_netif_destroy_default_wifi(",
        "httpd_stop(",
        "dns_server_->Stop()",
    ):
        assert failure.index("return false") < failure.index(teardown)
    assert "teardown_faulted_ = true" in failure[: failure.index("return false")]
    assert "bool wifi_teardown_faulted_ = false;" in manager_header

    for signature in (
        "void WifiManager::StartStation",
        "void WifiManager::StartConfigAp",
        "bool WifiManager::StopRadio",
    ):
        body = function_body(manager, signature)
        assert "wifi_teardown_faulted_" in body

    destructor = function_body(source, "WifiConfigurationAp::~WifiConfigurationAp")
    assert "Stop()" not in destructor
    assert "instance_any_id_ != nullptr" in destructor
    assert "std::abort()" in destructor

    assert "~WifiManager() = delete;" in manager_header
    assert "WifiManager::~WifiManager" not in manager
    assert "station_.release()" not in manager
    assert "config_ap_.release()" not in manager


def test_process_lifetime_manager_graph_and_no_handler_scanner_destruction():
    header = read("components/esp-wifi-connect/include/wifi_configuration_ap.h")
    source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    destructor = function_body(source, "WifiConfigurationAp::~WifiConfigurationAp")
    start = function_body(source, "void WifiConfigurationAp::Start")

    assert "std::atomic<bool> started_{false};" in header
    assert "started_.store(true)" in start
    assert "instance_any_id_ != nullptr" in destructor
    assert "instance_got_ip_ != nullptr" in destructor
    assert "std::abort()" in destructor
    assert "Stop()" not in destructor

    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    manager_header = read("components/esp-wifi-connect/include/wifi_manager.h")
    station = read("components/esp-wifi-connect/wifi_station.cc")
    station_destructor = function_body(station, "WifiStation::~WifiStation")
    assert "static WifiManager* instance = new WifiManager" in manager
    assert "~WifiManager() = delete;" in manager_header
    assert "WifiManager::~WifiManager" not in manager
    assert "instance_any_id_ != nullptr" in station_destructor
    assert "instance_got_ip_ != nullptr" in station_destructor
    assert "std::abort()" in station_destructor
    assert "Stop()" not in station_destructor


def test_config_cancelled_scan_completion_cannot_publish_during_credential_test():
    source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    completion = function_body(source, "void WifiConfigurationAp::CompleteOwnedScan")

    consume_guard = completion[
        completion.index("const bool consume_results") : completion.index("uint16_t ap_num")
    ]
    publish_guard = completion[
        completion.index("if (consume_results && scans_enabled_") : completion.index(
            "ap_records_.swap(scanned_records)"
        )
    ]
    assert "!is_connecting_" in consume_guard
    assert "!is_connecting_" in publish_guard


def test_station_and_config_retain_completion_when_driver_ap_cleanup_is_unproven():
    station_header = read("components/esp-wifi-connect/include/wifi_station.h")
    station_source = read("components/esp-wifi-connect/wifi_station.cc")
    config_header = read("components/esp-wifi-connect/include/wifi_configuration_ap.h")
    config_source = read("components/esp-wifi-connect/wifi_configuration_ap.cc")

    assert "ClaimScanRecovery(" in station_header
    assert "CompleteScanRecovery(" in station_header
    assert "std::optional<WifiScanLeaseCoordinator::Lease> scan_recovery_lease_;" in station_header
    assert "ClaimScanRecovery(" in config_header
    assert "CompleteScanRecovery(" in config_header
    assert "std::optional<WifiScanLeaseCoordinator::Lease> scan_recovery_lease_;" in config_header
    assert "scan_recovery_needed_" in station_header + station_source
    assert "scan_recovery_needed_" in config_header + config_source

    for source, signature in (
        (station_source, "void WifiStation::CompleteOwnedScan"),
        (config_source, "void WifiConfigurationAp::CompleteOwnedScan"),
    ):
        completion = function_body(source, signature)
        assert "esp_wifi_scan_get_ap_num" in completion
        assert "esp_wifi_scan_get_ap_records" in completion
        assert "esp_wifi_clear_ap_list" in completion
        assert "cleanup_proven" in completion
        assert completion.index("if (!cleanup_proven)") < completion.index(
            "FinishCompletion(lease)"
        )
        failure = completion[
            completion.index("if (!cleanup_proven)") : completion.index(
                "FinishCompletion(lease)"
            )
        ]
        assert "RetainFailedCompletion(lease)" in failure
        assert "RetainRecoveryDebtLocked(lease)" in failure
        assert "scan_lease_.reset()" not in failure


def test_station_and_config_publish_exact_recovery_debt_for_start_and_cancel():
    for header_path, source_path, owner in (
        (
            "components/esp-wifi-connect/include/wifi_station.h",
            "components/esp-wifi-connect/wifi_station.cc",
            "WifiStation",
        ),
        (
            "components/esp-wifi-connect/include/wifi_configuration_ap.h",
            "components/esp-wifi-connect/wifi_configuration_ap.cc",
            "WifiConfigurationAp",
        ),
    ):
        header = read(header_path)
        source = read(source_path)
        start = function_body(source, f"bool {owner}::StartOwnedScan")
        stop_signature = (
            f"bool {owner}::Stop"
            if owner == "WifiConfigurationAp"
            else f"void {owner}::Stop"
        )
        stop = function_body(source, stop_signature)
        claim = function_body(source, f"{owner}::ClaimScanRecovery")
        complete = function_body(source, f"bool {owner}::CompleteScanRecovery")

        assert "commit.drain_required" in start
        assert "RetainRecoveryDebtLocked(lease)" in start
        assert "RetainRecoveryDebtLocked(lease)" in stop
        assert "scan_recovery_lease_" in claim
        assert "BeginRecovery(" in claim
        assert "CompleteRecovery(" in complete
        assert "scan_lease_.reset()" in complete
        assert "scan_recovery_lease_.reset()" in complete
        assert "ScanRecoveryClaim" in header


def test_shared_scan_recovery_is_scheduled_by_process_lifetime_manager_worker():
    manager_header = read("components/esp-wifi-connect/include/wifi_manager.h")
    manager = read("components/esp-wifi-connect/wifi_manager.cc")

    assert '#include "wifi_scan_recovery_executor.h"' in manager_header
    assert "WifiScanRecoveryExecutor scan_recovery_executor_;" in manager_header
    assert "TaskHandle_t scan_recovery_task_" in manager_header
    assert "std::optional<ScanRecoveryWork> scan_recovery_claim_;" in manager_header
    assert "void WifiManager::ScheduleScanRecovery" in manager
    assert "void WifiManager::ScanRecoveryTask" in manager
    assert "void WifiManager::RunScanRecovery" in manager
    assert "xTaskCreate" in function_body(manager, "bool WifiManager::Initialize")
    assert "vTaskDelete" not in manager
    schedule = function_body(manager, "void WifiManager::ScheduleScanRecovery")
    assert "xTaskNotifyGive" in schedule
    assert "scan_recovery_executor_.Execute" not in schedule


def test_scanners_notify_exact_debt_only_after_releasing_scan_lock():
    for header_path, source_path, owner in (
        (
            "components/esp-wifi-connect/include/wifi_station.h",
            "components/esp-wifi-connect/wifi_station.cc",
            "WifiStation",
        ),
        (
            "components/esp-wifi-connect/include/wifi_configuration_ap.h",
            "components/esp-wifi-connect/wifi_configuration_ap.cc",
            "WifiConfigurationAp",
        ),
    ):
        header = read(header_path)
        source = read(source_path)
        assert "OnScanRecoveryNeeded" in header
        assert "NotifyScanRecoveryNeeded" in header
        notify = function_body(source, f"void {owner}::NotifyScanRecoveryNeeded")
        assert "scan_recovery_needed_" in notify
        assert "scan_mutex_" not in notify

        start = function_body(source, f"bool {owner}::StartOwnedScan")
        assert start.index("lifecycle_lock.unlock()") < start.index(
            "NotifyScanRecoveryNeeded(lease)"
        )


def test_manager_recovery_retains_one_exact_claim_across_bounded_retries():
    manager_header = read("components/esp-wifi-connect/include/wifi_manager.h")
    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    run = function_body(manager, "void WifiManager::RunScanRecovery")

    assert "scan_recovery_claim_" in manager_header
    assert "scan_recovery_active_" in manager_header
    assert "scan_recovery_retry_pending_" in manager_header
    assert run.count("ClaimScanRecovery(") == 2
    assert "work = scan_recovery_claim_" in run
    assert "vTaskDelay" in run
    assert "CompleteScanRecovery" in run
    assert "RetryScanAfterRecovery" in run
    assert "wifi_teardown_faulted_ = false" not in run


def test_manager_lifecycle_entry_points_are_blocked_during_scan_recovery():
    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    for signature in (
        "void WifiManager::StartStation",
        "void WifiManager::StopStation",
        "void WifiManager::StartConfigAp",
        "void WifiManager::StopConfigAp",
        "bool WifiManager::StopRadio",
    ):
        body = function_body(manager, signature)
        assert "scan_recovery_active_" in body


def test_mode_transitions_defer_target_start_until_exact_scan_recovery_finishes():
    header = read("components/esp-wifi-connect/include/wifi_manager.h")
    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    run = function_body(manager, "void WifiManager::RunScanRecovery")

    assert "enum class PendingLifecycleTarget" in header
    assert "pending_lifecycle_target_" in header
    assert "pending_lifecycle_generation_" in header
    assert "ResumePendingLifecycleTransition" in header

    for signature, stopped_source, target_start in (
        (
            "void WifiManager::StartStation",
            "config_ap_to_stop->Stop()",
            "StartStationTarget(",
        ),
        (
            "void WifiManager::StartConfigAp",
            "station_to_stop->Stop()",
            "StartConfigApTarget(",
        ),
    ):
        body = function_body(manager, signature)
        source_stop = body.index(stopped_source)
        defer = body.index("DeferLifecycleTransitionForRecovery", source_stop)
        target = body.index(target_start)
        assert source_stop < defer < target
        assert "return" in body[defer:target]

    start_station = function_body(manager, "void WifiManager::StartStation")
    assert start_station.index("config_ap_to_stop->Stop()") < start_station.index(
        "config_mode_active_ = false"
    )
    start_config = function_body(manager, "void WifiManager::StartConfigAp")
    assert start_config.index("station_to_stop->Stop()") < start_config.index(
        "station_active_ = false"
    )

    assert run.index("CompleteScanRecovery") < run.index(
        "ResumePendingLifecycleTransition"
    )


def test_pending_transition_is_generation_bound_and_consumed_once():
    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    defer = function_body(
        manager, "bool WifiManager::DeferLifecycleTransitionForRecovery"
    )
    resume = function_body(
        manager, "void WifiManager::ResumePendingLifecycleTransition"
    )

    assert "lifecycle_generation_ != transition_generation" in defer
    assert "pending_lifecycle_generation_ = transition_generation" in defer
    assert "pending_lifecycle_target_ = target" in defer
    assert "pending_lifecycle_target_ = PendingLifecycleTarget::kNone" in resume
    assert "pending_lifecycle_generation_ = 0" in resume
    assert "lifecycle_generation_ != pending_generation" in resume
    assert resume.count("StartStationTarget(") == 1
    assert resume.count("StartConfigApTarget(") == 1


def test_manager_active_flags_stay_false_while_target_transition_is_pending():
    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    defer = function_body(
        manager, "bool WifiManager::DeferLifecycleTransitionForRecovery"
    )
    assert "station_active_ = false" in defer
    assert "config_mode_active_ = false" in defer
    assert "lifecycle_transition_in_progress_ = false" in defer
    for signature in (
        "void WifiManager::StartStation",
        "void WifiManager::StopStation",
        "void WifiManager::StartConfigAp",
        "void WifiManager::StopConfigAp",
        "bool WifiManager::StopRadio",
    ):
        assert "pending_lifecycle_target_" in function_body(manager, signature)


def test_recovery_restore_reapplies_start_time_wifi_runtime_settings():
    station = read("components/esp-wifi-connect/wifi_station.cc")
    config = read("components/esp-wifi-connect/wifi_configuration_ap.cc")
    station_restore = function_body(
        station, "bool WifiStation::RestoreRadioAfterRecovery"
    )
    config_restore = function_body(
        config, "bool WifiConfigurationAp::RestoreRadioAfterRecovery"
    )

    assert "esp_wifi_set_mode(WIFI_MODE_STA)" in station_restore
    assert "esp_wifi_start()" in station_restore
    assert "esp_wifi_set_max_tx_power(max_tx_power_)" in station_restore
    assert "esp_wifi_set_ps(power_save_type)" in station_restore
    assert "power_save_type_ = ps_type" in function_body(
        station, "void WifiStation::SetPowerSaveLevel"
    )
    assert station_restore.index("esp_wifi_start()") < station_restore.index(
        "esp_wifi_set_max_tx_power(max_tx_power_)"
    )

    assert "esp_wifi_set_mode(WIFI_MODE_APSTA)" in config_restore
    assert "esp_wifi_set_config(WIFI_IF_AP" in config_restore
    assert "esp_wifi_set_ps(WIFI_PS_NONE)" in config_restore
    assert "esp_wifi_start()" in config_restore
    assert "esp_wifi_set_band_mode" in config_restore
    assert "esp_wifi_set_max_tx_power(max_tx_power_)" in config_restore


def test_coordinator_recovery_cannot_claim_active_completion_reader():
    coordinator = read(
        "components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h"
    )
    begin = function_body(
        coordinator, "RecoveryDecision BeginRecovery"
    )
    retain = function_body(
        coordinator, "bool RetainFailedCompletion"
    )

    assert "Phase::kCompleting" not in begin
    assert "phase_ != Phase::kRunning" in begin
    assert "phase_ != Phase::kDraining" in begin
    assert "phase_ != Phase::kCompleting" in retain
    assert "phase_ = Phase::kDraining" in retain


def test_station_session_data_has_one_mutex_and_driver_calls_use_snapshots():
    header = read("components/esp-wifi-connect/include/wifi_station.h")
    source = read("components/esp-wifi-connect/wifi_station.cc")
    stop = function_body(source, "void WifiStation::Stop")
    connect = function_body(source, "std::string WifiStation::StartConnectForSession")

    assert "mutable std::mutex session_data_mutex_;" in header
    assert "session_operation_mutex_" not in header + source
    assert "std::string GetSsid() const;" in header
    assert "std::string GetIpAddress() const;" in header
    for getter in ("std::string WifiStation::GetSsid", "std::string WifiStation::GetIpAddress"):
        assert "session_data_mutex_" in function_body(source, getter)

    assert "session_data_mutex_" in stop
    assert "std::fill(password_.begin(), password_.end(), '\\0')" in stop
    assert connect.index("session_data_mutex_") < connect.index("esp_wifi_set_config(")
    data_scope = connect[connect.index("session_data_mutex_") : connect.index("esp_wifi_set_config(")]
    assert data_scope.count("}") >= 1
    assert "std::fill(password_.begin(), password_.end(), '\\0')" in connect


def test_blufi_scan_state_is_owned_by_controller_not_cross_thread_booleans():
    header = read("main/boards/common/blufi.h")

    assert '#include "blufi_wifi_scan_controller.h"' in header
    assert "BlufiWifiScanController wifi_scan_controller_;" in header
    assert "m_scan_in_progress" not in header
    assert "m_scan_should_save_ssid" not in header
    assert "m_send_list_after_scan" not in header


def test_blufi_scan_completion_claims_owner_before_reading_driver_results():
    source = read("main/boards/common/blufi.cpp")
    handler = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert handler.index("BeginCompletion(") < handler.index("esp_wifi_scan_get_ap_num")
    assert handler.index("esp_wifi_clear_ap_list") < handler.index("FinishCompletion(")


def test_blufi_scan_cache_is_published_and_consumed_under_finalization_mutex():
    source = read("main/boards/common/blufi.cpp")
    scan_done = function_body(source, "void Blufi::_wifi_scan_event_handler")
    event_body = function_body(source, "void Blufi::_handle_event")
    get_list = event_body[
        event_body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") :
        event_body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")
    ]

    scan_cache_lock = scan_done.index("provisioning_finalization_mutex_")
    scan_cache_publish = scan_done.index("self->m_ap_records.swap(")
    assert scan_cache_lock < scan_cache_publish

    get_cache_lock = get_list.index("provisioning_finalization_mutex_")
    get_cache_read = get_list.index("IsWifiScanCacheFresh()")
    get_cache_move = get_list.index("cached_ap_records.swap(m_ap_records)")
    assert get_cache_lock < get_cache_read < get_cache_move


def test_blufi_scan_cache_publish_revalidates_exact_session_under_mutex():
    source = read("main/boards/common/blufi.cpp")
    scan_done = function_body(source, "void Blufi::_wifi_scan_event_handler")
    cache_section = scan_done[scan_done.index("provisioning_finalization_mutex_") :]

    assert "completion.owner.setup_generation ==" in cache_section
    assert "completion.owner.ble_session_state ==" in cache_section
    assert "completion.owner.ble_connection_epoch ==" in cache_section
    current = cache_section.index("completion_owner_is_current")
    publish = cache_section.index("self->m_ap_records.swap(")
    assert current < publish


def test_blufi_scan_cache_records_exact_owner_when_published():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    scan_done = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert "std::optional<BlufiWifiScanController::Request> m_ap_records_owner_;" in header
    publish = scan_done.index("self->m_ap_records.swap(scanned_ap_records)")
    owner = scan_done.index("self->m_ap_records_owner_ = completion.owner")
    assert publish < owner


def test_blufi_wifi_list_rejects_stale_cache_owner_and_accepts_exact_owner():
    source = read("main/boards/common/blufi.cpp")
    event_body = function_body(source, "void Blufi::_handle_event")
    get_list = event_body[
        event_body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") :
        event_body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")
    ]

    assert "m_ap_records_owner_.has_value()" in get_list
    assert "m_ap_records_owner_->setup_generation == current_generation" in get_list
    assert "m_ap_records_owner_->ble_session_state == current_session" in get_list
    assert "m_ap_records_owner_->ble_connection_epoch == current_connection" in get_list
    exact_owner = get_list.index("cache_owner_is_current")
    freshness = get_list.index("IsWifiScanCacheFresh()")
    consume = get_list.index("cached_ap_records.swap(m_ap_records)")
    clear_owner = get_list.index("m_ap_records_owner_.reset()")
    assert exact_owner < freshness < consume < clear_owner


def test_blufi_scan_request_captures_generation_session_and_connection():
    source = read("main/boards/common/blufi.cpp")
    request = function_body(source, "void Blufi::RequestWifiListScan")

    assert "setup_generation_.load" in request
    assert "ble_session_state_.load" in request
    assert "ble_connection_epoch_.load" in request
    assert "wifi_scan_controller_.RequestScan" in request


def test_blufi_scan_session_is_invalidated_on_disconnect_restart_and_deinit():
    source = read("main/boards/common/blufi.cpp")
    disconnect = function_body(source, "case ESP_BLUFI_EVENT_BLE_DISCONNECT:")
    restart = function_body(source, "esp_err_t Blufi::RestartForSetup")
    deinit = function_body(source, "esp_err_t Blufi::_deinit_impl")

    assert "InvalidateWifiScanSession(" in disconnect
    assert "InvalidateWifiScanSession(" in restart
    assert "InvalidateWifiScanSession(" in deinit


def test_blufi_does_not_unregister_scan_handler_while_callback_is_owed():
    source = read("main/boards/common/blufi.cpp")
    deinit = function_body(source, "esp_err_t Blufi::_deinit_impl")

    assert "CanUnregisterHandler()" in deinit
    assert deinit.index("CanUnregisterHandler()") < deinit.index(
        "esp_event_handler_instance_unregister"
    )


def test_blufi_scan_invalidation_clears_deferred_list_work():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    invalidation = function_body(source, "void Blufi::InvalidateWifiScanSession")

    assert "void InvalidateWifiScanSession(" in header
    assert "wifi_scan_controller_.InvalidateSession" in invalidation
    assert "m_wifi_list_dispatch_pending_epoch_.store(0" in invalidation


def test_blufi_scan_lifecycle_refreshes_at_final_ble_session_tuples():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    init = function_body(source, "esp_err_t Blufi::_init_impl")
    event = function_body(source, "void Blufi::_handle_event")
    connect = event[
        event.index("case ESP_BLUFI_EVENT_BLE_CONNECT:") :
        event.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT:")
    ]
    disconnect = event[
        event.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT:") :
        event.index("case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:")
    ]

    assert "void UpdateWifiScanSession(" in header
    assert init.index("BleSessionPhase::kAccepting") < init.index(
        "UpdateWifiScanSession("
    )
    assert connect.index("ble_connection_epoch_.fetch_add(1") < connect.index(
        "UpdateWifiScanSession("
    )
    assert disconnect.index("BleSessionPhase::kAccepting") < disconnect.index(
        "UpdateWifiScanSession("
    )


def test_restart_invalidates_scan_after_releasing_finalization_mutex():
    source = read("main/boards/common/blufi.cpp")
    restart = function_body(source, "esp_err_t Blufi::RestartForSetup")
    lock = restart.index(
        "std::lock_guard<std::mutex> initial_state_lock(provisioning_finalization_mutex_);"
    )
    generation = restart.index("setup_generation_.fetch_add(1)", lock)
    scope_end = restart.index("}\n    InvalidateWifiScanSession(", generation)
    invalidation = restart.index("InvalidateWifiScanSession(", scope_end)

    assert lock < generation < scope_end < invalidation


def test_blufi_scan_failures_are_deferred_and_exactly_owner_bound():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    start = function_body(source, "bool Blufi::StartOwnedWifiScan")
    failure = function_body(source, "void Blufi::ScheduleWifiScanFailure")

    assert "void ScheduleWifiScanFailure(" in header
    assert "Application::GetInstance().Schedule" in failure
    assert "request.setup_generation == current_generation" in failure
    assert "request.ble_session_state == current_session" in failure
    assert "request.ble_connection_epoch == current_connection" in failure
    stale_return = failure.index("if (!failure_owner_is_current)")
    send_error = failure.index("esp_blufi_send_error_info")
    assert stale_return < send_error
    assert "ScheduleWifiScanFailure(committed.owner" in start
    assert "esp_blufi_send_error_info" not in start


def test_empty_scan_completion_uses_owner_bound_failure_helper():
    source = read("main/boards/common/blufi.cpp")
    scan_done = function_body(source, "void Blufi::_wifi_scan_event_handler")
    send_list = function_body(source, "void Blufi::_send_wifi_list")

    assert "owned_ap_records.empty()" in scan_done
    assert "ScheduleWifiScanFailure(completion.owner" in scan_done
    assert "esp_blufi_send_error_info" not in send_list


def test_blufi_wifi_scan_done_handler_is_registered_for_sta_mode_scans():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    body = function_body(source, "bool Blufi::StartOwnedWifiScan")

    assert "EnsureWifiScanEventHandlerRegistered()" in body
    assert body.index("EnsureWifiScanEventHandlerRegistered()") < body.index("esp_wifi_scan_start")
    assert "bool EnsureWifiScanEventHandlerRegistered();" in header


def test_blufi_wifi_scan_is_passive_to_preserve_internal_dma_heap():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "bool Blufi::StartOwnedWifiScan")

    assert "wifi_scan_config_t scan_config" in body
    assert "scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE" in body
    assert "scan_config.scan_time.passive = WIFI_PASSIVE_SCAN_DEFAULT_TIME" in body
    scan_start_first_args = re.findall(r"esp_wifi_scan_start\(\s*([^,]+),", body)
    assert scan_start_first_args
    assert all(arg.strip() == "&scan_config" for arg in scan_start_first_args)
    assert "esp_wifi_scan_start(NULL, false)" not in body
    assert "esp_wifi_scan_start(nullptr, false)" not in body


def test_blufi_stops_idle_wifi_radio_before_allocating_ble_list_buffers():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_send_wifi_list")

    stop_guard = body.index("!WifiManager::GetInstance().IsConnected()")
    stop_wifi = body.index("WifiManager::GetInstance().StopRadio()", stop_guard)
    send_list = body.index("esp_err_t err = esp_blufi_send_wifi_list")

    assert stop_guard < stop_wifi < send_list
    assert "Deinitialize()" not in body


def test_wifi_manager_stop_radio_releases_runtime_buffers_without_deinitializing_driver():
    source = read("components/esp-wifi-connect/wifi_manager.cc")
    header = read("components/esp-wifi-connect/include/wifi_manager.h")
    body = function_body(source, "bool WifiManager::StopRadio")

    assert "bool StopRadio();" in header
    assert "station->Stop()" in body
    assert "config_ap->Stop()" in body
    assert "esp_wifi_stop()" in body
    assert "esp_wifi_deinit()" not in body
    assert "station_.reset()" not in body
    assert "config_ap_.reset()" not in body
    assert "initialized_ = false" not in body


def test_esp32s3_blufi_build_disables_unused_ble_features_to_preserve_dma_heap():
    defaults = read("sdkconfig.defaults.esp32s3")

    assert "CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n" in defaults
    assert "CONFIG_BT_BLE_SMP_ENABLE=n" in defaults
    assert "CONFIG_BT_BLE_42_DTM_TEST_EN=n" in defaults
    assert "CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y" in defaults
    assert "CONFIG_BT_BLE_42_ADV_EN=y" in defaults


def test_esp32s3_blufi_build_is_sized_for_one_peripheral_connection():
    defaults = read("sdkconfig.defaults.esp32s3")

    assert "CONFIG_BT_GATTC_ENABLE=n" in defaults
    # Bluedroid reserves GAP and GATT profiles before registering BluFi.
    assert "CONFIG_BT_GATT_MAX_SR_PROFILES=3" in defaults
    assert "CONFIG_BT_GATT_MAX_SR_ATTRIBUTES=16" in defaults
    assert "CONFIG_BT_ACL_CONNECTIONS=1" in defaults
    assert "CONFIG_BT_MULTI_CONNECTION_ENBALE=n" in defaults
    assert "CONFIG_BT_CTRL_BLE_MAX_ACT=2" in defaults
    assert "CONFIG_BT_CTRL_DTM_ENABLE=n" in defaults
    assert "CONFIG_BT_CTRL_BLE_SCAN=n" in defaults
    assert "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE=n" in defaults


def test_blufi_scan_reinitializes_wifi_after_list_dispatch_teardown():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "bool Blufi::StartOwnedWifiScan")

    initialize = body.index("wifi_manager.Initialize()")
    get_mode = body.index("esp_wifi_get_mode")
    station_capable = body.index(
        "current_mode == WIFI_MODE_STA || current_mode == WIFI_MODE_APSTA"
    )

    assert initialize < get_mode < station_capable


def test_blufi_scan_recovers_when_wifi_mode_read_fails():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "bool Blufi::StartOwnedWifiScan")

    get_mode = body.index("esp_wifi_get_mode(&current_mode)")
    read_failure = body.index("if (err != ESP_OK)", get_mode)
    set_fallback = body.index("current_mode = WIFI_MODE_NULL;", read_failure)
    set_station = body.index("esp_wifi_set_mode(WIFI_MODE_STA)", set_fallback)

    assert get_mode < read_failure < set_fallback < set_station
    assert "Failed to read WiFi mode before scan" in body
    assert "Failed to get WiFi mode" not in body


def test_blufi_scan_uses_one_passive_start_after_mode_is_station_capable():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "bool Blufi::StartOwnedWifiScan")

    assert "current_mode == WIFI_MODE_STA || current_mode == WIFI_MODE_APSTA" in body
    assert "Unexpected WiFi mode" not in body
    assert body.count("esp_wifi_scan_start(&scan_config, false)") == 1
    assert body.index("esp_wifi_set_mode(WIFI_MODE_STA)") < body.index(
        "esp_wifi_scan_start(&scan_config, false)"
    )
    assert body.count("err != ESP_OK && err != ESP_ERR_WIFI_STATE") == 2


def test_blufi_wifi_scan_failures_log_start_and_empty_result_separately():
    source = read("main/boards/common/blufi.cpp")
    start = function_body(source, "bool Blufi::StartOwnedWifiScan")
    scan_done = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert 'ScheduleWifiScanFailure(committed.owner, "scan_start_failed")' in start
    assert '"scan_completed_without_ap_records"' in scan_done


def test_blufi_logs_heap_around_connection_and_wifi_list_dispatch():
    source = read("main/boards/common/blufi.cpp")
    send_list = function_body(source, "void Blufi::_send_wifi_list")
    handler = function_body(source, "void Blufi::_handle_event")
    init_event = handler[
        handler.index("case ESP_BLUFI_EVENT_INIT_FINISH") :
        handler.index("case ESP_BLUFI_EVENT_DEINIT_FINISH")
    ]
    connect_event = handler[
        handler.index("case ESP_BLUFI_EVENT_BLE_CONNECT") :
        handler.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT")
    ]

    assert 'LogBlufiHeapSnapshot("blufi_init_finish")' in init_event
    assert 'LogBlufiHeapSnapshot("ble_connect")' in connect_event
    assert 'LogBlufiHeapSnapshot("wifi_list_before_dispatch")' in send_list
    assert 'LogBlufiHeapSnapshot("wifi_list_after_dispatch")' in send_list
    assert send_list.index("wifi_list_before_dispatch") < send_list.index(
        "esp_err_t err = esp_blufi_send_wifi_list"
    ) < send_list.index("wifi_list_after_dispatch")


def test_blufi_wifi_scan_caps_application_owned_candidates():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_wifi_scan_event_handler")

    cap = "ap_num = std::min<uint16_t>(ap_num, kMaxBlufiWifiScanCandidates);"
    assert "kMaxBlufiWifiScanCandidates" in source
    assert cap in body
    assert body.index(cap) < body.index("scanned_ap_records.resize(ap_num)")
    assert body.index(cap) < body.index("esp_wifi_scan_get_ap_records(")


def test_blufi_wifi_list_dispatch_is_deferred_and_guarded_until_scan_callback_returns():
    source = read("main/boards/common/blufi.cpp")
    handler = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert "ScheduleWifiListSend" in handler
    assert "_send_wifi_list();" not in handler

    helper = function_body(source, "void Blufi::ScheduleWifiListSend")
    assert "Application::GetInstance().Schedule" in helper
    assert "RunIfSetupGenerationCurrent" in helper
    assert "m_ble_is_connected" in helper


def test_blufi_deferred_wifi_list_is_bound_to_exact_ble_connection():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    handler = function_body(source, "void Blufi::_wifi_scan_event_handler")
    helper = function_body(source, "void Blufi::ScheduleWifiListSend")
    connect = source[
        source.index("case ESP_BLUFI_EVENT_BLE_CONNECT") :
        source.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT")
    ]
    disconnect = source[
        source.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT") :
        source.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST")
    ]

    assert "uint64_t expected_ble_session_state" in header
    assert "uint64_t expected_ble_connection_epoch" in header
    assert "ble_connection_epoch_" in header
    assert "ble_session_state_.load(std::memory_order_acquire)" in handler
    assert "ble_connection_epoch_.load(std::memory_order_acquire)" in handler
    assert "expected_ble_session_state" in helper
    assert "expected_ble_connection_epoch" in helper
    assert "current_ble_session_state != expected_ble_session_state" in helper
    assert "DecodeBleSessionPhase(expected_ble_session_state)" in helper
    assert "DecodeBleSessionPhase(current_ble_session_state)" in helper
    assert helper.count("BleSessionPhase::kConnected") >= 2
    assert "current_ble_connection_epoch != expected_ble_connection_epoch" in helper
    assert "ble_connection_epoch_.fetch_add(1" in connect
    assert "provisioning_finalization_mutex_" in handler
    assert "provisioning_finalization_mutex_" in connect
    assert "provisioning_finalization_mutex_" in disconnect
    assert "m_wifi_list_dispatch_epoch_.fetch_add(1" in disconnect
    assert "completion.owner.ble_connection_epoch" in handler


def test_blufi_wifi_list_retry_waits_for_owned_deferred_dispatch():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    handler = function_body(source, "void Blufi::_wifi_scan_event_handler")
    event_body = function_body(source, "void Blufi::_handle_event")
    get_list = event_body[
        event_body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") :
        event_body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")
    ]

    assert "m_wifi_list_dispatch_pending_epoch_" in header
    pending_check = get_list.index("m_wifi_list_dispatch_pending_epoch_")
    assert pending_check < get_list.index("IsWifiScanCacheFresh()")
    assert pending_check < get_list.index("m_ap_records.clear();")
    assert pending_check < get_list.index("RequestWifiListScan(true, true)")

    assert "std::vector<wifi_ap_record_t> owned_ap_records" in handler
    transfer = "owned_ap_records.swap(self->m_ap_records);"
    assert transfer in handler
    assert handler.index(transfer) < handler.index("FinishCompletion(")
    assert "std::move(owned_ap_records)" in handler


def test_blufi_stale_deferred_dispatch_only_destroys_its_owned_records():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    helper = function_body(source, "void Blufi::ScheduleWifiListSend")
    restart = function_body(source, "esp_err_t Blufi::RestartForSetup")
    event_body = function_body(source, "void Blufi::_handle_event")
    disconnect = event_body[
        event_body.index("case ESP_BLUFI_EVENT_BLE_DISCONNECT") :
        event_body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST")
    ]

    assert "uint64_t expected_wifi_list_dispatch_epoch" in header
    assert "std::vector<wifi_ap_record_t> ap_records" in header
    assert "m_wifi_list_dispatch_epoch_" in header
    assert "m_wifi_list_dispatch_pending_epoch_" in header
    assert "expected_wifi_list_dispatch_epoch" in helper
    assert "current_wifi_list_dispatch_epoch != expected_wifi_list_dispatch_epoch" in helper
    assert "compare_exchange_strong" in helper
    assert "_send_wifi_list(std::move(ap_records))" in helper
    assert "swap(m_ap_records)" not in helper
    assert "m_ap_records.clear()" not in helper
    assert "m_wifi_list_dispatch_epoch_.fetch_add(1" in restart
    assert "m_wifi_list_dispatch_epoch_.fetch_add(1" in disconnect


def test_blufi_wifi_scan_handler_registration_is_single_owner_and_unregistered_on_deinit():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    ensure_body = function_body(source, "bool Blufi::EnsureWifiScanEventHandlerRegistered")
    deinit_body = function_body(source, "esp_err_t Blufi::_deinit_impl")

    assert "scan_event_instance_" in header
    assert "esp_event_handler_instance_register" in ensure_body
    assert "WIFI_EVENT_SCAN_DONE" in ensure_body
    assert "scan_event_instance_ != nullptr" in ensure_body
    assert "esp_event_handler_instance_unregister" in deinit_body
    assert "scan_event_instance_ = nullptr" in deinit_body

def test_blufi_scan_done_handler_ignores_scan_events_it_did_not_start():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert "!completion.owned_callback" in body
    assert body.index("!completion.owned_callback") < body.index("esp_wifi_scan_get_ap_num")
    assert "Ignoring WiFi scan done event not owned by BluFi" in body

def test_blufi_wifi_list_requests_refresh_stale_cached_scan_results():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    body = function_body(source, "void Blufi::_handle_event")
    get_list = body[body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") : body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")]

    assert "m_ap_records_updated_us" in header
    assert "kWifiScanCacheMaxAgeUs" in header
    assert "IsWifiScanCacheFresh()" in header
    assert "IsWifiScanCacheFresh()" in source
    assert "cache_owner_is_current && !m_ap_records.empty()" in get_list
    assert "IsWifiScanCacheFresh()" in get_list
    fresh_idx = get_list.index("cache_owner_is_current && !m_ap_records.empty()")
    assert "_send_wifi_list(" in get_list[fresh_idx:]
    assert "m_ap_records.clear();" in get_list

def test_blufi_init_does_not_start_an_eager_wifi_scan():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "esp_err_t Blufi::_init_impl")

    assert "RequestWifiListScan(" not in body
    assert "StartOwnedWifiScan(" not in body

def test_blufi_init_resets_ble_timeout_latch_for_fresh_setup_window():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "esp_err_t Blufi::_init_impl")

    assert "ble_timed_out_ = false;" in body
    assert body.index("ble_timed_out_ = false;") < body.index("_controller_init()")

def test_blufi_wifi_list_coalesces_inflight_scan_with_app_visible_request():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_handle_event")
    get_list = body[body.index("case ESP_BLUFI_EVENT_GET_WIFI_LIST") : body.index("case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")]

    assert "RequestWifiListScan(true, true);" in get_list
    request = function_body(source, "void Blufi::RequestWifiListScan")
    assert "wifi_scan_controller_.RequestScan(request)" in request

def test_wifi_station_does_not_consume_blufi_owned_scan_results():
    source = read("managed_components/78__esp-wifi-connect/wifi_station.cc")
    header = read("managed_components/78__esp-wifi-connect/include/wifi_station.h")
    handler = function_body(source, "void WifiStation::WifiEventHandler")

    assert "scan_in_progress_" not in header + source
    assert "std::optional<WifiScanLeaseCoordinator::Lease> scan_lease_;" in header
    assert "bool StartOwnedScan();" in header
    assert "bool WifiStation::StartOwnedScan()" in source
    assert "ObserveScanDone(lease)" in handler
    assert handler.index("ObserveScanDone(lease)") < handler.index(
        "CompleteOwnedScan(lease)"
    )
    assert "Ignoring WiFi scan done event not owned by WifiStation" in handler
