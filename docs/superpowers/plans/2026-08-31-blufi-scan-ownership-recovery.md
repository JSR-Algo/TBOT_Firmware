# BluFi Wi-Fi Scan Ownership Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make BluFi Wi-Fi discovery recover from stale or missing ESP-IDF scan callbacks and a missing Wi-Fi driver without sending results across BLE/setup sessions, rebooting, or erasing NVS.

**Architecture:** Introduce a host-testable scan ownership controller that serializes request, completion, invalidation, pending-request, and recovery state under one mutex. BluFi owns exactly one physical nonblocking scan until its callback is consumed or a lifecycle-owned driver reset plus default-event-loop FIFO barrier proves the old callback incarnation is gone. `WifiManager` adds driver-only reconciliation/recovery operations that never repeat NVS, netif, or default-event-loop initialization.

**Tech Stack:** C++17, ESP-IDF Wi-Fi and event loop, FreeRTOS timers/semaphores, BluFi, pytest source-contract tests, host-native threaded tests, ESP32-S3 hardware E2E.

---

## File Map

- Create `main/boards/common/blufi_wifi_scan_controller.h`: mutex-owned, ESP-independent scan state machine.
- Create `tests/native/blufi_wifi_scan_controller_host_test.cc`: deterministic request/completion/restart/recovery interleavings.
- Create `scripts/run_host_native_blufi_wifi_scan_controller_test.sh`: compile and run the host model with warnings as errors.
- Create `tests/test_blufi_wifi_scan_controller_native.py`: pytest entry point for the native runner.
- Modify `main/boards/common/blufi.h`: replace cross-thread scan booleans with controller, watchdog, barrier, and helper declarations.
- Modify `main/boards/common/blufi.cpp`: integrate request ownership, callback consumption, lifecycle invalidation, pending restart, watchdog, driver reset, and event-loop barrier.
- Modify `components/esp-wifi-connect/include/wifi_manager.h`: declare driver reconciliation and scan-recovery APIs.
- Modify `components/esp-wifi-connect/wifi_manager.cc`: implement driver-only reinit and controlled stop/deinit/barrier/reinit.
- Modify `tests/test_blufi_wifi_scan_contract.py`: source contracts for controller ownership, passive scan preservation, recovery, and driver reconciliation.
- Modify `tests/test_blufi_provisioning_stability.py`: lifecycle invalidation and no-cross-session delivery contracts.
- Modify `tests/native_stubs/wifi_manager.h`: keep host/native compilation stubs aligned with the new public methods.

### Task 1: Build The Host-Testable Scan Ownership Controller

**Files:**
- Create: `main/boards/common/blufi_wifi_scan_controller.h`
- Create: `tests/native/blufi_wifi_scan_controller_host_test.cc`
- Create: `scripts/run_host_native_blufi_wifi_scan_controller_test.sh`
- Create: `tests/test_blufi_wifi_scan_controller_native.py`

- [ ] **Step 1: Write the failing native test for restart and delayed completion**

Create `tests/native/blufi_wifi_scan_controller_host_test.cc` with a first scenario that expresses the required API:

```cpp
#include "../../main/boards/common/blufi_wifi_scan_controller.h"

#include <cassert>
#include <iostream>

using Controller = BlufiWifiScanController;

static Controller::Request Request(uint32_t generation, uint64_t session,
                                   uint64_t connection, bool send = true) {
    return Controller::Request{
        .setup_generation = generation,
        .ble_session_state = session,
        .ble_connection_epoch = connection,
        .save_results = true,
        .send_list = send,
    };
}

void DelayedOldCompletionCannotSatisfyNewRequest() {
    Controller controller;
    auto first = controller.RequestScan(Request(1, 11, 101));
    assert(first.start_now);
    assert(controller.CommitStart(first.request_id, true).accepted);

    controller.InvalidateSession(2, 22, 202);
    auto second = controller.RequestScan(Request(2, 22, 202));
    assert(second.queued);

    auto old = controller.BeginCompletion(2, 22, 202);
    assert(old.owned_callback);
    assert(old.discard_results);
    assert(!old.send_list);

    auto drained = controller.FinishCompletion(old.request_id);
    assert(drained.start_pending);
    assert(drained.pending.setup_generation == 2);
    assert(drained.pending.ble_connection_epoch == 202);
}

int main() {
    DelayedOldCompletionCannotSatisfyNewRequest();
    std::cout << "PASS: BluFi WiFi scan controller host model\n";
    return 0;
}
```

- [ ] **Step 2: Add the native runner and pytest wrapper**

Create `scripts/run_host_native_blufi_wifi_scan_controller_test.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

"${CXX:-c++}" -std=c++17 -pthread -Wall -Wextra -Werror \
  "${repo_root}/tests/native/blufi_wifi_scan_controller_host_test.cc" \
  -o "${build_dir}/blufi_wifi_scan_controller_host_test"

"${build_dir}/blufi_wifi_scan_controller_host_test"
```

