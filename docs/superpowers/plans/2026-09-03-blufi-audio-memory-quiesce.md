# BluFi Audio Memory Quiesce Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Release the claimed robot's resident audio task stacks before BluFi initialization, then restore audio exactly once when the provisioning generation terminates.

**Architecture:** Add a small host-testable generation ledger that records whether a provisioning session stopped a running audio service. `AudioService::BeginWifiProvisioning()` cooperatively stops and waits for its three resident workers before returning a usable token; `EndWifiProvisioningAndRearm()` consumes that token and restarts only the service that the same generation stopped. Existing BluFi and application rollback paths remain the transaction coordinator.

**Tech Stack:** C++17, ESP-IDF 5.5.4, FreeRTOS tasks/event groups, host-native C++ tests, pytest source contracts, esptool, Android ADB/UIAutomator.

---

## File Map

- Create `main/audio/provisioning_audio_worker_state.h`: thread-safe generation ledger for exact-once audio restart ownership.
- Modify `main/audio/audio_service.h`: add worker-stop timeout, atomic running state, lifecycle ledger, and bounded worker-drain helper.
- Modify `main/audio/audio_service.cc`: stop/wait before BluFi and restart only after current-token rearm.
- Modify `tests/native/wake_word_lifecycle_gate_test.cc`: native tests for stopped/running, stale-token, and duplicate-token behavior.
- Modify `tests/test_wake_word_lifecycle_contract.py`: source contracts for stop/wait/init order and fail-closed timeout behavior.
- Modify `tests/test_tbot_claim_runtime_contract.py`: update the running-state assertion for the atomic start gate.
- Modify `scripts/run_host_native_wake_word_lifecycle_test.sh`: compile the new header through the existing native gate.
- Create `docs/qa/ad-hoc/2026-09-03-blufi-audio-memory-quiesce.md`: automated and physical evidence log.

### Task 1: Lock Exact-Once Restart Ownership With A Native RED Test

**Files:**
- Create: `main/audio/provisioning_audio_worker_state.h`
- Modify: `tests/native/wake_word_lifecycle_gate_test.cc`

- [ ] **Step 1: Add the failing lifecycle cases**

Include the not-yet-created controller in `tests/native/wake_word_lifecycle_gate_test.cc`:

```cpp
#include "audio/provisioning_audio_worker_state.h"
```

Append these cases before `return 0;`:

```cpp
ProvisioningAudioWorkerState audio_workers;
Require(audio_workers.Bind(41, true),
        "running audio binds restart ownership to the provisioning generation");
Require(!audio_workers.Bind(42, true),
        "a second generation cannot replace active restart ownership");

const auto stale_audio = audio_workers.Consume(40);
Require(!stale_audio.accepted && !stale_audio.restart_required,
        "stale completion cannot restart audio");

const auto current_audio = audio_workers.Consume(41);
Require(current_audio.accepted && current_audio.restart_required,
        "current completion restarts audio once");

const auto duplicate_audio = audio_workers.Consume(41);
Require(!duplicate_audio.accepted && !duplicate_audio.restart_required,
        "duplicate completion cannot restart audio twice");

Require(audio_workers.Bind(43, false),
        "previously stopped audio still binds the generation");
const auto stopped_audio = audio_workers.Consume(43);
Require(stopped_audio.accepted && !stopped_audio.restart_required,
        "previously stopped audio remains stopped after provisioning");
```

- [ ] **Step 2: Run the native gate and verify RED**

Run:

```bash
bash scripts/run_host_native_wake_word_lifecycle_test.sh
```

Expected: compilation fails because `audio/provisioning_audio_worker_state.h` does not exist.

- [ ] **Step 3: Add the minimal generation ledger**

Create `main/audio/provisioning_audio_worker_state.h`:

```cpp
#pragma once

#include <cstdint>
#include <mutex>

class ProvisioningAudioWorkerState {
public:
    struct Completion {
        bool accepted = false;
        bool restart_required = false;
    };

    bool Bind(uint64_t generation, bool restart_required) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0 || active_generation_ != 0) {
            return false;
        }
        active_generation_ = generation;
        restart_required_ = restart_required;
        return true;
    }

    Completion Consume(uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0 || active_generation_ != generation) {
            return {};
        }
        const bool restart_required = restart_required_;
        active_generation_ = 0;
        restart_required_ = false;
        return {true, restart_required};
    }

private:
    std::mutex mutex_;
    uint64_t active_generation_ = 0;
    bool restart_required_ = false;
};
```

- [ ] **Step 4: Run the native gate and verify GREEN**

Run:

```bash
bash scripts/run_host_native_wake_word_lifecycle_test.sh
```

Expected: all wake-word lifecycle executables exit `0`.

- [ ] **Step 5: Commit the lifecycle ledger**

