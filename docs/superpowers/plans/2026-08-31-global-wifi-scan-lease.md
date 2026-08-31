# Global Wi-Fi Scan Lease Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every firmware Wi-Fi scanner exclusive, generation-bound ownership of the global ESP-IDF scan callback before BluFi callback recovery continues.

**Architecture:** Add an ESP-independent lease coordinator owned by `WifiManager`, plus one reusable default-event-loop FIFO barrier. Migrate Station, Config AP, BluFi, and the board blocking scan to the lease so an early callback can be latched safely and a cancelled scanner cannot leak a queued event into the next owner.

**Tech Stack:** C++17, ESP-IDF Wi-Fi/event loop, FreeRTOS semaphores, pytest source contracts, host-native deterministic concurrency tests.

---

## File Map

- Create `components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h`: ESP-independent physical scan ownership state machine.
- Create `components/esp-wifi-connect/default_event_loop_barrier.cc`: shared FIFO barrier implementation.
- Create `components/esp-wifi-connect/include/default_event_loop_barrier.h`: bounded barrier API.
- Modify `components/esp-wifi-connect/include/wifi_manager.h`: own and expose the coordinator without taking the manager mutex in callback paths.
- Modify `components/esp-wifi-connect/wifi_manager.cc`: initialize lease access and reuse the barrier in scan teardown/recovery.
- Modify `components/esp-wifi-connect/include/wifi_station.h` and `components/esp-wifi-connect/wifi_station.cc`: replace boolean ownership with a Station lease.
- Modify `components/esp-wifi-connect/include/wifi_configuration_ap.h` and `components/esp-wifi-connect/wifi_configuration_ap.cc`: lease periodic Config AP scans.
- Modify `main/boards/common/blufi_wifi_scan_controller.h`: latch a globally authenticated early completion until submission commit.
- Modify `main/boards/common/blufi.h` and `main/boards/common/blufi.cpp`: acquire/retain/release the BluFi physical lease and route all callback handling through it.
- Modify `main/boards/m5stack-cardputer-adv/wifi_config_ui.cc`: lease and drain the blocking scan.
- Create `tests/native/wifi_scan_lease_coordinator_host_test.cc`: deterministic lease/start/callback/drain/recovery model.
- Create `scripts/run_host_native_wifi_scan_lease_coordinator_test.sh`: warnings-as-errors native runner.
- Create `tests/test_wifi_scan_lease_coordinator_native.py`: pytest wrapper.
- Modify `tests/test_blufi_wifi_scan_contract.py`: source inventory and BluFi lease contracts.
- Modify `tests/test_blufi_provisioning_stability.py`: early callback and session-delivery contracts.
- Modify `tests/native_stubs/wifi_manager.h`: keep native consumers aligned.

### Task 1: Build The Host-Testable Global Lease Coordinator

**Files:**
- Create: `components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h`
- Create: `tests/native/wifi_scan_lease_coordinator_host_test.cc`
- Create: `scripts/run_host_native_wifi_scan_lease_coordinator_test.sh`
- Create: `tests/test_wifi_scan_lease_coordinator_native.py`

- [ ] **Step 1: Write the failing lease identity test**

Create a native test that requires this public model:

```cpp
using Coordinator = WifiScanLeaseCoordinator;

Coordinator coordinator;
auto station = coordinator.TryAcquire(Coordinator::Owner::kStation);
assert(station.acquired);
assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
assert(!coordinator.ObserveScanDone(
    Coordinator::Lease{Coordinator::Owner::kBlufi,
                       station.lease.lease_id,
                       station.lease.driver_incarnation}).consume_now);
```

- [ ] **Step 2: Add the native runner and verify RED**

Compile with C++17, pthreads, `-Wall -Wextra -Werror`, then run:

```bash
python3 -m pytest -q tests/test_wifi_scan_lease_coordinator_native.py
```

Expected: FAIL because `wifi_scan_lease_coordinator.h` does not exist.

- [ ] **Step 3: Implement the minimal coordinator states and identities**

Define:

```cpp
class WifiScanLeaseCoordinator {
public:
    enum class Owner : uint8_t { kStation, kConfigAp, kBlufi, kBlockingUi };
    enum class Phase : uint8_t {
        kFree, kStarting, kRunning, kCompleting, kDraining, kRecovering
    };

    struct Lease {
        Owner owner = Owner::kStation;
        uint64_t lease_id = 0;
        uint32_t driver_incarnation = 0;
    };

    struct AcquireDecision { bool acquired = false; Lease lease; };
    struct CallbackDecision {
        bool consume_now = false;
        bool deferred_until_commit = false;
    };
    struct CommitDecision {
        bool accepted = false;
        bool consume_latched = false;
        bool released = false;
        bool callback_won_error = false;
        bool drain_required = false;
    };

    class DrainDecision {
    public:
        bool armed() const;
        uint64_t drain_id() const;
    private:
        DrainDecision();
        DrainDecision(bool armed, uint64_t drain_id);
        friend class WifiScanLeaseCoordinator;
    };
    class DrainProof;
    class RecoveryDecision {
    public:
        bool begun() const;
        uint64_t recovery_id() const;
    private:
        RecoveryDecision();
        RecoveryDecision(bool begun, uint64_t recovery_id);
        friend class WifiScanLeaseCoordinator;
    };
    class RecoveryProof;

    AcquireDecision TryAcquire(Owner owner);
    CallbackDecision ObserveScanDone(const Lease& lease);
    CommitDecision CommitSubmission(const Lease& lease, bool accepted);
    bool FinishCompletion(const Lease& lease);
    bool BeginDrain(const Lease& lease);
    DrainDecision ArmDrainBarrier(const Lease& lease);
    bool CompleteDrain(const Lease& lease, const DrainProof& proof);
    RecoveryDecision BeginRecovery(const Lease& lease);
    bool CompleteRecovery(const Lease& lease, const RecoveryProof& proof);
};

class DefaultEventLoopScanDrainExecutor;
class WifiScanRecoveryExecutor;
```

Every method locks only the coordinator mutex. `TryAcquire` succeeds only from
`Free`; all other operations require exact owner, lease ID, and incarnation.
Drain/recovery proof constructors are private and friend only the concrete drain
and recovery executors. No generic callable may manufacture a proof. Native
tests define test-only versions of those exact friend classes; production code
defines them in Tasks 2 and the original recovery Task 5.

- [ ] **Step 4: Add deterministic early-callback and error races**

Add tests proving:

```cpp
void EarlyMatchingCallbackWaitsForSuccessfulCommit();
void CallbackRacingSynchronousErrorWinsExactlyOnce();
void SynchronousErrorWithoutCallbackRequiresBarrierBeforeRelease();
void ForeignOrStaleCallbackCannotClaimLease();
void BarrierFailureRetainsDrainingLease();
void RecoveryAdvancesIncarnationBeforeNextAcquire();
```

Use condition variables/barriers, never sleeps. Also prove `BeginDrain()` may
win while submission is still `Starting`, and a later commit cannot resurrect
`Running` or release before callback/barrier proof.

- [ ] **Step 5: Run GREEN and sanitizer variants**

Run the native test with Clang/GCC when available, ASan/UBSan, and TSan using the
same pattern as the BluFi controller runner. Expected: all variants print a
single `PASS` line and pytest reports `1 passed`.

- [ ] **Step 6: Commit**

```bash
git add components/esp-wifi-connect/include/wifi_scan_lease_coordinator.h \
  tests/native/wifi_scan_lease_coordinator_host_test.cc \
  scripts/run_host_native_wifi_scan_lease_coordinator_test.sh \
  tests/test_wifi_scan_lease_coordinator_native.py
git commit -m "test(wifi): model global scan lease"
```

### Task 2: Add One Reusable Default-Event-Loop FIFO Barrier

**Files:**
- Create: `components/esp-wifi-connect/include/default_event_loop_barrier.h`
- Create: `components/esp-wifi-connect/default_event_loop_barrier.cc`
- Modify: `components/esp-wifi-connect/CMakeLists.txt`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] **Step 1: Write failing source contracts**

Require a `DrainDefaultEventLoop(std::chrono::milliseconds timeout)` API and
assert registration, private event post, bounded semaphore wait, unregister,
and cleanup order. Assert that the helper contains no Wi-Fi driver reset and no
scanner-specific state.

- [ ] **Step 2: Run RED**

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'default_event_loop_barrier'
```

Expected: FAIL because the helper does not exist.

- [ ] **Step 3: Implement the bounded barrier**

Expose the raw bounded barrier operation:

```cpp
bool DrainDefaultEventLoop(std::chrono::milliseconds timeout);
```

Use a private `ESP_EVENT_DEFINE_BASE`, binary semaphore, instance registration,
`esp_event_post`, bounded `xSemaphoreTake`, unregister, and semaphore deletion.
Return false for every failed stage and never retain a handler or semaphore.
`DefaultEventLoopScanDrainExecutor` receives an armed drain ticket, calls
`esp_wifi_scan_stop()` after submission commit, runs this barrier, and returns
the opaque proof for that exact ticket. It must not accept a precomputed boolean
or caller-supplied callback.

- [ ] **Step 4: Run GREEN and commit**

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'default_event_loop_barrier'
git diff --check
git add components/esp-wifi-connect/include/default_event_loop_barrier.h \
  components/esp-wifi-connect/default_event_loop_barrier.cc \
  components/esp-wifi-connect/CMakeLists.txt tests/test_blufi_wifi_scan_contract.py
git commit -m "fix(wifi): add default event loop barrier"
```

