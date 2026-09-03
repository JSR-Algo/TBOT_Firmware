# BluFi Opmode Exact-Connect Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent BluFi credential submission from hanging behind an automatic saved-network station scan by making the exact-credential worker the sole owner of station startup during provisioning.

**Architecture:** Keep `ESP_BLUFI_EVENT_SET_WIFI_OPMODE` responsible for manager initialization and non-station mode handling, but make its STA/APSTA branches defer station startup. Preserve `StartStationConnectFromCredentials()` as the only BluFi path that claims staged credentials and calls `StartStationWithCredentialsIfScanIdle()`, with no new task, timer, or lifecycle state.

**Tech Stack:** C++17, ESP-IDF 5.5, Python 3/pytest source contracts, existing host-native Wi-Fi/BluFi test scripts, ESP32-S3 USB Serial/JTAG, Android ADB/BluFi UI.

---

## File Map

- Modify `tests/test_blufi_provisioning_stability.py`: add a source-level regression that isolates the opmode event and locks its STA/APSTA, AP, default, explicit-connect, and password-fallback responsibilities.
- Modify `main/boards/common/blufi.cpp`: stop STA/APSTA opmode events from calling `WifiManager::StartStation()` while preserving manager initialization and other mode behavior.
- Create `docs/qa/ad-hoc/2026-09-03-blufi-opmode-exact-connect.md`: record credential-free automated, build, flash, and physical E2E results.
- Verify `tests/`, `build/xiaozhi.bin`, branch history, firmware `main`, and only the completed BluFi worktrees.

### Task 1: Add the RED opmode ownership contract

**Files:**
- Modify: `tests/test_blufi_provisioning_stability.py`
- Test: `tests/test_blufi_provisioning_stability.py`

- [ ] **Step 1: Add an event-case extraction helper**

Add this helper beside `_req_connect_body()`:

```python
def _wifi_opmode_body() -> str:
    """The scoped block for ESP_BLUFI_EVENT_SET_WIFI_OPMODE."""
    blufi = read("main/boards/common/blufi.cpp")
    return _function_body(blufi, "case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:")
```

- [ ] **Step 2: Add the failing ownership regression**

Add this test beside the other extended BluFi event contracts:

```python
def test_blufi_opmode_defers_sta_start_to_exact_credential_owner():
    opmode = _wifi_opmode_body()
    connect = _req_connect_body()
    password = _function_body(
        read("main/boards/common/blufi.cpp"),
        "case ESP_BLUFI_EVENT_RECV_STA_PASSWD:",
    )

    assert "WifiManager::GetInstance()" in opmode
    assert "wifi_manager.Initialize()" in opmode

    sta = opmode[
        opmode.index("case WIFI_MODE_STA:") :
        opmode.index("case WIFI_MODE_AP:")
    ]
    ap = opmode[
        opmode.index("case WIFI_MODE_AP:") :
        opmode.index("case WIFI_MODE_APSTA:")
    ]
    apsta = opmode[
        opmode.index("case WIFI_MODE_APSTA:") :
        opmode.index("default:")
    ]
    fallback = opmode[opmode.index("default:") :]

    assert "StartStation()" not in sta
    assert "StartStation()" not in apsta
    assert "StartConfigAp()" in ap
    assert "StopStation()" in fallback
    assert "StopConfigAp()" in fallback

    assert 'StartStationConnectFromCredentials("blufi_connect_request")' in connect
    assert "ScheduleStationConnectFallback" in password
```

- [ ] **Step 3: Run the RED test**

Run:

```bash
python3 -m pytest tests/test_blufi_provisioning_stability.py::test_blufi_opmode_defers_sta_start_to_exact_credential_owner -q
```

Expected: FAIL because both `WIFI_MODE_STA` and `WIFI_MODE_APSTA` still contain `wifi_manager.StartStation()`.

- [ ] **Step 4: Commit the RED regression**

```bash
git add tests/test_blufi_provisioning_stability.py
git commit -m "test(blufi): reproduce opmode station start collision"
```

### Task 2: Defer STA/APSTA startup to the exact-credential worker

**Files:**
- Modify: `main/boards/common/blufi.cpp`
- Test: `tests/test_blufi_provisioning_stability.py`

- [ ] **Step 1: Make the minimal event-handler change**

Replace only the station branches inside `ESP_BLUFI_EVENT_SET_WIFI_OPMODE` with:

```cpp
switch (param->wifi_mode.op_mode) {
    case WIFI_MODE_STA:
        ESP_LOGI(BLUFI_TAG,
                 "Deferring station start until BluFi credentials are ready");
        break;
    case WIFI_MODE_AP:
        wifi_manager.StartConfigAp();
        break;
    case WIFI_MODE_APSTA:
        ESP_LOGW(BLUFI_TAG,
                 "APSTA mode not supported; deferring station start until "
                 "BluFi credentials are ready");
        break;
    default:
        wifi_manager.StopStation();
        wifi_manager.StopConfigAp();
        break;
}
```