Create `tests/test_blufi_wifi_scan_controller_native.py`:

```python
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_blufi_wifi_scan_controller_host_model():
    subprocess.run(
        [str(ROOT / "scripts/run_host_native_blufi_wifi_scan_controller_test.sh")],
        cwd=ROOT,
        check=True,
    )
```

- [ ] **Step 3: Run the native test and verify RED**

Run:

```bash
chmod +x scripts/run_host_native_blufi_wifi_scan_controller_test.sh
python3 -m pytest -q tests/test_blufi_wifi_scan_controller_native.py
```

Expected: FAIL because `main/boards/common/blufi_wifi_scan_controller.h` does not exist.

- [ ] **Step 4: Implement the minimal controller API**

Create `main/boards/common/blufi_wifi_scan_controller.h` with the following public model and mutex-owned transitions:

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

class BlufiWifiScanController {
public:
    enum class Phase : uint8_t { kIdle, kStarting, kRunning, kDraining };

    struct Request {
        uint32_t setup_generation = 0;
        uint64_t ble_session_state = 0;
        uint64_t ble_connection_epoch = 0;
        bool save_results = true;
        bool send_list = false;
    };

    struct RequestDecision {
        uint64_t request_id = 0;
        bool start_now = false;
        bool queued = false;
    };

    struct StartDecision {
        bool accepted = false;
        bool send_failure = false;
        bool draining = false;
        bool start_pending = false;
        uint64_t pending_request_id = 0;
        Request pending;
    };

    struct CompletionDecision {
        uint64_t request_id = 0;
        bool owned_callback = false;
        bool discard_results = true;
        bool save_results = false;
        bool send_list = false;
        Request owner;
    };

    struct FinishDecision {
        bool start_pending = false;
        uint64_t request_id = 0;
        Request pending;
    };

    struct RecoveryTicket {
        uint64_t request_id = 0;
        uint32_t driver_incarnation = 0;
        bool valid = false;
    };

    RequestDecision RequestScan(const Request& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::kIdle) {
            owner_ = request;
            owner_request_id_ = ++last_request_id_;
            phase_ = Phase::kStarting;
            callback_claimed_ = false;
            invalidated_ = false;
            return {.request_id = owner_request_id_, .start_now = true};
        }
        pending_ = request;
        return {.request_id = owner_request_id_, .queued = true};
    }

    StartDecision CommitStart(uint64_t request_id, bool accepted) {
        std::lock_guard<std::mutex> lock(mutex_);
        StartDecision result;
        if (request_id != owner_request_id_) {
            return result;
        }
        if (callback_claimed_) {
            result.accepted = accepted;
            result.draining = phase_ == Phase::kDraining;
            return result;
        }
        if (phase_ != Phase::kStarting) {
            return result;
        }
        if (!accepted) {
            result.send_failure = !invalidated_ && owner_.send_list;
            phase_ = Phase::kIdle;
            owner_request_id_ = 0;
            invalidated_ = false;
            if (pending_) {
                owner_ = *pending_;
                pending_.reset();
                owner_request_id_ = ++last_request_id_;
                phase_ = Phase::kStarting;
                result.start_pending = true;
                result.pending_request_id = owner_request_id_;
                result.pending = owner_;
            }
            return result;
        }
        result.accepted = true;
        phase_ = invalidated_ ? Phase::kDraining : Phase::kRunning;
        result.draining = phase_ == Phase::kDraining;
        return result;
    }

    CompletionDecision BeginCompletion(uint32_t current_generation,
                                       uint64_t current_session,
                                       uint64_t current_connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        CompletionDecision result;
        if (callback_claimed_ ||
            (phase_ != Phase::kStarting && phase_ != Phase::kRunning &&
             phase_ != Phase::kDraining)) {
            return result;
        }
        // A callback racing CommitStart proves ESP-IDF accepted the scan.
        if (phase_ == Phase::kStarting) {
            phase_ = invalidated_ ? Phase::kDraining : Phase::kRunning;
        }
        callback_claimed_ = true;
        result.request_id = owner_request_id_;
        result.owned_callback = true;
        result.owner = owner_;
        const bool current = phase_ == Phase::kRunning && !invalidated_ &&
            owner_.setup_generation == current_generation &&
            owner_.ble_session_state == current_session &&
            owner_.ble_connection_epoch == current_connection;
        result.discard_results = !current;
        result.save_results = current && owner_.save_results;
        result.send_list = current && owner_.send_list;
        return result;
    }

    FinishDecision FinishCompletion(uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        FinishDecision result;
        if (request_id != owner_request_id_ || !callback_claimed_) {
            return result;
        }
        phase_ = Phase::kIdle;
        owner_request_id_ = 0;
        callback_claimed_ = false;
        invalidated_ = false;
        if (pending_) {
            result.start_pending = true;
            owner_ = *pending_;
            pending_.reset();
            owner_request_id_ = ++last_request_id_;
            phase_ = Phase::kStarting;
            result.request_id = owner_request_id_;
            result.pending = owner_;
        }
        return result;
    }

    void InvalidateSession(uint32_t current_generation, uint64_t current_session,
                           uint64_t current_connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ &&
            (pending_->setup_generation != current_generation ||
             pending_->ble_session_state != current_session ||
             pending_->ble_connection_epoch != current_connection)) {
            pending_.reset();
        }
        if (phase_ == Phase::kStarting || phase_ == Phase::kRunning) {
            invalidated_ = true;
            if (phase_ == Phase::kRunning) {
                phase_ = Phase::kDraining;
            }
        }
    }

    RecoveryTicket BeginRecovery(uint64_t expected_request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((phase_ != Phase::kRunning && phase_ != Phase::kDraining) || recovering_) {
            return {};
        }
        if (expected_request_id != owner_request_id_) {
            return {};
        }
        recovering_ = true;
        invalidated_ = true;
        phase_ = Phase::kDraining;
        return {.request_id = owner_request_id_,
                .driver_incarnation = driver_incarnation_,
                .valid = true};
    }

    FinishDecision CompleteRecovery(const RecoveryTicket& ticket, bool success) {
        std::lock_guard<std::mutex> lock(mutex_);
        FinishDecision result;
        if (!ticket.valid || !recovering_ || ticket.request_id != owner_request_id_ ||
            ticket.driver_incarnation != driver_incarnation_) {
            return result;
        }
        recovering_ = false;
        if (!success) {
            return result;
        }
        ++driver_incarnation_;
        phase_ = Phase::kIdle;
        owner_request_id_ = 0;
        callback_claimed_ = false;
        invalidated_ = false;
        if (pending_) {
            result.start_pending = true;
            owner_ = *pending_;
            pending_.reset();
            owner_request_id_ = ++last_request_id_;
            phase_ = Phase::kStarting;
            result.request_id = owner_request_id_;
            result.pending = owner_;
        }
        return result;
    }

    Phase phase() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return phase_;
    }