### Task 3: Migrate WifiStation And Config AP To The Lease

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Modify: `components/esp-wifi-connect/include/wifi_station.h`
- Modify: `components/esp-wifi-connect/wifi_station.cc`
- Modify: `components/esp-wifi-connect/include/wifi_configuration_ap.h`
- Modify: `components/esp-wifi-connect/wifi_configuration_ap.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] **Step 1: Write failing Station/Config ownership contracts**

Require `WifiManager::ScanLeaseCoordinator()` to expose the process-lifetime
coordinator without taking `WifiManager::mutex_`. Assert every Station/Config
`esp_wifi_scan_start()` is preceded by `TryAcquire`, every `SCAN_DONE` is
preceded by `ObserveScanDone`, and Stop performs:

```text
cancel timer -> BeginDrain -> await submission commit -> ArmDrainBarrier
-> concrete executor scan_stop -> FIFO barrier -> CompleteDrain
```

- [ ] **Step 2: Run RED**

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'station_scan_lease or config_ap_scan_lease'
```

Expected: FAIL on the current boolean/direct-scan paths.

- [ ] **Step 3: Migrate Station**

Add a dedicated Station scan-lease mutex and replace `scan_in_progress_` with
`std::optional<WifiScanLeaseCoordinator::Lease>` protected by that mutex.
Acquire before submission. On start error,
commit failure and clear the optional only if released. On `SCAN_DONE`, first
observe the exact lease; ignore foreign events without reading AP records. Stop
prevents new timer scans, stops the scan, begins drain, waits outside locks, and
arms a drain ticket after submission commit, runs the concrete executor, and
completes drain only with the matching opaque proof. The executor repeats
`scan_stop` even if Stop already called it before commit.

- [ ] **Step 4: Migrate Config AP**

Route initial and timer scans through one `StartOwnedScan()` helper with the same
lease protocol. Its event handler claims the exact Config AP lease before AP
record access. Stop uses the same bounded drain order as Station.

- [ ] **Step 5: Add deterministic manager integration tests**

Prove a Station timer and Config AP timer cannot both reach physical submission,
and a barrier timeout keeps the second owner blocked. Use host stubs or an
ESP-independent integration model; do not rely only on source ordering.

- [ ] **Step 6: Run GREEN and commit**

```bash
python3 -m pytest -q tests/test_wifi_scan_lease_coordinator_native.py \
  tests/test_blufi_wifi_scan_contract.py
git diff --check
git add components/esp-wifi-connect/include/wifi_manager.h \
  components/esp-wifi-connect/wifi_manager.cc \
  components/esp-wifi-connect/include/wifi_station.h \
  components/esp-wifi-connect/wifi_station.cc \
  components/esp-wifi-connect/include/wifi_configuration_ap.h \
  components/esp-wifi-connect/wifi_configuration_ap.cc \
  tests/test_blufi_wifi_scan_contract.py
git commit -m "fix(wifi): serialize station and config scans"
```

### Task 4: Bind BluFi Logical Ownership To The Global Lease

**Files:**
- Modify: `main/boards/common/blufi_wifi_scan_controller.h`
- Modify: `main/boards/common/blufi.h`
- Modify: `main/boards/common/blufi.cpp`
- Modify: `tests/native/blufi_wifi_scan_controller_host_test.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`
- Modify: `tests/test_blufi_provisioning_stability.py`
- Modify: `tests/native_stubs/wifi_manager.h`

- [ ] **Step 1: Write the failing early-callback integration test**

Model this exact sequence:

```cpp
auto request = logical.RequestScan(CurrentRequest());
auto lease = physical.TryAcquire(Owner::kBlufi);
assert(logical.ClaimStart(request.request_id).claimed);
assert(physical.ObserveScanDone(lease.lease).deferred_until_commit);
auto physical_commit = physical.CommitSubmission(lease.lease, true);
assert(physical_commit.consume_latched);
auto completion = logical.BeginCompletion(CurrentTuple());
assert(completion.owned_callback);
```

Before implementation it must fail because the logical controller rejects all
`Starting` callbacks and BluFi stores no global lease.

- [ ] **Step 2: Add busy-lease backpressure tests**

Prove a BluFi GET arriving while Station owns/drains the lease keeps one current
logical request, sends no failure, and starts exactly once after lease release.
Prove disconnect/reconnect replaces it with the new exact lifecycle tuple.

- [ ] **Step 3: Implement Application-task lease acquisition**

Add helpers:

```cpp
void ScheduleOwnedWifiScanStart(uint64_t request_id,
                                BlufiWifiScanController::Request request);
void RetryOwnedWifiScanAfterLeaseBusy(uint64_t request_id,
                                      BlufiWifiScanController::Request request);
void ConsumeOwnedWifiScanCompletion(uint64_t request_id);
```