Do not change `StartStationConnectFromCredentials()`,
`ScheduleStationConnectFallback()`, `ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP`, scan
lease handling, generation checks, connection epochs, or disconnect behavior.

- [ ] **Step 2: Run the focused GREEN test**

Run:

```bash
python3 -m pytest tests/test_blufi_provisioning_stability.py::test_blufi_opmode_defers_sta_start_to_exact_credential_owner -q
```

Expected: `1 passed`.

- [ ] **Step 3: Run the complete provisioning and scan contract files**

Run:

```bash
python3 -m pytest tests/test_blufi_provisioning_stability.py tests/test_blufi_wifi_scan_contract.py -q
```

Expected: all tests pass. In particular, existing staged-credential, duplicate-trigger, fallback, disconnect, scan lease, watchdog, BLE generation, and recovery contracts remain green.

- [ ] **Step 4: Commit the implementation**

```bash
git add main/boards/common/blufi.cpp
git commit -m "fix(blufi): defer station start to exact credentials"
```

### Task 3: Run automated release and production build gates

**Files:**
- Verify: `tests/`
- Verify: `build/xiaozhi.bin`

- [ ] **Step 1: Run focused BluFi and Wi-Fi Python suites**

```bash
python3 -m pytest -q tests/test_blufi*.py tests/test_wifi*.py
```

Expected: zero failures.

- [ ] **Step 2: Run native lifecycle/controller suites used by this path**

```bash
scripts/run_host_native_blufi_wifi_scan_lease_timer_test.sh
scripts/run_host_native_blufi_wifi_scan_controller_test.sh
scripts/run_host_native_wifi_manager_recovery_test.sh
```

If the Wi-Fi manager script has a different repository name, locate the exact committed runner with:

```bash
rg --files scripts | rg 'wifi.*manager.*recovery|manager.*wifi.*recovery'
```

Then run that single matching script. Expected: every native executable exits zero with no assertion failure.

- [ ] **Step 3: Run the full firmware Python suite**

```bash
python3 -m pytest -q tests
```

Expected: zero failures, with only repository-declared skips.

- [ ] **Step 4: Build the LCDWiki ESP32-S3 production image**

```bash
PATH="/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH" ./build-lcdwiki.sh --no-flash
```

Expected: ESP-IDF reports `Project build complete`, produces `build/xiaozhi.bin`, and the image still fits the configured application partition.

- [ ] **Step 5: Inspect scope, cleanliness, and artifact identity**

```bash
git diff --check
git status --short
git log --oneline -8
shasum -a 256 build/xiaozhi.bin
```

Expected: no whitespace errors; implementation commits are visible; only intentional QA evidence may remain uncommitted; the binary checksum contains no credentials.

### Task 4: Flash and prove the Android-to-robot flow physically

**Files:**
- Create: `docs/qa/ad-hoc/2026-09-03-blufi-opmode-exact-connect.md`

- [ ] **Step 1: Release the serial port**

If the prior monitor session is still active, send Ctrl+] to it. Then run:

```bash
lsof /dev/cu.usbmodem101
```

Expected: no process holds the robot serial port before flashing.

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

Expected: esptool verifies the image and hard-resets the robot without altering NVS or assets.

- [ ] **Step 3: Verify device identities and start credential-free logging**

```bash
/Users/manhhodinh/Library/Android/sdk/platform-tools/adb devices
```

Expected: Android device `efc5314f` is listed as `device`. Start a 115200-baud robot monitor and Android log capture that filters out application input fields and never records passwords.

- [ ] **Step 4: Verify automatic provisioning entry and Wi-Fi scan**

Allow unavailable saved Wi-Fi to trigger provisioning without pressing BOOT. From Android, connect to the robot BLE device and request the Wi-Fi list.

Expected robot sequence includes:

```text
WiFi connection timeout, entering config mode
BLUFI advertising started
Starting owned WiFi scan
WiFi scan done
Sending WiFi list
```

Expected Android result: the intended SSID is visible. Reject the build if a normal scan enters an ownership rejection/recovery loop or if memory allocation fails.

- [ ] **Step 5: Verify the formerly hanging valid-connect path**

Select the authorized test network and enter its password only in the Android password field. Do not place the password in terminal commands, shell history, screenshots for commit, logs, tests, or documentation.

Expected robot ordering:

```text
BLUFI Set WIFI opmode 1
Deferring station start until BluFi credentials are ready
Recv STA SSID
Recv STA PASSWORD
BLUFI request wifi connect to AP via esp-wifi-connect
Starting WiFi connect from BluFi credentials
Station stopped
```

The robot must then associate with the selected network, obtain an IP address, complete provisioning teardown, and reach its connected/initialized state. There must be no automatic saved-network scan between the opmode event and the exact-credential start.

- [ ] **Step 6: Exercise reconnect, switch, invalid-input, and interruption cases**