private:
    mutable std::mutex mutex_;
    Phase phase_ = Phase::kIdle;
    Request owner_;
    std::optional<Request> pending_;
    uint64_t last_request_id_ = 0;
    uint64_t owner_request_id_ = 0;
    uint32_t driver_incarnation_ = 1;
    bool callback_claimed_ = false;
    bool invalidated_ = false;
    bool recovering_ = false;
};
```

- [ ] **Step 5: Expand the native test to cover threading, failed start, and recovery**

Add cases that assert:

```cpp
void FailedStartDoesNotReplyToInvalidatedSession();
void ConcurrentRequestQueuesExactlyOnePendingRequest();
void DrainingCompletionDiscardsAndStartsPendingOnce();
void LostCallbackRecoveryAdvancesDriverBeforePendingStart();
void CallbackAndInvalidateSerializeWithoutLosingSendFlag();
```

Use `std::thread`, `std::condition_variable`, and barriers rather than sleeps. In the recovery case, require `CompleteRecovery(ticket, true)` before `start_pending` becomes true.

- [ ] **Step 6: Run the native model and verify GREEN**

Run:

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_controller_native.py
```

Expected: `1 passed` and runner output `PASS: BluFi WiFi scan controller host model`.

- [ ] **Step 7: Commit the controller**

```bash
git add main/boards/common/blufi_wifi_scan_controller.h \
  tests/native/blufi_wifi_scan_controller_host_test.cc \
  scripts/run_host_native_blufi_wifi_scan_controller_test.sh \
  tests/test_blufi_wifi_scan_controller_native.py
git commit -m "test(blufi): model WiFi scan ownership"
```

### Task 2: Integrate Owned Request And Completion Flow Into BluFi

**Files:**
- Modify: `main/boards/common/blufi.h`
- Modify: `main/boards/common/blufi.cpp`
- Modify: `tests/test_blufi_wifi_scan_contract.py`
- Modify: `tests/test_blufi_provisioning_stability.py`

- [ ] **Step 1: Write failing source contracts for controller-only scan state**

Add tests requiring:

```python
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


def test_blufi_scan_request_captures_generation_session_and_connection():
    source = read("main/boards/common/blufi.cpp")
    request = function_body(source, "void Blufi::RequestWifiListScan")
    assert "setup_generation_.load" in request
    assert "ble_session_state_.load" in request
    assert "ble_connection_epoch_.load" in request
    assert "wifi_scan_controller_.RequestScan" in request
```

