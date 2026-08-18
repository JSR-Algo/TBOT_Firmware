# BluFi Wi-Fi Scan Memory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make robot Wi-Fi discovery and the immediately following secure BluFi provisioning succeed in dense RF environments without a reset or manual SSID fallback.

**Architecture:** Use an explicit passive ESP-IDF scan, bound application-owned candidate records, and defer the BluFi list notification until after the Wi-Fi scan callback returns. Guard the deferred notification with the setup generation and active BLE state so stale scan completions cannot write into a replacement session.

**Tech Stack:** ESP-IDF 5.5.4, C++17 firmware, FreeRTOS/Application task scheduling, pytest source-contract regressions, Android ADB, USB ESP32-S3 flash and serial capture.

---

## File Map

- Modify `tests/test_blufi_wifi_scan_contract.py`: regression contracts for passive scanning, bounded candidates, deferred dispatch, and stale-session protection.
- Modify `main/boards/common/blufi.h`: declare the deferred list helper and bounded scan-candidate constant/state contract.
- Modify `main/boards/common/blufi.cpp`: configure passive scan, bound result retrieval, release scan ownership, and schedule generation-checked list dispatch.
- Create `docs/qa/ad-hoc/2026-08-18-blufi-wifi-scan-memory.md`: record RED/GREEN, build, flash, Android, serial, merge, and worktree-cleanup evidence.

### Task 1: Lock The Regression With Failing Tests

**Files:**
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] **Step 1: Add a passive-scan contract**

Add a test that extracts `Blufi::start_wifi_scan` and requires an explicit passive configuration in both scan-start branches:

```python
def test_blufi_wifi_scan_is_passive_to_preserve_internal_dma_heap():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "bool Blufi::start_wifi_scan")

    assert "wifi_scan_config_t scan_config" in body
    assert "scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE" in body
    assert "scan_config.scan_time.passive" in body
    assert "esp_wifi_scan_start(&scan_config, false)" in body
    assert "esp_wifi_scan_start(NULL, false)" not in body
```

- [ ] **Step 2: Add bounded-result and deferred-dispatch contracts**

Add tests requiring a candidate cap and forbidding inline notification from the Wi-Fi event callback:

```python
def test_blufi_wifi_scan_caps_application_owned_candidates():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_wifi_scan_event_handler")

    assert "kMaxBlufiWifiScanCandidates" in source
    assert "std::min<uint16_t>(ap_num, kMaxBlufiWifiScanCandidates)" in body
    assert body.index("std::min<uint16_t>") < body.index("m_ap_records.resize")


def test_blufi_wifi_list_dispatch_is_deferred_until_scan_callback_returns():
    source = read("main/boards/common/blufi.cpp")
    body = function_body(source, "void Blufi::_wifi_scan_event_handler")
    helper = function_body(source, "void Blufi::ScheduleWifiListSend")

    assert "ScheduleWifiListSend" in body
    assert "_send_wifi_list();" not in body
    assert "Application::GetInstance().Schedule" in helper
    assert "RunIfSetupGenerationCurrent" in helper
    assert "m_ble_is_connected" in helper
```

- [ ] **Step 3: Run the focused tests and verify RED**

Run:

```bash
pytest -q \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py -x
```

Expected: the new passive-scan test fails because the code still calls `esp_wifi_scan_start(NULL, false)`; the new deferred-dispatch test fails because `_send_wifi_list()` is still called inline.

- [ ] **Step 4: Commit the RED tests**

```bash
git add tests/test_blufi_wifi_scan_contract.py
git commit -m "test(blufi): reproduce scan memory starvation"
```

### Task 2: Implement Passive, Bounded, Deferred Scan Delivery

**Files:**
- Modify: `main/boards/common/blufi.h`
- Modify: `main/boards/common/blufi.cpp`

- [ ] **Step 1: Declare bounded scan and deferred dispatch**

Add the helper declaration beside `_send_wifi_list()` in `blufi.h`:

```cpp
void ScheduleWifiListSend(uint32_t expected_generation);
```

Add bounded constants beside `kMaxBlufiWifiListApRecords` in `blufi.cpp`:

```cpp
static constexpr uint16_t kMaxBlufiWifiScanCandidates = 8;
static constexpr uint32_t kBlufiPassiveScanTimeMs = 120;
```

- [ ] **Step 2: Replace active default scanning with explicit passive scanning**

At the start of `Blufi::start_wifi_scan()`, create one configuration reused by both mode branches:

```cpp
wifi_scan_config_t scan_config{};
scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
scan_config.show_hidden = false;
scan_config.scan_time.passive = kBlufiPassiveScanTimeMs;
```

Replace both calls with:

```cpp
err = esp_wifi_scan_start(&scan_config, false);
```

- [ ] **Step 3: Bound driver-result retrieval and release driver memory**

In `_wifi_scan_event_handler`, cap `ap_num` before resizing the vector:

```cpp
ap_num = std::min<uint16_t>(ap_num, kMaxBlufiWifiScanCandidates);
self->m_ap_records.resize(ap_num);
esp_err_t err = esp_wifi_scan_get_ap_records(&ap_num, self->m_ap_records.data());
```

After the retrieval attempt, call `esp_wifi_clear_ap_list()` before any deferred notification is scheduled. Preserve the current error handling and set `m_ap_records_updated_us` only after successful retrieval.

- [ ] **Step 4: Defer notification onto the Application task**

Implement the helper outside the event callback:

```cpp
void Blufi::ScheduleWifiListSend(uint32_t expected_generation) {
    Application::GetInstance().Schedule([this, expected_generation]() {
        RunIfSetupGenerationCurrent(expected_generation, [this]() {
            if (!m_ble_is_connected) {
                std::vector<wifi_ap_record_t>().swap(m_ap_records);
                m_ap_records_updated_us = 0;
                return;
            }
            _send_wifi_list();
        });
    });
}
```

In `_wifi_scan_event_handler`, reset scan ownership before scheduling:

```cpp
self->m_scan_in_progress = false;
if (self->m_send_list_after_scan) {
    self->m_send_list_after_scan = false;
    self->ScheduleWifiListSend(self->setup_generation_.load());
}
```

- [ ] **Step 5: Run focused tests and verify GREEN**

Run:

```bash
pytest -q \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_blufi_security_and_events.py \
  tests/test_provisioning_log_redaction.py
```

Expected: all selected tests pass with zero failures.

- [ ] **Step 6: Commit the firmware fix**

```bash
git add main/boards/common/blufi.cpp main/boards/common/blufi.h
git commit -m "fix(blufi): defer passive Wi-Fi scan delivery"
```

### Task 3: Run Automated Gates And Build The Branch

**Files:**
- Create: `docs/qa/ad-hoc/2026-08-18-blufi-wifi-scan-memory.md`

- [ ] **Step 1: Run the complete BluFi and Wi-Fi provisioning test group**

Run:

```bash
pytest -q \
  tests/test_blufi_*.py \
  tests/test_wifi_board_provisioning.py \
  tests/test_wifi_provisioning_brand.py \
  tests/test_provisioning_log_redaction.py \
  tests/test_provisioning_success_teardown_contract.py \
  tests/test_tbot_connect_runtime_fsm_contract.py
```

Expected: exit code `0`, no failed tests.

- [ ] **Step 2: Build the LCDWiki production firmware**

Run:

```bash
PATH="/usr/bin:$PATH" ./build-lcdwiki.sh --no-flash
```

Expected: board guard prints `LCDWiki ES3C35P board confirmed`, `idf.py build` exits `0`, and `build/xiaozhi.bin` is produced.

- [ ] **Step 3: Record branch evidence**

Create the QA record with exact commit SHAs, RED failures, GREEN pass counts, build exit status, binary size/SHA-256, and the hardware checklist. Do not claim hardware success yet.

- [ ] **Step 4: Commit branch verification evidence**

```bash
git add docs/qa/ad-hoc/2026-08-18-blufi-wifi-scan-memory.md
git commit -m "docs(qa): record BluFi scan branch verification"
```

### Task 4: Review And Merge Into Main

**Files:**
- Review all branch changes relative to `main`.

- [ ] **Step 1: Inspect branch scope and run diff checks**

```bash
git status --short
git diff --check main...HEAD
git log --oneline main..HEAD
git diff --stat main...HEAD
```

Expected: clean worktree, no whitespace errors, and only the spec, plan, focused tests, BluFi implementation, and QA record are changed.

- [ ] **Step 2: Review for regressions**

Check that scan errors clear `m_scan_in_progress` and `m_send_list_after_scan`, stale scheduled sends cannot notify a replacement BLE session, credentials remain redacted, and no config-wide PSRAM/LWIP changes are introduced.

- [ ] **Step 3: Merge locally into main**

```bash
git switch main
git merge --no-ff fix/blufi-wifi-scan-memory -m "merge fix/blufi-wifi-scan-memory"
```

Expected: merge succeeds without conflict.

- [ ] **Step 4: Re-run tests on merged main**

