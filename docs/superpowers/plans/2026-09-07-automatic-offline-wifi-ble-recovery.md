# Automatic Offline Wi-Fi BLE Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Guarantee that an owned robot with unavailable saved Wi-Fi automatically advertises BluFi after one fixed 60-second offline window and can be found through the mobile **Doi Wi-Fi** flow without pressing BOOT.

**Architecture:** Centralize the station-offline recovery timer in `WifiBoard::EnsureWifiRecoveryTimeout()`. Arm it before every saved-network station start outcome, retain an existing timer across repeated failures, cancel it on connection, and let the existing conditional Wi-Fi-config transaction perform the race-safe switch to BluFi. Mobile already has the required two 40-second reconnect scans, so this plan verifies that contract instead of adding a second recovery mechanism.

**Tech Stack:** ESP-IDF C++17, ESP timer, existing BluFi provisioning transaction, Python contract tests, React Native/TypeScript/Jest, Android ADB, ESP serial HIL

---

## File Map

- Modify `main/boards/common/wifi_board.h`: declare the single helper that owns automatic Wi-Fi recovery deadline arming.
- Modify `main/boards/common/wifi_board.cc`: arm a non-sliding deadline for every saved-network offline start outcome, reuse it for disconnect events, and guard stale expiry.
- Modify `tests/test_wifi_board_provisioning.py`: add source-level regression coverage for startup busy/already-active states, non-sliding behavior, cancellation, and timeout races.
- Verify `tests/features/device/pair-search-multi-device.test.tsx` in `tbot-mobile`: prove offline **Doi Wi-Fi** discovery still spans 80 seconds.
- Use the attached Android phone and `/dev/cu.usbmodem1101`: flash only the app image and collect physical end-to-end evidence.

### Task 1: Reproduce the missing startup recovery deadline

**Files:**
- Modify: `tests/test_wifi_board_provisioning.py`
- Test: `tests/test_wifi_board_provisioning.py`

- [ ] **Step 1: Add failing startup-outcome contract tests**

Add these tests after `test_wb12_try_wifi_connect_branches_on_stored_ssids`:

```python
def test_wb12a_saved_wifi_arms_recovery_before_station_start_outcome():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _func_body(
        wifi_board,
        "WifiStationStartResult WifiBoard::TryWifiConnect()",
        "void WifiBoard::OnNetworkEvent(",
    )

    have_idx = body.index("if (have_ssid)")
    arm_idx = body.index("EnsureWifiRecoveryTimeout();", have_idx)
    start_idx = body.index(
        "WifiManager::GetInstance().StartStationIfScanIdle()", have_idx
    )
    return_idx = body.index("return start_result;", start_idx)

    assert have_idx < arm_idx < start_idx < return_idx
    assert "ShouldArmWifiConnectTimeout(start_result)" not in body


def test_wb12aa_wifi_recovery_timeout_helper_is_non_sliding_and_offline_only():
    wifi_board = read("main/boards/common/wifi_board.cc")
    wifi_header = read("main/boards/common/wifi_board.h")
    body = _func_body(
        wifi_board,
        "void WifiBoard::EnsureWifiRecoveryTimeout()",
        "WifiStationStartResult WifiBoard::TryWifiConnect()",
    )

    assert "void EnsureWifiRecoveryTimeout();" in wifi_header
    assert "in_config_mode_" in body
    assert "WifiManager::GetInstance().IsConnected()" in body
    active_idx = body.index("esp_timer_is_active(connect_timer_)")
    arm_idx = body.index(
        "esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL)"
    )
    assert active_idx < arm_idx
    assert "return;" in body[:arm_idx]
    assert "WiFi recovery timeout armed" in body
```

- [ ] **Step 2: Run the tests and verify RED**

Run:

```bash
python3 -m pytest -q \
  tests/test_wifi_board_provisioning.py::test_wb12a_saved_wifi_arms_recovery_before_station_start_outcome \
  tests/test_wifi_board_provisioning.py::test_wb12aa_wifi_recovery_timeout_helper_is_non_sliding_and_offline_only
```

Expected: both tests fail because `EnsureWifiRecoveryTimeout()` does not exist and startup only arms the timer for `kStartedNow`.

- [ ] **Step 3: Commit the RED tests**