Using only the Android UI for credential entry, run:

1. Three robot disconnect/reconnect cycles against the same authorized network.
2. Three switches to the second authorized network, returning to provisioning between attempts without pressing BOOT.
3. One intentionally invalid password followed by correction.
4. One BLE disconnect during scanning followed by rediscovery and a successful connection.
5. One BLE disconnect after credentials are staged but before the connect control frame, followed by a clean new provisioning session.

Expected: valid attempts reach connected/initialized state; invalid credentials return to a recoverable setup state; stale candidates or callbacks never connect, mutate a newer session, or leave the UI hanging.

- [ ] **Step 7: Record credential-free QA evidence**

Create `docs/qa/ad-hoc/2026-09-03-blufi-opmode-exact-connect.md` with measured counts/durations and this structure:

```markdown
# BluFi Opmode Exact-Connect QA

## Automated gates

- Focused BluFi/Wi-Fi pytest: PASS (count and duration).
- Native scan timer/controller and Wi-Fi manager recovery: PASS.
- Full firmware pytest: PASS (count, skips, and duration).
- LCDWiki ESP32-S3 production build: PASS (binary size and SHA-256).

## Physical E2E

- Automatic BluFi entry without BOOT: PASS.
- Android receives robot Wi-Fi list: PASS.
- Opmode defers station startup: PASS.
- Exact-credential provisioning reaches connected/initialized state: PASS.
- Same-network disconnect/reconnect, three cycles: PASS.
- Authorized-network switching, three cycles: PASS.
- Invalid password then correction: PASS.
- BLE interruption during scan then rediscovery: PASS.
- BLE interruption before connect frame then clean retry: PASS.
- No hung station stop, stale-session mutation, scan ownership loop, or memory-allocation failure: PASS.

No Wi-Fi credentials are stored in this evidence.
```

- [ ] **Step 8: Commit the evidence**

```bash
git add docs/qa/ad-hoc/2026-09-03-blufi-opmode-exact-connect.md
git commit -m "docs(blufi): record exact connect verification"
```

### Task 5: Review, merge, reverify main, and clean completed worktrees

**Files:**
- Verify: firmware branch history, firmware `main`, and worktree list

- [ ] **Step 1: Invoke the code-review and completion verification gates**

Use `requesting-code-review` to review the complete branch for behavioral regressions, lifecycle races, credential leakage, and missing tests. Resolve all blocking findings. Then use `verification-before-completion` and run:

```bash
python3 -m pytest tests/test_blufi_provisioning_stability.py tests/test_blufi_wifi_scan_contract.py -q
git diff --check
git status --short
```

Expected: review has no unresolved blocking findings, all tests pass, and the feature worktree is clean.

- [ ] **Step 2: Confirm the complete intended branch delta**

```bash
git diff --stat main...HEAD
git diff main...HEAD -- main/boards/common/blufi.cpp main/boards/common/blufi.h main/audio_service.cpp main/audio_service.h components/esp-wifi-connect tests docs/qa docs/superpowers
```

Expected: the branch contains only the completed BluFi lifecycle, audio-memory, scan-watchdog, and exact-connect work plus their tests/docs. No Wi-Fi password or unrelated feature change is present.

- [ ] **Step 3: Merge into firmware main**

From `/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware`, run:

```bash
git status --short --branch
git merge --no-ff fix/blufi-audio-memory-quiesce
```

Expected: the primary worktree is on `main`, has no conflicting local changes, and the merge completes without conflicts. If unexpected user changes appear, stop without modifying them.

- [ ] **Step 4: Re-run software and build gates from main**

From `/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware`, run:

```bash
python3 -m pytest tests/test_blufi_provisioning_stability.py tests/test_blufi_wifi_scan_contract.py -q
python3 -m pytest -q tests
PATH="/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH" ./build-lcdwiki.sh --no-flash
```

Expected: focused and full tests pass, and the production image builds from the merged `main` history.

- [ ] **Step 5: Flash main and repeat one complete physical E2E**

Flash the `main` build using the app-only command from Task 4. Repeat automatic setup entry, Android scan, exact-credential connection, one disconnect, and one successful reconnect without pressing BOOT.

Expected: behavior matches the branch evidence and reaches connected/initialized state.

- [ ] **Step 6: Remove only completed BluFi worktrees**

First inspect exact paths and verify they are clean and merged:

```bash
git worktree list
git -C /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/blufi-audio-memory-quiesce status --short
git -C /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/blufi-lifecycle-final status --short
git branch --merged main
```

Only after the physical `main` E2E passes, remove:

```bash
git worktree remove /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/blufi-audio-memory-quiesce
git worktree remove /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/blufi-lifecycle-final
```

Expected: both completed BluFi worktrees disappear. Preserve `candidate-course-mode-539`, `course-mode-ed76-portable-lock`, and `google-live-evidence-journey` unchanged.
