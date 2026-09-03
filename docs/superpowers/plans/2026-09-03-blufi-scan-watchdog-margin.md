# BluFi Scan Watchdog Margin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent the BluFi passive Wi-Fi scan watchdog from racing the normal five-second ESP-IDF scan completion while retaining bounded recovery for a genuinely stalled scan.

**Architecture:** Keep the existing scan lease, exact-tuple, generation, BLE epoch, and recovery state machines unchanged. Introduce one named eight-second watchdog duration in `blufi.cpp`, enforce that timing through a source contract, and extend the host-native timer test to prove that normal completion at five seconds disarms the pending watchdog before its later callback can request recovery.

**Tech Stack:** C++17, ESP-IDF 5.5, `esp_timer`, Python 3/pytest source contracts, host-native Clang tests, ESP32-S3 USB Serial/JTAG.

---

## File Map

- Modify `tests/test_blufi_wifi_scan_contract.py`: lock the named timeout, require an eight-second value greater than the observed five-second passive-scan boundary, and forbid the old literal at the timer arm site.
- Modify `tests/native/blufi_wifi_scan_lease_timer_host_test.cc`: model a five-second normal completion followed by a stale eight-second timer callback and prove no recovery signal escapes.
- Modify `main/boards/common/blufi.cpp`: define and use the named eight-second watchdog constant without changing ownership or recovery logic.
- Create `docs/qa/ad-hoc/2026-09-03-blufi-scan-watchdog-margin.md`: record credential-free automated, build, flash, and physical E2E evidence.

### Task 1: Add the RED watchdog timing contracts

**Files:**
- Modify: `tests/test_blufi_wifi_scan_contract.py`
- Modify: `tests/native/blufi_wifi_scan_lease_timer_host_test.cc`

- [ ] **Step 1: Add the failing Python source contract**

Add this test next to the existing BluFi watchdog contracts:

```python
def test_blufi_scan_watchdog_has_margin_after_passive_scan_boundary():
    source = read("main/boards/common/blufi.cpp")
    watchdog = function_body(source, "void Blufi::ScheduleOwnedWifiScanWatchdog")

    match = re.search(
        r"kWifiScanWatchdogTimeoutUs\s*=\s*(\d+)LL\s*\*\s*1000\s*\*\s*1000",
        source,
    )
    assert match is not None
    assert int(match.group(1)) > 5
    assert "wifi_scan_watchdog_timer_.Arm(exact, kWifiScanWatchdogTimeoutUs)" in watchdog
    assert "5LL * 1000 * 1000" not in watchdog
```

If `re` is not already imported at the top of the test module, add `import re` with the standard-library imports.

- [ ] **Step 2: Add the failing native boundary regression**

Add this test before the anonymous namespace closes and invoke it from `main()`:

```cpp
void PassiveScanCompletionBeforeWatchdogPreventsRecoverySignal() {
    FakeDriver driver;
    Sink sink;
    Timer timer({&driver, Create, Stop, StartOnce, NowUs, BeforeClaim},
                &sink, Signal);
    const auto exact = Tuple(7, 70, 4);

    assert(timer.Arm(exact, 8'000'000));
    driver.now_us.store(5'000'000);
    assert(timer.CurrentExactTuple().has_value());
    assert(sink.signal_count == 0);

    assert(timer.Disarm(exact));
    driver.now_us.store(8'000'000);
    driver.callback(driver.callback_arg);
    assert(sink.signal_count == 0);
}
```

Add the call:

```cpp
PassiveScanCompletionBeforeWatchdogPreventsRecoverySignal();
```

- [ ] **Step 3: Run the RED tests**

Run:

```bash
python3 -m pytest tests/test_blufi_wifi_scan_contract.py -k watchdog -q
scripts/run_host_native_blufi_wifi_scan_lease_timer_test.sh
```

Expected: the Python contract fails because `kWifiScanWatchdogTimeoutUs` does not exist and the arm site still uses `5LL * 1000 * 1000`; the native timer test passes because it independently documents the intended timer semantics.

- [ ] **Step 4: Commit the RED tests**

```bash
git add tests/test_blufi_wifi_scan_contract.py tests/native/blufi_wifi_scan_lease_timer_host_test.cc
git commit -m "test(blufi): reproduce scan watchdog boundary race"
```