- [ ] **Step 2: Run the contracts and verify RED**

Run:

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'controller_not_cross_thread or claims_owner or captures_generation'
```

Expected: FAIL because BluFi still uses the three plain booleans.

- [ ] **Step 3: Replace the fields and declare bounded helpers**

In `main/boards/common/blufi.h`:

```cpp
#include "blufi_wifi_scan_controller.h"

BlufiWifiScanController wifi_scan_controller_;
esp_event_handler_instance_t scan_event_instance_ = nullptr;

void RequestWifiListScan(bool save_results, bool send_list);
bool StartOwnedWifiScan(uint64_t request_id);
void SchedulePendingWifiScan(uint64_t request_id,
                             const BlufiWifiScanController::Request& request);
void InvalidateWifiScanSession();
```

Remove `m_scan_in_progress`, `m_scan_should_save_ssid`, and `m_send_list_after_scan`. Keep AP cache and dispatch epochs.

- [ ] **Step 4: Convert GET_WIFI_LIST to the request helper**

Implement a helper that snapshots ownership before requesting a scan:

```cpp
void Blufi::RequestWifiListScan(bool save_results, bool send_list) {
    BlufiWifiScanController::Request request{
        .setup_generation = setup_generation_.load(std::memory_order_acquire),
        .ble_session_state = ble_session_state_.load(std::memory_order_acquire),
        .ble_connection_epoch = ble_connection_epoch_.load(std::memory_order_acquire),
        .save_results = save_results,
        .send_list = send_list,
    };
    const auto decision = wifi_scan_controller_.RequestScan(request);
    if (decision.start_now) {
        StartOwnedWifiScan(decision.request_id);
    }
}
```

In `ESP_BLUFI_EVENT_GET_WIFI_LIST`, preserve the fresh-cache response. When no fresh cache exists, call `RequestWifiListScan(true, true)` once. A request received during `Running` or `Draining` is coalesced by the controller.

- [ ] **Step 5: Make scan submission two-phase and preserve passive scanning**

Refactor `start_wifi_scan()` into `StartOwnedWifiScan(uint64_t request_id)`. Keep:

```cpp
wifi_scan_config_t scan_config{};
scan_config.show_hidden = false;
scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
scan_config.scan_time.passive = WIFI_PASSIVE_SCAN_DEFAULT_TIME;
```

After preparation, call exactly:

```cpp
const esp_err_t scan_error = esp_wifi_scan_start(&scan_config, false);
const auto committed = wifi_scan_controller_.CommitStart(
    request_id, scan_error == ESP_OK);
```

If `committed.send_failure` is true, log `reason=scan_start_failed` and send `ESP_BLUFI_WIFI_SCAN_FAIL`. If the request was invalidated while the ESP call ran and the start succeeds, the controller must leave it `Draining`, not `Idle`.

If `committed.start_pending` is true after a synchronous start failure, call `SchedulePendingWifiScan(committed.pending_request_id, committed.pending)` so a coalesced current-session request is not stranded.

- [ ] **Step 6: Claim completion before touching driver-owned results**

At the start of `_wifi_scan_event_handler`, snapshot current generation/session/connection atomics and call:

```cpp
const auto completion = self->wifi_scan_controller_.BeginCompletion(
    self->setup_generation_.load(std::memory_order_acquire),
    self->ble_session_state_.load(std::memory_order_acquire),
    self->ble_connection_epoch_.load(std::memory_order_acquire));
if (!completion.owned_callback) {
    ESP_LOGI(BLUFI_TAG, "Ignoring WiFi scan done event not owned by BluFi");
    return;
}
```

For `discard_results`, call `esp_wifi_clear_ap_list()` and skip all cache/send work. For a current owner, retain the existing cap and record retrieval, controlled by `completion.save_results`. Move records into `ScheduleWifiListSend` only when `completion.send_list` is true.

Call `FinishCompletion(completion.request_id)` only after `esp_wifi_clear_ap_list()` and record ownership are complete. If it returns `start_pending`, schedule `SchedulePendingWifiScan(result.request_id, result.pending)` on the Application task. `FinishCompletion` has already reserved that pending request as `Starting`; do not call `RequestScan` a second time.

- [ ] **Step 7: Keep pending scan start off the event-loop task**

Implement:

```cpp
void Blufi::SchedulePendingWifiScan(
        uint64_t request_id,
        const BlufiWifiScanController::Request& request) {
    Application::GetInstance().Schedule([this, request_id, request]() {
        if (request.setup_generation != setup_generation_.load(std::memory_order_acquire) ||
            request.ble_session_state != ble_session_state_.load(std::memory_order_acquire) ||
            request.ble_connection_epoch != ble_connection_epoch_.load(std::memory_order_acquire)) {
            wifi_scan_controller_.InvalidateSession(
                setup_generation_.load(std::memory_order_acquire),
                ble_session_state_.load(std::memory_order_acquire),
                ble_connection_epoch_.load(std::memory_order_acquire));
            return;
        }
        StartOwnedWifiScan(request_id);
    });
}
```

Do not call `esp_wifi_scan_start()` from the ESP event-loop callback.

- [ ] **Step 8: Run focused tests and native model**

Run:

```bash
python3 -m pytest -q \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py
```

Expected: all pass.

- [ ] **Step 9: Commit request/completion ownership**

```bash
git add main/boards/common/blufi.h main/boards/common/blufi.cpp \
  tests/test_blufi_wifi_scan_contract.py tests/test_blufi_provisioning_stability.py