```bash
git add main/audio/provisioning_audio_worker_state.h \
  tests/native/wake_word_lifecycle_gate_test.cc
git commit -m "test(audio): define provisioning worker ownership"
```

### Task 2: Reproduce The Missing Worker Drain At The BluFi Boundary

**Files:**
- Modify: `tests/test_wake_word_lifecycle_contract.py`

- [ ] **Step 1: Add the stop-before-init source contract**

Add:

```python
def test_wifi_provisioning_drains_resident_audio_workers_before_blufi_init():
    audio_h = read("main/audio/audio_service.h")
    audio_cc = read("main/audio/audio_service.cc")
    wifi = read("main/boards/common/wifi_board.cc")

    begin = audio_cc[
        audio_cc.index("AudioService::WifiProvisioningBeginResult AudioService::BeginWifiProvisioning"):
        audio_cc.index("bool AudioService::EndWifiProvisioningAndRearm")
    ]
    assert "const bool restart_audio = IsRunning();" in begin
    assert begin.index("provisioning_audio_workers_.Bind") < begin.index("Stop();")
    assert begin.index("Stop();") < begin.index("WaitForServiceWorkersStopped")
    assert "if (!WaitForServiceWorkersStopped(kProvisioningWorkerStopTimeoutMs))" in begin
    assert "return {{}, false};" in begin

    assert "std::atomic<bool> service_stopped_{true};" in audio_h
    assert "bool WaitForServiceWorkersStopped(uint32_t timeout_ms);" in audio_h

    entry = wifi[
        wifi.index("void WifiBoard::StartWifiConfigMode("):
        wifi.index("void WifiBoard::EnterWifiConfigMode()")
    ]
    assert entry.index("BeginWifiProvisioning()") < entry.index("blufi.RestartForSetup()")
```

- [ ] **Step 2: Add the exact-once rearm source contract**

Add:

```python
def test_wifi_provisioning_restarts_only_workers_owned_by_current_token():
    source = read("main/audio/audio_service.cc")
    end = source[source.index("bool AudioService::EndWifiProvisioningAndRearm"):]
    end = end[:end.index("void AudioService::EnableVoiceProcessing")]

    lifecycle = end.index("wake_word_lifecycle_.EndProvisioningAndRearm(token)")
    consume = end.index("provisioning_audio_workers_.Consume(token.generation)")
    restart = end.index("Start();")
    assert lifecycle < consume < restart
    assert "if (!completion.accepted)" in end
    assert "if (completion.restart_required)" in end
```

- [ ] **Step 3: Run the focused pytest and verify RED**

Run:

```bash
python3 -m pytest -q \
  tests/test_wake_word_lifecycle_contract.py \
  tests/test_provisioning_success_teardown_contract.py -x
```

Expected: the new worker-drain test fails because `BeginWifiProvisioning()` does not call `Stop()` or wait for resident task handles.

- [ ] **Step 4: Commit the RED contracts**

```bash
git add tests/test_wake_word_lifecycle_contract.py
git commit -m "test(blufi): reproduce resident audio heap starvation"
```

### Task 3: Stop And Drain Audio Workers Before Returning The Provisioning Token

**Files:**
- Modify: `main/audio/audio_service.h`
- Modify: `main/audio/audio_service.cc`
- Modify: `tests/test_tbot_claim_runtime_contract.py`

- [ ] **Step 1: Declare the lifecycle state and drain helper**

In `main/audio/audio_service.h`, include the new controller and add:

```cpp
#include "audio/provisioning_audio_worker_state.h"

static constexpr uint32_t kProvisioningWorkerStopTimeoutMs = 5000;
```

Replace the running flag and add the state/helper beside the task handles:

```cpp
std::atomic<bool> service_stopped_{true};
ProvisioningAudioWorkerState provisioning_audio_workers_;
bool WaitForServiceWorkersStopped(uint32_t timeout_ms);
```

- [ ] **Step 2: Make running-state accesses atomic**

Use acquire/release operations at lifecycle boundaries:

```cpp
bool IsRunning() const {
    return !service_stopped_.load(std::memory_order_acquire);
}
```

In `Start()` use:

```cpp
bool expected = true;
if (!service_stopped_.compare_exchange_strong(
        expected, false, std::memory_order_acq_rel)) {
    ESP_LOGW(TAG, "Audio service already running; ignoring duplicate start");
    return;
}
```

In `Stop()` use:

```cpp
service_stopped_.store(true, std::memory_order_release);
```

Existing task-loop boolean checks then read the atomic flag safely. In
`tests/test_tbot_claim_runtime_contract.py`, replace the old plain-bool source
assertions with:

```python
assert "service_stopped_.compare_exchange_strong(" in start_body
assert "std::memory_order_acq_rel" in start_body
```

- [ ] **Step 3: Implement bounded cooperative drain**

Add after `AudioService::Stop()`:

```cpp
bool AudioService::WaitForServiceWorkersStopped(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(task_handle_mutex_);
            if (audio_input_task_handle_ == nullptr &&
                audio_output_task_handle_ == nullptr &&
                opus_codec_task_handle_ == nullptr) {
                return true;
            }
        }
        if (xTaskGetTickCount() - start >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

- [ ] **Step 4: Bind ownership, stop, and wait inside provisioning begin**

Immediately after `BeginProvisioningAndQuiesce()` returns its token, add:

```cpp
const bool restart_audio = IsRunning();
if (!provisioning_audio_workers_.Bind(
        provisioning_token.generation, restart_audio)) {
    ESP_LOGE(TAG, "Audio provisioning worker ownership is already active");
    return {{}, false};
}
if (restart_audio) {
    Stop();
    if (!WaitForServiceWorkersStopped(kProvisioningWorkerStopTimeoutMs)) {
        ESP_LOGE(TAG, "Audio worker shutdown timed out; provisioning remains fail-closed");
        return {{}, false};
    }
    ESP_LOGI(TAG, "Audio worker stacks released for WiFi config");
}
```

Keep wake-word destruction and `FinishProvisioningReset()` after the worker drain. This guarantees `WifiBoard` cannot call `Blufi::RestartForSetup()` while any resident worker stack remains allocated.

- [ ] **Step 5: Run focused tests and verify the drain contract is GREEN**

Run:

```bash
python3 -m pytest -q \
  tests/test_wake_word_lifecycle_contract.py \
  tests/test_audio_stack_metrics_contract.py \
  tests/test_tbot_claim_runtime_contract.py
bash scripts/run_host_native_wake_word_lifecycle_test.sh
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit worker drain**

```bash
git add main/audio/audio_service.h main/audio/audio_service.cc \
  tests/test_tbot_claim_runtime_contract.py
git commit -m "fix(audio): drain workers before BluFi setup"
```

### Task 4: Restore Audio Exactly Once On Every Existing Terminal Path

**Files:**
- Modify: `main/audio/audio_service.cc`
- Modify: `tests/test_wake_word_lifecycle_contract.py`

- [ ] **Step 1: Implement token-owned restart**

Replace `EndWifiProvisioningAndRearm()` with:

```cpp
bool AudioService::EndWifiProvisioningAndRearm(WifiProvisioningToken token) {
    if (!wake_word_lifecycle_.EndProvisioningAndRearm(token)) {
        return false;
    }
    const auto completion =
        provisioning_audio_workers_.Consume(token.generation);
    if (!completion.accepted) {
        ESP_LOGE(TAG, "Audio provisioning worker completion token was not owned");
        return false;
    }
    if (completion.restart_required) {
        Start();
    }
    return true;
}
```

The existing callers already cover reservation-commit failure, BluFi init failure, setup abort/timeout, and successful BLE teardown. Do not add a second restart call to those callers.

- [ ] **Step 2: Run lifecycle and terminal-path tests**

Run:

```bash
python3 -m pytest -q \
  tests/test_wake_word_lifecycle_contract.py \
  tests/test_provisioning_success_teardown_contract.py \
  tests/test_wifi_board_provisioning.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_tbot_repair_pairing_contract.py
bash scripts/run_host_native_wake_word_lifecycle_test.sh
```

Expected: all selected tests pass with zero failures.

- [ ] **Step 3: Commit exact-once restoration**

```bash
git add main/audio/audio_service.cc tests/test_wake_word_lifecycle_contract.py
git commit -m "fix(audio): restore workers after provisioning"
```

### Task 5: Run Automated Release Gates And Build The LCDWiki Image

**Files:**
- Create: `docs/qa/ad-hoc/2026-09-03-blufi-audio-memory-quiesce.md`

- [ ] **Step 1: Run the focused Wi-Fi/BluFi/audio gate**

Run:

```bash
python3 -m pytest -q \
  tests/test_wake_word_lifecycle_contract.py \
  tests/test_provisioning_success_teardown_contract.py \
  tests/test_wifi_board_provisioning.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_blufi_security_and_events.py \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_wifi_manager_recovery_native.py \
  tests/test_provisioning_log_redaction.py \
  tests/test_ssid_manager_contract.py \
  tests/test_tbot_remote_unpair_contract.py \
  tests/test_tbot_repair_pairing_contract.py
bash scripts/run_host_native_wake_word_lifecycle_test.sh
```

Expected: all selected tests and native executables pass.

- [ ] **Step 2: Run the full firmware suite**

Run:

```bash
python3 -m pytest -q tests
```

Expected: `1520 passed, 1 skipped`, with zero failures.

- [ ] **Step 3: Build the exact production board image**

Populate the ignored component directory from the already verified main
checkout, preserving any worktree-local entries:

```bash
mkdir -p managed_components
for component in /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/managed_components/*; do
  name="$(basename "$component")"
  if [ ! -e "managed_components/$name" ]; then
    ln -s "$component" "managed_components/$name"
  fi
done
```

Then run:

```bash
./build-lcdwiki.sh --no-flash
```

Expected: `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`, successful ESP32-S3 link, `xiaozhi.bin` within the app partition, and no build errors.

- [ ] **Step 4: Record automated evidence**

Create `docs/qa/ad-hoc/2026-09-03-blufi-audio-memory-quiesce.md` with the RED failure, focused/full test counts, image byte size, SHA-256, partition margin, and exact commit under test.

- [ ] **Step 5: Commit the evidence**

```bash
git add docs/qa/ad-hoc/2026-09-03-blufi-audio-memory-quiesce.md
git commit -m "docs(blufi): record audio memory verification"
```

### Task 6: Flash And Prove Physical Android-Robot E2E

**Files:**
- Modify: `docs/qa/ad-hoc/2026-09-03-blufi-audio-memory-quiesce.md`

- [ ] **Step 1: Verify attached hardware identities**

Run:

```bash
/Users/manhhodinh/Library/Android/sdk/platform-tools/adb devices -l
ls /dev/cu.usbmodem*
```

Expected: Android `efc5314f` and robot `/dev/cu.usbmodem101` are visible. Stop if either identity is absent.

- [ ] **Step 2: Flash application-only firmware**

Run from the ESP-IDF environment:

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x20000 build/xiaozhi.bin
```

Expected: all bytes written and `Hash of data verified`; bootloader, partitions, NVS, OTA data, and assets remain untouched.

- [ ] **Step 3: Prove automatic discoverability without BOOT**

Capture serial while saved Wi-Fi is unavailable. Expected order:

```text
WifiBoard: WiFi connection timeout, entering config mode
AudioService: Audio worker stacks released for WiFi config
BLUFI_CLASS: BLUFI init finish
BLUFI_CLASS: BLUFI advertising started
```

Require materially increased internal/DMA free heap and no `heap_alloc_failed`, `Memory Full`, or partial advertising-data write. Use Android BLE scan/UI automation to confirm a `TBOT-*` device appears.

- [ ] **Step 4: Run three disconnect/reconnect cycles on `SUMI_LAU1`**

For each cycle, use Android to pair, enter password `hongvantruong`, wait for robot/app success, locally unpair/disconnect, then pair again without reboot or BOOT. Record serial and logcat evidence that every cycle advertises, connects, exits `wifi_configuring`, and restores audio once.

- [ ] **Step 5: Run Wi-Fi switch and failure recovery cases**

Switch to `Van Phong Tam Dentist` with password `66668888`, return to `SUMI_LAU1` with password `hongvantruong`, try one invalid password, and interrupt BLE once during setup. Require the robot to remain or return to discoverable setup and recover with the valid password without a reboot.

- [ ] **Step 6: Update and commit physical evidence**

Add timestamps, SSID-only results (never passwords), heap snapshots, three-cycle outcomes, Wi-Fi-switch outcomes, and log file paths to the QA document.

```bash
git add docs/qa/ad-hoc/2026-09-03-blufi-audio-memory-quiesce.md
git commit -m "test(blufi): record physical reconnect evidence"
```

### Task 7: Review, Merge To Main, Reverify, And Clean Up

**Files:**
- Modify only if review finds an in-scope defect.

- [ ] **Step 1: Inspect the complete branch diff**

Run:

```bash
git status --short
git diff main...HEAD --check
git diff --stat main...HEAD
```

Expected: clean worktree and only the files listed in this plan.

- [ ] **Step 2: Run verification-before-completion**

Re-run the focused gate, full pytest suite, and production build against the exact branch tip. Confirm the physical evidence references the same image SHA-256.

- [ ] **Step 3: Merge without discarding concurrent user work**

Verify the main checkout is clean and has not moved unexpectedly, then merge `fix/blufi-audio-memory-quiesce` into `main` with a non-fast-forward merge. Stop on conflicts or unexpected changes.

- [ ] **Step 4: Re-run the focused gate on merged main**

Run the Task 5 focused command from the main checkout. Expected: zero failures.

- [ ] **Step 5: Remove completed Wi-Fi worktrees only after all gates pass**

Remove `.worktrees/blufi-audio-memory-quiesce` and the previously completed `.worktrees/blufi-lifecycle-final`, then delete their local feature branches. Preserve `candidate-course-mode-539`, `course-mode-ed76-portable-lock`, and `google-live-evidence-journey`.

- [ ] **Step 6: Report exact final state**

Report merged commit IDs, automated counts, firmware SHA-256, physical E2E cycle results, remaining worktrees, and any residual risk. Do not claim that Wi-Fi can literally never fail; claim only the verified scenarios and production gates.