### Task 2: Add the watchdog timing margin

**Files:**
- Modify: `main/boards/common/blufi.cpp`
- Test: `tests/test_blufi_wifi_scan_contract.py`
- Test: `tests/native/blufi_wifi_scan_lease_timer_host_test.cc`

- [ ] **Step 1: Define the named duration beside existing BluFi scan constants**

Add:

```cpp
static constexpr int64_t kWifiScanWatchdogTimeoutUs = 8LL * 1000 * 1000;
```

Place it after `kMaxBlufiWifiScanCandidates` so the scan timeout remains visible with the other file-local BluFi scan configuration.

- [ ] **Step 2: Use the named duration at the exact timer arm site**

Change `ScheduleOwnedWifiScanWatchdog` to:

```cpp
void Blufi::ScheduleOwnedWifiScanWatchdog(
        uint64_t request_id, const WifiScanLeaseCoordinator::Lease& lease) {
    const BlufiWifiScanLeaseTimer::ExactTuple exact{request_id, lease};
    if (!wifi_scan_watchdog_timer_.Arm(exact, kWifiScanWatchdogTimeoutUs)) {
        HandleWifiScanWatchdog(exact);
    }
}
```

Do not change `HandleWifiScanWatchdog`, lease ownership checks, generation checks, BLE connection epoch checks, or recovery routing.

- [ ] **Step 3: Run focused GREEN verification**

Run:

```bash
python3 -m pytest tests/test_blufi_wifi_scan_contract.py -q
scripts/run_host_native_blufi_wifi_scan_lease_timer_test.sh
scripts/run_host_native_blufi_wifi_scan_controller_test.sh
```

Expected: every command exits zero; the contract file and both native scan suites report no failures.

- [ ] **Step 4: Commit the implementation**

```bash
git add main/boards/common/blufi.cpp
git commit -m "fix(blufi): leave margin for passive scan completion"
```

### Task 3: Run repository and production build gates

**Files:**
- Verify: `tests/`
- Verify: `build/xiaozhi.bin`

- [ ] **Step 1: Run all focused BluFi and Wi-Fi tests**

```bash
python3 -m pytest -q tests/test_blufi*.py tests/test_wifi*.py
```

Expected: zero failures.

- [ ] **Step 2: Run the full firmware Python suite**

```bash
python3 -m pytest -q tests
```

Expected: zero failures, with only repository-declared skips.

- [ ] **Step 3: Build the LCDWiki ESP32-S3 production image**

```bash
PATH="/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH" ./build-lcdwiki.sh --no-flash
```

Expected: ESP-IDF reports `Project build complete` and produces `build/xiaozhi.bin`.

- [ ] **Step 4: Inspect scope and formatting**

```bash
git diff --check
git status --short
git log --oneline -5
shasum -a 256 build/xiaozhi.bin
```

Expected: no whitespace errors; only intentional QA evidence may remain uncommitted; the binary checksum is recorded without credentials.

### Task 4: Flash and verify the physical Android-to-robot scan flow

**Files:**
- Create: `docs/qa/ad-hoc/2026-09-03-blufi-scan-watchdog-margin.md`

- [ ] **Step 1: Stop any existing serial monitor**

Send Ctrl+] to the active ESP-IDF monitor session, or verify no process holds `/dev/cu.usbmodem101`:

```bash
lsof /dev/cu.usbmodem101
```

Expected: the serial port is free before flashing.

- [ ] **Step 2: Flash only the application partition**

```bash
/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python -m esptool \
  --chip esp32s3 \
  -p /dev/cu.usbmodem101 \
  -b 460800 \
  --before default_reset \
  --after hard_reset \
  write_flash \
  --flash_mode dio \
  --flash_size 16MB \
  --flash_freq 80m \
  0x20000 build/xiaozhi.bin
```

Expected: esptool verifies the application image and resets the robot; NVS and asset partitions remain intact.

- [ ] **Step 3: Capture robot serial output during Android discovery**

Open a 115200-baud serial monitor, let saved-Wi-Fi timeout enter BluFi automatically, connect Android to the advertised `TBOT-...` device, and request the Wi-Fi list without pressing BOOT.