git commit -m "fix(blufi): own WiFi scan callbacks"
```

### Task 3: Invalidate Scan Ownership Across BLE Lifecycle Changes

**Files:**
- Modify: `main/boards/common/blufi.cpp`
- Modify: `tests/test_blufi_wifi_scan_contract.py`
- Modify: `tests/test_blufi_provisioning_stability.py`

- [ ] **Step 1: Write failing lifecycle invalidation contracts**

Require `InvalidateWifiScanSession()` in:

```python
def test_blufi_scan_session_is_invalidated_on_disconnect_restart_and_deinit():
    source = read("main/boards/common/blufi.cpp")
    disconnect = function_body(source, "case ESP_BLUFI_EVENT_BLE_DISCONNECT:")
    restart = function_body(source, "esp_err_t Blufi::RestartForSetup")
    deinit = function_body(source, "esp_err_t Blufi::_deinit_impl")
    assert "InvalidateWifiScanSession();" in disconnect
    assert "InvalidateWifiScanSession();" in restart
    assert "InvalidateWifiScanSession();" in deinit


def test_blufi_does_not_unregister_scan_handler_while_callback_is_owed():
    source = read("main/boards/common/blufi.cpp")
    deinit = function_body(source, "esp_err_t Blufi::_deinit_impl")
    assert "CanUnregisterHandler()" in deinit
    assert deinit.index("CanUnregisterHandler()") < deinit.index(
        "esp_event_handler_instance_unregister"
    )
```

- [ ] **Step 2: Run and verify RED**

Run:

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'invalidated_on_disconnect or unregister_scan_handler'
```

Expected: FAIL because deinit currently clears flags and always unregisters.

- [ ] **Step 3: Add controller queries for handler and watchdog ownership**

Add to `BlufiWifiScanController`:

```cpp
bool CallbackOwed() const;
bool CanUnregisterHandler() const;
std::optional<RecoveryTicket> RecoveryTicketIfOwned(
    uint64_t request_id, uint32_t driver_incarnation) const;
```

`CanUnregisterHandler()` returns true only in `Idle` with no recovery in progress.

- [ ] **Step 4: Implement one invalidation helper**

```cpp
void Blufi::InvalidateWifiScanSession() {
    wifi_scan_controller_.InvalidateSession(
        setup_generation_.load(std::memory_order_acquire),
        ble_session_state_.load(std::memory_order_acquire),
        ble_connection_epoch_.load(std::memory_order_acquire));
    m_wifi_list_dispatch_pending_epoch_.store(0, std::memory_order_release);
}
```

Call it after generation/session/connection state changes on BLE disconnect, at setup restart, and at deinit. Do not hold the scan controller mutex while acquiring lifecycle/finalization locks; the controller method owns only its internal lock.

- [ ] **Step 5: Preserve the handler until the callback drains**

In `_deinit_impl`, replace unconditional handler unregistration with:

```cpp
InvalidateWifiScanSession();
if (scan_event_instance_ != nullptr && wifi_scan_controller_.CanUnregisterHandler()) {
    esp_event_handler_instance_unregister(
        WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_event_instance_);
    scan_event_instance_ = nullptr;
}
```

The singleton handler may remain registered while BLE is down; it only consumes/discards the owed Wi-Fi callback and performs no BLE send.

- [ ] **Step 6: Add delayed-old-event native choreography**

Extend the host model so restart invalidates `Running`, queues a new request, delivers the old callback to the same controller, and proves the pending request starts only after `FinishCompletion`.

- [ ] **Step 7: Run lifecycle suites**

```bash
python3 -m pytest -q \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_blufi_deinit_transaction.py
```

Expected: all pass.

- [ ] **Step 8: Commit lifecycle invalidation**

```bash
git add main/boards/common/blufi_wifi_scan_controller.h \
  main/boards/common/blufi.cpp \
  tests/native/blufi_wifi_scan_controller_host_test.cc \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py
git commit -m "fix(blufi): drain stale WiFi scan callbacks"
```