Run the Task 3 pytest command again. Expected: exit `0` with no failures.

- [ ] **Step 5: Build a fresh main artifact**

```bash
PATH="/usr/bin:$PATH" ./build-lcdwiki.sh --no-flash
shasum -a 256 build/xiaozhi.bin
```

Expected: fresh main build succeeds; record the merged main SHA and artifact hash before flashing.

### Task 5: Flash And Verify The Real Android Wi-Fi Flow

**Files:**
- Modify: `docs/qa/ad-hoc/2026-08-18-blufi-wifi-scan-memory.md`

- [ ] **Step 1: Verify the connected hardware target**

```bash
ls -l /dev/cu.usbmodem1101
/Users/manhhodinh/Library/Android/sdk/platform-tools/adb devices -l
```

Expected: the ESP32-S3 serial port and Android device are both present.

- [ ] **Step 2: Flash the exact merged-main artifact**

```bash
BUILD_DIR="$PWD/build" PORT=/dev/cu.usbmodem1101 scripts/flash_prod_new_robot.sh
```

Expected: chip-id probe succeeds and the command ends with `FLASH_OK /dev/cu.usbmodem1101`.

- [ ] **Step 3: Enter the existing account's `Doi Wi-Fi` flow**

Use ADB to open the TBOT app's device page and select `Doi Wi-Fi`. Put the robot into its fresh setup window without unlinking it. Clear Android logcat and capture robot serial before starting the scan.

- [ ] **Step 4: Verify automatic robot Wi-Fi scan**

Expected evidence:

- the Android screen lists nearby networks returned by the robot;
- serial shows `BLUFI get wifi list`, passive scan start/done, and list dispatch;
- serial contains zero matches for `heap_alloc_failed` and `BLE_INIT: Malloc failed`.

- [ ] **Step 5: Verify scan-to-secure-provisioning continuity**

Select the target network and complete provisioning without resetting the robot between scan and credential delivery. Expected serial evidence:

- `DH negotiation completed successfully`;
- `Recv STA SSID` and `Recv STA PASSWORD`;
- `connected to WiFi` and a successful connection report;
- no BluFi security timeout or GATT allocation failure.

Expected mobile evidence: the device page reports `Truc tuyen` with a Wi-Fi RSSI value.

- [ ] **Step 6: Record and commit hardware evidence on main**

Update the QA record with timestamps, merged main SHA, artifact hash, flash result, Android screen result, and redacted serial excerpts. Commit:

```bash
git add docs/qa/ad-hoc/2026-08-18-blufi-wifi-scan-memory.md
git commit -m "docs(qa): verify BluFi Wi-Fi scan on hardware"
```

### Task 6: Remove Auxiliary Worktrees Without Losing Branches

**Files:**
- No tracked file changes.

- [ ] **Step 1: Re-audit every auxiliary worktree**

```bash
git worktree list --porcelain
for wt in .worktrees/*; do
  git -C "$wt" status --porcelain=v1
done
```

Expected: every worktree scheduled for removal is clean. Stop if any new change appears.

- [ ] **Step 2: Record merged versus unmerged branch tips**

For each worktree, use `git merge-base --is-ancestor <worktree-head> main`. Preserve all branch refs, especially the currently unmerged `lesson-prod/t54-cinematic-frame-task`, `lesson-prod/t54-opus-stack-budget`, `lesson-prod/t54-sd-sync-audio-quiesce`, and `lesson-prod/t54-terminal-audio-quarantine` branches.

- [ ] **Step 3: Remove only the clean worktree checkouts**

Run `git worktree remove <exact-path>` once per clean auxiliary worktree. Do not pass `--force`; do not delete unmerged branch refs.

- [ ] **Step 4: Prune and verify final repository state**

```bash
git worktree prune
git worktree list
git status --short
git log -3 --oneline --decorate
```

Expected: only the primary firmware worktree remains, `main` is clean, the Wi-Fi fix and hardware-evidence commits are present, and unmerged branch refs still exist.

## Self-Review

- Spec coverage: passive scanning, bounded candidates, deferred delivery, stale-session protection, failure cleanup, test-first proof, build, merged-main flash, Android scan-to-provision verification, and safe worktree cleanup are each mapped to a task.
- Placeholder scan: no deferred implementation steps or unspecified error handling remain.
- Type consistency: `ScheduleWifiListSend(uint32_t expected_generation)`, `kMaxBlufiWifiScanCandidates`, and `kBlufiPassiveScanTimeMs` use the same names in tests, declarations, implementation, and verification steps.
