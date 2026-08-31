# Shared Wi-Fi Scan Recovery Executor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recover any retained global Wi-Fi scan lease without rebooting or erasing NVS, while preventing a stale `SCAN_DONE` callback from crossing into the next driver incarnation.

**Architecture:** Add one process-lifetime `WifiScanRecoveryExecutor` owned by `WifiManager`. Scanner-owned APIs atomically claim an exact recovery debt; the executor alone performs `scan_stop -> wifi_stop -> wifi_deinit -> FIFO barrier -> wifi_init`, mints the opaque proof, and the scanner atomically completes its exact lease. Failed recovery remains fail-closed and is retried on the Application task without starting another physical scan.

**Tech Stack:** ESP-IDF Wi-Fi/event APIs, FreeRTOS/ESP timers, C++17 mutexes and optionals, native Clang host tests with ASan/UBSan/TSan, pytest source contracts.

---

### Task 1: Build The Concrete Recovery Executor

**Files:**
- Create: `components/esp-wifi-connect/include/wifi_scan_recovery_executor.h`
- Create: `components/esp-wifi-connect/wifi_scan_recovery_executor.cc`
- Modify: `components/esp-wifi-connect/CMakeLists.txt`
- Create: `tests/native/wifi_scan_recovery_executor_host_test.cc`
- Create: `scripts/run_host_native_wifi_scan_recovery_executor_test.sh`
- Create: `tests/test_wifi_scan_recovery_executor_native.py`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] **Step 1: Write the failing executor choreography test**

Model an injected driver with recorded calls and require exactly:

```cpp
const auto proof = executor.Execute(recovery);
assert(driver.calls == std::vector<std::string>({
    "scan_stop", "wifi_stop", "wifi_deinit", "barrier", "wifi_init"}));
assert(proof.Proves(recovery.recovery_id()));
```

Add failure cases for every operation, barrier timeout, concurrent Execute calls,
and reinitialization failure. A failed execution must return an invalid proof and
must never permit a new physical scan.

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m pytest -q tests/test_wifi_scan_recovery_executor_native.py
```

Expected: FAIL because the executor and runner do not exist.

- [ ] **Step 3: Implement the only RecoveryProof factory**

Declare a non-copy-owned process-lifetime executor with a serialization mutex:

```cpp
class WifiScanRecoveryExecutor {
public:
    WifiScanLeaseCoordinator::RecoveryProof Execute(
        const WifiScanLeaseCoordinator::RecoveryDecision& recovery);

private:
    std::mutex mutex_;
};
```

Production execution must use the exact order:

```cpp
esp_wifi_scan_stop();
esp_wifi_stop();
esp_wifi_deinit();
const bool drained = DrainDefaultEventLoop(std::chrono::milliseconds(1000));
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
cfg.nvs_enable = false;
const bool ready = esp_wifi_init(&cfg) == ESP_OK;
```

Only this class may call the private `RecoveryProof` constructor. Treat
`ESP_ERR_WIFI_NOT_INIT` as an already-stopped/deinitialized state where the
ESP-IDF API documents it as safe. Barrier failure or init failure produces no
proof. Do not initialize NVS, netif, or the default event loop.

- [ ] **Step 4: Run GREEN and sanitizers**

Run:

```bash
python3 -m pytest -q tests/test_wifi_scan_recovery_executor_native.py \
  tests/test_wifi_scan_lease_coordinator_native.py
bash scripts/run_host_native_wifi_scan_recovery_executor_test.sh
git diff --check
```

Expected: all tests and supported sanitizer variants pass.

- [ ] **Step 5: Commit**

```bash
git add components/esp-wifi-connect/include/wifi_scan_recovery_executor.h \
  components/esp-wifi-connect/wifi_scan_recovery_executor.cc \
  components/esp-wifi-connect/CMakeLists.txt \
  tests/native/wifi_scan_recovery_executor_host_test.cc \
  scripts/run_host_native_wifi_scan_recovery_executor_test.sh \
  tests/test_wifi_scan_recovery_executor_native.py \
  tests/test_blufi_wifi_scan_contract.py