```bash
git add tests/test_wifi_board_provisioning.py
git commit -m "test(wifi): reproduce missing offline recovery deadline"
```

### Task 2: Arm one fixed deadline for every offline station start

**Files:**
- Modify: `main/boards/common/wifi_board.h`
- Modify: `main/boards/common/wifi_board.cc`
- Test: `tests/test_wifi_board_provisioning.py`

- [ ] **Step 1: Declare the recovery helper**

Add beside `TryWifiConnect()` in `wifi_board.h`:

```cpp
    void EnsureWifiRecoveryTimeout();
```

- [ ] **Step 2: Implement non-sliding recovery arming**

Insert before `TryWifiConnect()` in `wifi_board.cc`:

```cpp
void WifiBoard::EnsureWifiRecoveryTimeout() {
    auto& wifi = WifiManager::GetInstance();
    if (in_config_mode_ || wifi.IsConnected()) {
        return;
    }
    if (esp_timer_is_active(connect_timer_)) {
        ESP_LOGI(TAG, "WiFi recovery timeout already armed; retaining deadline");
        return;
    }

    const esp_err_t timer_error =
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
    if (timer_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to arm WiFi recovery timeout: %s",
                 esp_err_to_name(timer_error));
        return;
    }
    ESP_LOGI(TAG, "WiFi recovery timeout armed for %ds", CONNECT_TIMEOUT_SEC);
}
```

- [ ] **Step 3: Arm before station startup and preserve every outcome**

Replace the saved-SSID start block with:

```cpp
        auto& app = Application::GetInstance();
        app.EnsureBleAdvertisingForUnclaimedSavedWifi();
        EnsureWifiRecoveryTimeout();

        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        const auto start_result =
            WifiManager::GetInstance().StartStationIfScanIdle();
        if (start_result == WifiStationStartResult::kBusyOrFailed) {
            ESP_LOGW(TAG, "WiFi station start busy; recovery deadline remains armed");
        } else if (start_result == WifiStationStartResult::kAlreadyActive) {
            ESP_LOGI(TAG, "WiFi station already active; recovery deadline remains armed");
        }
        return start_result;
```

This deliberately removes the `ShouldArmWifiConnectTimeout(start_result)` gate. The deadline measures continuous offline time, not whether this call created the station generation.

- [ ] **Step 4: Reuse the helper for runtime disconnects**

Replace the manual timer block in `NetworkEvent::Disconnected` with:

```cpp
            EnsureWifiRecoveryTimeout();
```

- [ ] **Step 5: Update the runtime-disconnect contract to follow the helper**

Replace `test_wb14_runtime_disconnect_arms_non_sliding_recovery_timeout` with:

```python
def test_wb14_runtime_disconnect_uses_non_sliding_recovery_helper():
    wifi_board = read("main/boards/common/wifi_board.cc")
    fn = _func_body(
        wifi_board,
        "void WifiBoard::OnNetworkEvent(",
        "void WifiBoard::SetNetworkEventCallback(",
    )
    case_idx = fn.index("case NetworkEvent::Disconnected:")
    case_end = fn.index("case NetworkEvent::WifiConfigModeEnter:", case_idx)
    body = fn[case_idx:case_end]
    helper = _func_body(
        wifi_board,
        "void WifiBoard::EnsureWifiRecoveryTimeout()",
        "WifiStationStartResult WifiBoard::TryWifiConnect()",
    )

    assert "EnsureWifiRecoveryTimeout();" in body
    assert "esp_timer_start_once" not in body
    assert "esp_timer_is_active(connect_timer_)" in helper
    assert helper.index("esp_timer_is_active(connect_timer_)") < helper.index(
        "esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL)"
    )
```

- [ ] **Step 6: Run focused GREEN tests**

Run:

```bash
python3 -m pytest -q \
  tests/test_wifi_board_provisioning.py::test_wb12a_saved_wifi_arms_recovery_before_station_start_outcome \
  tests/test_wifi_board_provisioning.py::test_wb12aa_wifi_recovery_timeout_helper_is_non_sliding_and_offline_only \
  tests/test_wifi_board_provisioning.py::test_wb14_runtime_disconnect_uses_non_sliding_recovery_helper \
  tests/test_wifi_board_provisioning.py::test_wb15_runtime_reconnect_cancels_pending_recovery_timeout
```

Expected: all four tests pass.