### Task 4: Reconcile A Missing Wi-Fi Driver Without Reinitializing Global Services

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Modify: `tests/native_stubs/wifi_manager.h`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] **Step 1: Write the failing WifiManager source contract**

Add:

```python
def test_wifi_manager_reconciles_missing_driver_without_global_reinit():
    header = read("components/esp-wifi-connect/include/wifi_manager.h")
    source = read("components/esp-wifi-connect/wifi_manager.cc")
    body = function_body(source, "bool WifiManager::EnsureDriverReadyForProvisioningScan")
    assert "bool EnsureDriverReadyForProvisioningScan();" in header
    assert "esp_wifi_get_mode" in body
    assert "ESP_ERR_WIFI_NOT_INIT" in body
    assert "WIFI_INIT_CONFIG_DEFAULT" in body
    assert "cfg.nvs_enable = false" in body
    assert "nvs_flash_init" not in body
    assert "esp_netif_init" not in body
    assert "esp_event_loop_create_default" not in body
```

- [ ] **Step 2: Run and verify RED**

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k reconciles_missing_driver
```

Expected: FAIL because the method does not exist.

- [ ] **Step 3: Declare and implement driver reconciliation**

Add to `wifi_manager.h`:

```cpp
bool EnsureDriverReadyForProvisioningScan();
```

Implement under `WifiManager::mutex_`:

```cpp
bool WifiManager::EnsureDriverReadyForProvisioningScan() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t probe = esp_wifi_get_mode(&mode);
    if (probe == ESP_OK) {
        return true;
    }
    if (probe != ESP_ERR_WIFI_NOT_INIT || station_active_ || config_mode_active_) {
        ESP_LOGE(TAG, "WiFi driver unavailable for provisioning scan: %s",
                 esp_err_to_name(probe));
        return false;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    const esp_err_t init = esp_wifi_init(&cfg);
    if (init != ESP_OK) {
        ESP_LOGE(TAG, "WiFi driver-only reinit failed: %s", esp_err_to_name(init));
        return false;
    }
    ESP_LOGW(TAG, "WiFi driver reconciled for provisioning scan");
    return true;
}
```

Do not recreate `station_` or `config_ap_`; inactive helper objects remain valid because their driver/netif handlers are created only on `Start()`.

- [ ] **Step 4: Use reconciliation before mode selection**

In `StartOwnedWifiScan`, require:

```cpp
auto& wifi_manager = WifiManager::GetInstance();
if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
    // Commit start failure for the owning request.
}
if (!wifi_manager.EnsureDriverReadyForProvisioningScan()) {
    // Commit start failure for the owning request.
}
```

After this succeeds, treat any later `esp_wifi_get_mode()` error as a start failure rather than pretending the driver exists.

- [ ] **Step 5: Update native stubs and run tests**

Add a stub returning true:

```cpp
bool EnsureDriverReadyForProvisioningScan() { return true; }
```

Run:

```bash
python3 -m pytest -q \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_wifi_manager_contract.py
```

If `tests/test_wifi_manager_contract.py` is absent, run all tests matching `*wifi_manager*` via `rg --files tests | rg 'wifi_manager'` and record the selected files.

- [ ] **Step 6: Commit driver reconciliation**

```bash
git add components/esp-wifi-connect/include/wifi_manager.h \
  components/esp-wifi-connect/wifi_manager.cc \
  tests/native_stubs/wifi_manager.h \
  tests/test_blufi_wifi_scan_contract.py
git commit -m "fix(wifi): reconcile provisioning scan driver"
```

### Task 5: Recover A Lost Callback With Driver Reset And Event-Loop Barrier

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Modify: `main/boards/common/blufi.h`
- Modify: `main/boards/common/blufi.cpp`
- Modify: `main/boards/common/blufi_wifi_scan_controller.h`
- Modify: `tests/native/blufi_wifi_scan_controller_host_test.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`
- Modify: `tests/test_blufi_provisioning_stability.py`

- [ ] **Step 1: Add failing tests for watchdog and FIFO barrier order**

Require these source-order invariants:

```python
def test_blufi_lost_scan_recovery_stops_driver_then_drains_event_loop_before_reinit():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::RecoverStuckWifiScan")
    begin = body.index("BeginRecovery(request_id)")
    recover = body.index("RecoverProvisioningScanDriver")
    complete = body.index("CompleteRecovery")
    assert begin < recover < complete

    manager = read("components/esp-wifi-connect/wifi_manager.cc")
    recovery = function_body(manager, "bool WifiManager::RecoverProvisioningScanDriver")
    assert recovery.index("esp_wifi_scan_stop") < recovery.index("esp_wifi_stop")
    assert recovery.index("esp_wifi_stop") < recovery.index("esp_wifi_deinit")
    assert recovery.index("drain_default_event_loop") < recovery.index("esp_wifi_init")