Expected serial sequence:

```text
Starting owned WiFi scan
WiFi scan started
WiFi scan done
Found ... APs
Sending WiFi list
```

Reject the build if a normal scan repeats either of these messages:

```text
Ignoring WiFi scan done event not owned by BluFi
Requesting WiFi scan recovery
```

- [ ] **Step 4: Provision and exercise reconnect scenarios**

Using the Android UI, verify the selected SSID text exactly before submitting credentials. Exercise one valid provisioning flow, three robot disconnect/reconnect cycles, a switch to the second authorized Wi-Fi network, one invalid-password attempt followed by correction, and one BLE interruption followed by rediscovery. Never copy either Wi-Fi password into commands saved in shell history, test fixtures, logs, screenshots intended for commit, or the QA file.

Expected: the Wi-Fi list appears each time, the robot reaches its connected/initialized state on valid credentials, invalid credentials return to a recoverable provisioning state, and no BOOT press is required.

- [ ] **Step 5: Record credential-free QA evidence**

Create `docs/qa/ad-hoc/2026-09-03-blufi-scan-watchdog-margin.md` with:

```markdown
# BluFi Scan Watchdog Margin QA

## Automated gates

- Focused BluFi/Wi-Fi pytest: PASS (record count and duration).
- Native lease timer: PASS.
- Native scan controller: PASS.
- Full firmware pytest: PASS (record count, skips, and duration).
- LCDWiki ESP32-S3 production build: PASS (record binary SHA-256).

## Physical E2E

- Automatic BluFi entry without BOOT: PASS.
- Android receives robot Wi-Fi list: PASS.
- Valid provisioning: PASS.
- Robot disconnect/reconnect, three cycles: PASS.
- Wi-Fi network switch: PASS.
- Invalid password then correction: PASS.
- BLE interruption then rediscovery: PASS.
- No normal-scan ownership rejection/recovery loop: PASS.
- No memory-allocation failure: PASS.

No Wi-Fi credentials are stored in this evidence.
```

- [ ] **Step 6: Commit the evidence**

```bash
git add docs/qa/ad-hoc/2026-09-03-blufi-scan-watchdog-margin.md
git commit -m "docs(blufi): record scan watchdog verification"
```

### Task 5: Review, merge, reverify main, and clean completed worktrees

**Files:**
- Verify: firmware branch history and worktree list

- [ ] **Step 1: Run final verification before integration**

```bash
python3 -m pytest tests/test_blufi_wifi_scan_contract.py -q
scripts/run_host_native_blufi_wifi_scan_lease_timer_test.sh
scripts/run_host_native_blufi_wifi_scan_controller_test.sh
git diff --check
git status --short
```

Expected: all tests pass and the feature worktree is clean.

- [ ] **Step 2: Review the complete branch diff**

Compare the branch with firmware `main`, checking for correctness, behavioral regressions, credential leakage, and missing tests:

```bash
git diff --stat main...HEAD
git diff main...HEAD -- main/boards/common/blufi.cpp main/boards/common/blufi.h main/audio_service.cpp main/audio_service.h tests docs/qa docs/superpowers
```

Expected: only the approved audio-memory quiesce and scan-watchdog work is present; no password appears in tracked content.

- [ ] **Step 3: Merge into firmware main**

From the primary firmware worktree, use a non-interactive merge:

```bash
git merge --no-ff fix/blufi-audio-memory-quiesce
```

Expected: merge completes without conflicts.

- [ ] **Step 4: Re-run final software and physical gates on main**

Run the focused contracts, native tests, full pytest suite, production build, flash the `main` artifact, and repeat one complete Android-to-robot provisioning flow. Expected: the same automated and physical results as Tasks 3 and 4.

- [ ] **Step 5: Remove only completed Wi-Fi worktrees**

First inspect exact paths:

```bash
git worktree list
```

After confirming both branches are merged and clean, remove only:

```bash
git worktree remove /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/blufi-audio-memory-quiesce
git worktree remove /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/blufi-lifecycle-final
```

Expected: the completed BluFi worktrees disappear; `candidate-course-mode-539`, `course-mode-ed76-portable-lock`, and `google-live-evidence-journey` remain untouched.