- [ ] **Step 7: Commit the fixed startup deadline**

```bash
git add main/boards/common/wifi_board.h main/boards/common/wifi_board.cc tests/test_wifi_board_provisioning.py
git commit -m "fix(wifi): arm offline recovery for every station start"
```

### Task 3: Make stale expiry and connection races explicit

**Files:**
- Modify: `tests/test_wifi_board_provisioning.py`
- Modify: `main/boards/common/wifi_board.cc`

- [ ] **Step 1: Add a failing stale-expiry test**

```python
def test_wb18a_timeout_does_not_request_ble_after_wifi_recovers():
    wifi_board = read("main/boards/common/wifi_board.cc")
    body = _func_body(
        wifi_board,
        "void WifiBoard::OnWifiConnectTimeout(",
        "// ---",
    )

    connected_idx = body.index("WifiManager::GetInstance().IsConnected()")
    request_idx = body.index("board->RequestWifiConfigMode(false, true);")
    assert connected_idx < request_idx
    guard = body[connected_idx:request_idx]
    assert "WiFi recovery timeout ignored because station recovered" in guard
    assert "return;" in guard
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
python3 -m pytest -q tests/test_wifi_board_provisioning.py::test_wb18a_timeout_does_not_request_ble_after_wifi_recovers
```

Expected: FAIL because expiry currently relies only on the later main-task conditional check.

- [ ] **Step 3: Add the callback-side live-state guard**

Immediately after the active-config guard in `OnWifiConnectTimeout()` add:

```cpp
    if (WifiManager::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "WiFi recovery timeout ignored because station recovered");
        return;
    }
```

Keep `RequestWifiConfigMode(false, true)` unchanged so all later race boundaries remain protected by the existing conditional transaction.

- [ ] **Step 4: Verify GREEN and the complete Wi-Fi board contract**

Run:

```bash
python3 -m pytest -q tests/test_wifi_board_provisioning.py
```

Expected: all tests pass.

- [ ] **Step 5: Commit the race guard**

```bash
git add main/boards/common/wifi_board.cc tests/test_wifi_board_provisioning.py
git commit -m "fix(wifi): ignore stale offline recovery expiry"
```

### Task 4: Verify owned identity and transactional SSID safety

**Files:**
- Test: `tests/test_wifi_board_provisioning.py`
- Test: `tests/test_ssid_manager_contract.py`
- Test: `tests/test_blufi_provisioning_stability.py`
- Test: `tests/test_wifi_provisioning_brand.py`

- [ ] **Step 1: Run ownership/config-entry contracts**

```bash
python3 -m pytest -q \
  tests/test_wifi_board_provisioning.py \
  tests/test_ssid_manager_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_wifi_provisioning_brand.py
```

Expected: all tests pass, including conditional connection re-checks, claimed credential-only activation, saved-tuple rollback, and WebSocket-token refresh.

- [ ] **Step 2: Run the full firmware regression suite**

```bash
python3 -m pytest -q tests
```

Expected: all firmware-owned tests pass. Do not run pytest against `managed_components/`; its upstream LVGL generator requires unrelated host tools.

- [ ] **Step 3: Check formatting and secrets policy**

```bash
git diff --check
python3 -m pytest -q tests/test_provisioning_log_redaction.py
```

Expected: no whitespace errors and no credential-bearing logs.

### Task 5: Verify the mobile offline discovery window

**Files:**
- Verify: `/Users/manhhodinh/Documents/TBOT/tbot-mobile/src/features/device/pairing/screens/PairSearchScreen.tsx`
- Test: `/Users/manhhodinh/Documents/TBOT/tbot-mobile/tests/features/device/pair-search-multi-device.test.tsx`

- [ ] **Step 1: Run the reconnect-window regression**

```bash
cd /Users/manhhodinh/Documents/TBOT/tbot-mobile
npx jest --selectProjects unit \
  tests/features/device/pair-search-multi-device.test.tsx --runInBand
```

Expected: the test proves reconnect mode makes two `scanForTJBotDevices(40_000)` calls and can discover the owned robot on the second scan.

- [ ] **Step 2: Run mobile type and focused Wi-Fi checks**

```bash
npm run typecheck
npx jest --selectProjects unit \
  tests/ble/service.test.ts \
  tests/features/device/pair-wifi-flow.test.tsx \
  tests/navigation/device-pairing-route-params.test.ts --runInBand
```