def test_blufi_scan_watchdog_runs_recovery_on_application_task():
    source = read("main/boards/common/blufi.cpp")
    callback = function_body(source, "void Blufi::_wifi_scan_watchdog_cb")
    assert "Application::GetInstance().Schedule" in callback
    assert "RecoverStuckWifiScan" in callback
    assert "esp_wifi_deinit" not in callback
```

- [ ] **Step 2: Run and verify RED**

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'lost_scan_recovery or scan_watchdog'
```

Expected: FAIL because watchdog and recovery APIs do not exist.

- [ ] **Step 3: Add a bounded WifiManager recovery API**

Declare:

```cpp
bool RecoverProvisioningScanDriver(
    const std::function<bool()>& drain_default_event_loop);
```

Implement under `WifiManager::mutex_` with this exact safety order:

```cpp
bool WifiManager::RecoverProvisioningScanDriver(
        const std::function<bool()>& drain_default_event_loop) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || station_active_ || config_mode_active_) {
        return false;
    }
    esp_wifi_scan_stop();
    const esp_err_t stop = esp_wifi_stop();
    if (stop != ESP_OK && stop != ESP_ERR_WIFI_NOT_INIT) {
        return false;
    }
    const esp_err_t deinit = esp_wifi_deinit();
    if (deinit != ESP_OK && deinit != ESP_ERR_WIFI_NOT_INIT) {
        return false;
    }
    const bool drained = drain_default_event_loop && drain_default_event_loop();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    const esp_err_t init = esp_wifi_init(&cfg);
    if (init != ESP_OK) {
        return false;
    }
    // Keep WifiManager internally consistent even when the barrier times out.
    // BluFi remains Draining and retries recovery; it does not start a scan.
    return drained;
}
```

Document that the callback executes while the WifiManager mutex is held and must not call WifiManager.

- [ ] **Step 4: Add the default-event-loop FIFO barrier**

In `blufi.cpp`, define a private event base and barrier event:

```cpp
ESP_EVENT_DEFINE_BASE(TBOT_BLUFI_SCAN_RECOVERY_EVENT);
constexpr int32_t kTbotBlufiScanBarrierEvent = 1;
```

Add a handler that gives a binary semaphore. Implement `DrainWifiScanEventLoop()` by registering the barrier handler, posting `TBOT_BLUFI_SCAN_RECOVERY_EVENT`, waiting no more than 1000 ms, then unregistering/deleting the semaphore. Because the default event loop is FIFO, the barrier runs after already-posted `WIFI_EVENT_SCAN_DONE` events.

Return false on allocation, register, post, wait, or unregister failure. Do not hold the scan controller mutex while waiting.

- [ ] **Step 5: Add the one-shot scan watchdog**

In `blufi.h` add:

```cpp
esp_timer_handle_t wifi_scan_watchdog_ = nullptr;
std::mutex wifi_scan_watchdog_mutex_;
uint64_t wifi_scan_watchdog_request_id_ = 0;
static constexpr int64_t kWifiScanWatchdogUs = 8LL * 1000 * 1000;

void ArmWifiScanWatchdog(uint64_t request_id);
void CancelWifiScanWatchdog(uint64_t request_id);
static void _wifi_scan_watchdog_cb(void* arg);
void RecoverStuckWifiScan(uint64_t request_id);
bool DrainWifiScanEventLoop();
```

Create the timer lazily and retain it across BluFi `deinit()` while a Wi-Fi callback is owed; BLE teardown must not disable recovery of a driver/event callback. Delete it only in the `Blufi` destructor. Arm only after `CommitStart(...).accepted`; cancel only when the same request begins completion, start fails, or recovery completes.

The timer callback must only snapshot the request ID and schedule `RecoverStuckWifiScan` on the Application task.

- [ ] **Step 6: Implement recovery and pending restart**

```cpp
void Blufi::RecoverStuckWifiScan(uint64_t request_id) {
    const auto ticket = wifi_scan_controller_.BeginRecovery(request_id);
    if (!ticket.valid || ticket.request_id != request_id) {
        return;
    }
    const bool recovered = WifiManager::GetInstance().RecoverProvisioningScanDriver(
        [this]() { return DrainWifiScanEventLoop(); });
    const auto result = wifi_scan_controller_.CompleteRecovery(ticket, recovered);
    if (!recovered) {
        ESP_LOGW(BLUFI_TAG, "WiFi scan recovery remains draining request=%llu",
                 static_cast<unsigned long long>(request_id));
        ArmWifiScanWatchdog(request_id);
        return;
    }
    if (result.start_pending) {
        SchedulePendingWifiScan(result.request_id, result.pending);
    }
}
```