Acquire the global lease before `ClaimStart`. If busy, keep the logical request
unsubmitted and schedule a bounded Application-task retry tied to request ID and
exact lifecycle tuple. After acquiring, revalidate `ClaimStart`; if it fails,
release the unused lease without submitting.

- [ ] **Step 4: Authenticate callbacks before logical completion**

The event handler snapshots the stored BluFi lease and calls
`ObserveScanDone(lease)`. Foreign events return immediately without AP-list
access. `deferred_until_commit` only latches the event. `consume_now` calls one
shared completion helper.

After `esp_wifi_scan_start()` returns, call both physical and logical commit.
If physical commit returns `consume_latched`, invoke the same completion helper
once. A callback winning a reported start error suppresses `WIFI_SCAN_FAIL`.

- [ ] **Step 5: Finish physical ownership after driver AP-list ownership**

Call `FinishCompletion(lease)` only after AP records have been collected or
cleared and the logical controller has finished the matching request. Pending
logical work may be scheduled only after the physical lease is `Free`.

- [ ] **Step 6: Run GREEN and commit**

```bash
python3 -m pytest -q \
  tests/test_wifi_scan_lease_coordinator_native.py \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_blufi_deinit_transaction.py
git diff --check
git add main/boards/common/blufi_wifi_scan_controller.h \
  main/boards/common/blufi.h main/boards/common/blufi.cpp \
  tests/native/blufi_wifi_scan_controller_host_test.cc \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py tests/native_stubs/wifi_manager.h
git commit -m "fix(blufi): bind scans to global lease"
```

### Task 5: Migrate The Blocking Board Scan And Enforce Complete Inventory

**Files:**
- Modify: `main/boards/m5stack-cardputer-adv/wifi_config_ui.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] **Step 1: Write a failing direct-call inventory contract**

Use `rg` from pytest to enumerate every non-generated direct
`esp_wifi_scan_start()` call. Require each call site to appear inside a helper
that acquires an exact lease and reaches either callback completion, synchronous
failure release, or stop-plus-barrier drain.

- [ ] **Step 2: Run RED**

```bash
python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py \
  -k 'all_wifi_scan_start_calls_use_global_lease'
```

Expected: FAIL on the Cardputer blocking scan.

- [ ] **Step 3: Lease the blocking scan**

Acquire `Owner::kBlockingUi` before the blocking call. If busy, return a clear
UI-level scan failure without disturbing the current owner. After the blocking
call and AP-list retrieval/clear, call `BeginDrain`, arm a drain ticket after
submission commit, run the concrete drain executor, then call `CompleteDrain`
with the opaque proof. A barrier failure retains the lease and reports a bounded
diagnostic.

- [ ] **Step 4: Run all lease tests and commit**

```bash
python3 -m pytest -q \
  tests/test_wifi_scan_lease_coordinator_native.py \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_blufi_deinit_transaction.py
git diff --check
git add main/boards/m5stack-cardputer-adv/wifi_config_ui.cc \
  tests/test_blufi_wifi_scan_contract.py
git commit -m "fix(wifi): lease blocking scan callers"
```

### Task 6: Review The Lease And Resume Driver Recovery Tasks

**Files:**
- Verify all files modified by Tasks 1-5.
- Modify tests/docs only for real reviewer findings.

- [ ] **Step 1: Run independent spec review**

Review against
`docs/superpowers/specs/2026-08-31-global-wifi-scan-lease-design.md`. Block on
every Critical/Important finding, explicitly covering early callbacks, stale
events, all scanner call sites, drain failure, and lock order.

- [ ] **Step 2: Run independent code-quality review**

Review coordinator transitions, optional lease storage races, event-loop task
behavior, stop ordering, barrier cleanup, callback/error double completion, and
test determinism. Fix Critical/Important findings test-first and re-review.

- [ ] **Step 3: Run combined focused verification**

```bash
python3 -m pytest -q \
  tests/test_wifi_scan_lease_coordinator_native.py \
  tests/test_blufi_wifi_scan_controller_native.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_blufi_deinit_transaction.py \
  tests/test_wifi_provisioning_brand.py \
  tests/test_blufi_advertising_ledger_native.py \
  tests/test_blufi_lifecycle_serialization_native.py
```

Expected: zero failures.

- [ ] **Step 4: Resume the original recovery plan**

Continue with Task 4 and Task 5 in
`docs/superpowers/plans/2026-08-31-blufi-scan-ownership-recovery.md`, using the
global lease and shared FIFO barrier as prerequisites. Do not duplicate a
BluFi-private barrier or release a lease before recovery incarnation advances.

- [ ] **Step 5: Commit any review-only corrections**

Commit only real test-first corrections. If no corrections are needed, do not
create an empty commit.