Expected: all checks pass. No mobile production change or commit is required unless this verification exposes a regression.

### Task 6: Build and flash the firmware app image

**Files:**
- Build artifact: `build/xiaozhi.bin`
- Hardware port: `/dev/cu.usbmodem1101`

- [ ] **Step 1: Stop serial monitors and verify the target port**

```bash
ps -axo pid=,command= | rg 'serial.tools.miniterm|idf.py monitor'
ls -l /dev/cu.usbmodem1101
lsof /dev/cu.usbmodem1101
```

Expected: the port exists and no unrelated process owns it.

- [ ] **Step 2: Build with the established ESP-IDF environment**

```bash
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py build
```

Expected: exit `0` and `build/xiaozhi.bin` exists.

- [ ] **Step 3: Flash only the application partition**

```bash
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 app-flash
```

Expected: flash succeeds without erasing NVS, cloud identity, saved Wi-Fi, bootloader, partition table, or assets.

### Task 7: Physical Android and robot end-to-end proof

**Files:**
- Android package: `com.TJBotmobile`
- Android serial: `efc5314f`
- Robot serial: `/dev/cu.usbmodem1101`

- [ ] **Step 1: Start secret-safe serial monitoring**

```bash
python3 -m serial.tools.miniterm /dev/cu.usbmodem1101 115200 --raw
```

Expected: boot logs appear. Do not include a password in shell arguments or captured reports.

- [ ] **Step 2: Prove automatic BLE fallback without BOOT**

Boot the robot while its saved access point is unavailable. Do not press BOOT.

Expected serial sequence within approximately 60 seconds:

```text
WiFi recovery timeout armed for 60s
WiFi connection timeout, entering config mode
EnterWifiConfigMode / WiFi config mode
BLE setup timer armed
```

Expected Android behavior: open the owned robot, tap **Doi Wi-Fi**, and the reconnect search discovers `TBOT-288485851A80` during its two-scan window.

- [ ] **Step 3: Select the target network and enter its password securely**

Use UI automation only for taps. Focus the Android password field, read the password through a TTY with echo disabled, send it through interactive stdin, and unset the shell variable. Never place it in a command argument, environment listing, source file, or report.

Expected: the robot-side Wi-Fi list contains the user-selected SSID and the app enables **Ket noi Robot**.

- [ ] **Step 4: Verify provisioning completion**

Expected serial evidence:

```text
DH negotiation completed successfully
Recv STA SSID (len=...)
Recv STA PASSWORD
Got IP: ...
Successful provisioning teardown complete
Claimed after BluFi Wi-Fi success
token_empty=0
Session ID: ...
Heartbeat accepted (HTTP 204)
```

Reject the run if logs contain `Authentication failed`, an endless
`wifi_configuring` state, or a credential value.

- [ ] **Step 5: Verify the Android result**

Dump the UI hierarchy and confirm the device page shows:

```text
Truc tuyen
Living-room Robot
Van Phong Tam Dentist
```

- [ ] **Step 6: Repeat the offline recovery cycle twice more**

For each cycle, make the saved AP unavailable, reboot/reset through USB RTS only,
wait for automatic BLE advertising, enter through **Doi Wi-Fi**, and reconnect.
Do not press BOOT.

Expected: three consecutive automatic fallback and reconnect cycles complete,
with no stuck initialization, GATT deadlock, token loss, or manual robot-button
dependency.

### Task 8: Final verification and integration

**Files:**
- Verify all changed firmware files and commits.

- [ ] **Step 1: Run fresh final gates**

```bash
python3 -m pytest -q tests
git diff --check
git status --short
git log -5 --oneline --decorate
```

Expected: all tests pass, the worktree is clean, and the Wi-Fi recovery commits are on the intended branch.

- [ ] **Step 2: Merge into main if implementation used a feature branch**

```bash
git switch main
git merge --ff-only fix/automatic-offline-wifi-recovery
```

Expected: fast-forward succeeds. If work was executed directly on `main`, record that no merge was necessary.

- [ ] **Step 3: Re-run focused tests on merged main**

```bash
python3 -m pytest -q \
  tests/test_wifi_board_provisioning.py \
  tests/test_wifi_provisioning_brand.py
```

Expected: all focused tests pass on `main`.