After successful recovery, unregister and re-register the scan handler only if needed; the event-loop barrier must complete before a new driver incarnation or new physical scan starts.

- [ ] **Step 7: Extend native recovery choreography**

Use deterministic barriers to prove:

1. callback is missing;
2. recovery begins and blocks new physical scan;
3. driver reset completes;
4. barrier is marked drained;
5. driver incarnation advances;
6. exactly one pending request becomes startable.

- [ ] **Step 8: Run recovery-focused tests**

```bash
python3 -m pytest -q \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py
```

Expected: all pass.

- [ ] **Step 9: Commit lost-callback recovery**

```bash
git add components/esp-wifi-connect/include/wifi_manager.h \
  components/esp-wifi-connect/wifi_manager.cc \
  main/boards/common/blufi.h main/boards/common/blufi.cpp \
  main/boards/common/blufi_wifi_scan_controller.h \
  tests/native/blufi_wifi_scan_controller_host_test.cc \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py
git commit -m "fix(blufi): recover lost WiFi scan callbacks"
```

### Task 6: Review, Full Verification, Production Build, And Hardware E2E

**Files:**
- Verify all modified files.
- Update only tests/docs if a reviewer identifies a real gap.

- [ ] **Step 1: Run spec review for Tasks 1-5**

Request an independent review against:

```text
docs/superpowers/specs/2026-08-31-blufi-scan-ownership-recovery-design.md
```

Block on every Critical or Important finding. Require explicit review of callback loss, event-loop barrier proof, handler lifetime, lock ordering, station/config exclusion, and no cross-session response.

- [ ] **Step 2: Run code-quality review**

Require review of all scan controller transitions, external-call two-phase commits, watchdog lifecycle, WifiManager mutex callback contract, non-BluFi build guards, and native-test determinism. Fix Critical/Important issues test-first and re-review.

- [ ] **Step 3: Run focused and native suites**

```bash
python3 -m pytest -q \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_wifi_provisioning_brand.py \
  tests/test_blufi_advertising_ledger_native.py \
  tests/test_blufi_lifecycle_serialization_native.py \
  tests/test_blufi_deinit_transaction.py
```

Expected: zero failures.

- [ ] **Step 4: Run the full firmware suite**

```bash
python3 -m pytest -q tests
```

Expected: zero failures when `managed_components` is available. If the isolated worktree lacks managed JPEG/font assets, temporarily link the main worktree's `managed_components` only for verification, remove the link afterward, and record both the unlinked environmental failures and linked full pass.

- [ ] **Step 5: Build the production LCDWiki ESP32-S3 image**

Use ESP-IDF 5.5.x with the production `sdkconfig` from the firmware main worktree and the established LCDWiki defaults. The command must produce `xiaozhi.bin` and pass:

```bash
python3 scripts/assert_lcdwiki_prod_config.py <resolved-sdkconfig>
```

Do not claim build success if the configured ESP-IDF Python environment is unavailable. Repair or select a valid local Python 3.9/3.10 ESP-IDF environment first.

- [ ] **Step 6: Merge the reviewed firmware branch into firmware `main`**

After all review/test/build gates pass:

```bash
git -C /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware switch main
git -C /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware merge --no-ff fix/blufi-lifecycle-final \
  -m "merge: harden BluFi WiFi provisioning lifecycle"
```

Run focused tests again on `main` before flashing.

- [ ] **Step 7: Flash only the app partition and preserve NVS**

Flash the reviewed `xiaozhi.bin` only at offset `0x20000` to `/dev/cu.usbmodem1101`. Do not erase flash and do not write NVS, bootloader, partition table, OTA data, or assets.

- [ ] **Step 8: Install mobile `main` and run physical E2E**

With the Android phone unlocked and authorized in `adb devices -l`, run at least:

1. Scan and connect the robot to `SUMI_LAU1` using the user-provided password.
2. Disconnect/unpair from the phone; verify robot automatically returns to initialization/search mode without BOOT.
3. Reconnect three consecutive times.
4. Change Wi-Fi to `Van Phong Tam Dentist` using the user-provided password, then change back.
5. During one scan, disconnect/reconnect BLE so the old scan completion must drain; verify no stale AP list is delivered to the new session.
6. Trigger rapid repeated Wi-Fi-list requests; verify only one physical scan is active and the current request receives one response.

Capture robot serial evidence for scan request ID/incarnation, stale drain, AP-list dispatch, credentials received, station connection, IP acquisition, and setup completion. Do not print passwords.

- [ ] **Step 9: Clean worktrees only after E2E passes**

Remove the completed Wi-Fi worktree, prune stale worktree metadata, and verify `git worktree list` across all repositories. Preserve branches unless the user explicitly asks to delete them. Do not remove unrelated esp32-server worktrees.