git commit -m "fix(wifi): add shared scan recovery executor"
```

### Task 2: Add Manager-Owned Recovery Scheduling

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Modify: `components/esp-wifi-connect/include/wifi_station.h`
- Modify: `components/esp-wifi-connect/wifi_station.cc`
- Modify: `components/esp-wifi-connect/include/wifi_configuration_ap.h`
- Modify: `components/esp-wifi-connect/wifi_configuration_ap.cc`
- Modify: `tests/native/wifi_scan_recovery_executor_host_test.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] **Step 1: Write failing owner-debt scheduling tests**

Require deterministic cases for Station and Config AP:

```text
exact debt published -> manager schedules once -> scanner claims exact lease
-> executor resets driver and drains FIFO -> scanner completes exact proof
-> coordinator becomes Free at next incarnation -> pending scan may retry
```

Also prove duplicate notifications coalesce, a callback that consumes the debt
before execution cancels recovery, failure retries without releasing ownership,
and manager lifecycle transitions remain blocked during recovery.

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'shared_scan_recovery or recovery_scheduler'
```

Expected: FAIL because manager scheduling does not exist.

- [ ] **Step 3: Expose scanner-owned recovery callbacks**

Add a recovery-needed callback to each scanner. Invoke it only after releasing
the scanner mutex and only for a retained exact debt. The manager callback must
schedule Application-task work; it must not run driver recovery on the ESP event
task or while a scanner/coordinator mutex is held.

- [ ] **Step 4: Implement one manager recovery state machine**

`WifiManager` owns the executor and a mutex-protected state containing owner,
generation, scheduled/running flags, and retry timer. The Application-task worker:

```cpp
auto claim = owner->ClaimScanRecovery();
if (!claim) return;
auto proof = scan_recovery_executor_.Execute(claim->recovery);
if (proof.Proves(claim->recovery.recovery_id())) {
    owner->CompleteScanRecovery(*claim, proof);
} else {
    ScheduleScanRecoveryRetry(owner, generation);
}
```

Before execution, atomically mark the manager lifecycle as recovery-in-progress.
Block `StartStation`, `StartConfigAp`, `StopRadio`, and competing recovery work.
After successful driver reinit and exact scanner completion, clear teardown fault
only when that fault was caused by the same recoverable scan teardown debt. Leave
unrelated Config connection-boundary faults fail-closed.

- [ ] **Step 5: Verify GREEN**

Run:

```bash
python3 -m pytest -q tests/test_blufi*.py tests/test_wifi*.py
bash scripts/run_host_native_wifi_scan_recovery_executor_test.sh
bash scripts/run_host_native_wifi_scan_lease_coordinator_test.sh
git diff --check
```

Expected: zero failures; normal and supported sanitizer variants pass.

- [ ] **Step 6: Commit**

```bash
git add components/esp-wifi-connect/include/wifi_manager.h \
  components/esp-wifi-connect/wifi_manager.cc \
  components/esp-wifi-connect/include/wifi_station.h \
  components/esp-wifi-connect/wifi_station.cc \
  components/esp-wifi-connect/include/wifi_configuration_ap.h \
  components/esp-wifi-connect/wifi_configuration_ap.cc \
  tests/native/wifi_scan_recovery_executor_host_test.cc \
  tests/test_blufi_wifi_scan_contract.py
git commit -m "fix(wifi): schedule shared scan recovery"
```

### Task 3: Review Recovery Before BluFi Integration

**Files:**
- Verify all files modified by Tasks 1-2.
- Modify tests/docs only for real reviewer findings.

- [ ] **Step 1: Run independent spec review**

Block on every Critical/Important finding. Verify exact owner/lease/incarnation,
driver reset order, barrier placement, proof minting, lifecycle exclusion,
retry behavior, process-lifetime ownership, and no NVS erase/global reinit.

- [ ] **Step 2: Run independent quality review**

Stress concurrent debt notification, callback-vs-recovery races, retry timer
lifetime, Application-task handoff, lock order, failure after deinit, and
incarnation exhaustion. Fix every Critical/Important finding test-first.

- [ ] **Step 3: Run combined verification**

Run:

```bash
python3 -m pytest -q tests/test_blufi*.py tests/test_wifi*.py
bash scripts/run_host_native_wifi_scan_recovery_executor_test.sh
bash scripts/run_host_native_wifi_scan_lease_coordinator_test.sh
git diff --check
```

Expected: zero failures and a clean worktree before Task 4 BluFi integration.
