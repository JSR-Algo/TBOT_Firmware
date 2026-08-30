# Claimed Idle Heartbeat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep backend management presence alive for a claimed, Wi-Fi-connected idle robot even when its passive lesson WebSocket closes or fails.

**Architecture:** Add one firmware policy helper for claimed-idle management heartbeat ownership. Apply it only at protocol error/close boundaries; real network loss, setup entry, claim removal, and heartbeat authentication recovery keep their existing stop behavior.

**Tech Stack:** ESP-IDF C++, FreeRTOS timers/tasks, pytest source-contract tests, esptool, Android ADB/UIAutomator.

---

### Task 1: Lock the regression with a failing contract test

**Files:**
- Modify: `tests/test_lesson_passive_websocket_contract.py`

- [ ] **Step 1: Add the failing test**

```python
def test_claimed_idle_management_heartbeat_survives_passive_websocket_churn():
    source = read("main/application.cc")
    header = read("main/application.h")
    initialize = function_body(source, "void Application::InitializeProtocol")

    assert "bool ShouldKeepManagementHeartbeat() const;" in header
    assert "bool Application::ShouldKeepManagementHeartbeat() const" in source

    network_error = initialize[
        initialize.index("protocol_->OnNetworkError") :
        initialize.index("protocol_->OnIncomingAudio")
    ]
    closed = initialize[
        initialize.index("protocol_->OnAudioChannelClosed") :
        initialize.index("protocol_->OnIncomingJson")
    ]
    for callback in (network_error, closed):
        assert "ShouldKeepManagementHeartbeat()" in callback
        assert "StartHeartbeat();" in callback
        assert "StopHeartbeat();" in callback
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: `pytest -q tests/test_lesson_passive_websocket_contract.py::test_claimed_idle_management_heartbeat_survives_passive_websocket_churn`

Expected: FAIL because `ShouldKeepManagementHeartbeat` does not exist.

### Task 2: Implement the minimal heartbeat ownership policy

**Files:**
- Modify: `main/application.h`
- Modify: `main/application.cc`

- [ ] **Step 1: Declare and define the helper**

```cpp
bool Application::ShouldKeepManagementHeartbeat() const {
    return IsDeviceClaimed() &&
           !lesson_runtime_active_.load() &&
           GetDeviceState() != kDeviceStateWifiConfiguring &&
           GetDeviceState() != kDeviceStateAudioTesting;
}
```

- [ ] **Step 2: Apply it at WebSocket error and close boundaries**

```cpp
if (ShouldKeepManagementHeartbeat()) {
    StartHeartbeat();
    DispatchDeviceHeartbeat();
} else {
    StopHeartbeat();
}
```

Use this policy in `OnNetworkError` and `OnAudioChannelClosed`; preserve all existing reconnect, UI, and lesson branches.

- [ ] **Step 3: Run the focused test and verify GREEN**

Run: `pytest -q tests/test_lesson_passive_websocket_contract.py::test_claimed_idle_management_heartbeat_survives_passive_websocket_churn`

Expected: PASS.

### Task 3: Reconcile related contracts and build

**Files:**
- Modify only stale assertions in existing focused tests when they contradict the approved management-heartbeat policy.

- [ ] **Step 1: Run focused firmware contracts**

Run: `pytest -q tests/test_lesson_passive_websocket_contract.py tests/test_claimed_state_recovery.py tests/test_tbot_connect_review_fixes.py tests/test_tbot_connect_runtime_fsm_contract.py tests/test_lesson_sd_sync_no_claim_gate_contract.py`

Expected: all relevant tests PASS; update only assertions that previously required unconditional WebSocket-driven heartbeat stop.

- [ ] **Step 2: Build firmware**

Run: `/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python /Users/manhhodinh/esp/esp-idf/tools/idf.py build`

Expected: build completes and produces `build/xiaozhi.bin`.

### Task 4: Flash and run Android-to-robot E2E

**Files:**
- No source changes unless new evidence reproduces a distinct bug.

- [ ] **Step 1: Flash without BOOT**

Run: `/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python /Users/manhhodinh/esp/esp-idf/tools/idf.py -p /dev/cu.usbmodem1101 flash`

Expected: every image hash verifies and RTS resets the robot.

- [ ] **Step 2: Provision through Android using the user-selected Wi-Fi**

Use ADB/UIAutomator to retry Bluetooth setup, select the discovered robot, select the requested network, enter its password without logging it, and connect.

Expected: BLE transfer, Wi-Fi association, claim confirmation, and app online transition complete.

- [ ] **Step 3: Observe runtime stability**

Capture redacted serial evidence for at least two 20-second heartbeat intervals while passive WebSocket reconnects occur.

Expected: repeated `Heartbeat accepted`, no panic/abort/assert/reboot, and app remains online.

- [ ] **Step 4: Repeat reconnect/change-Wi-Fi journeys**

Run at least two additional disconnect/reconnect cycles and then a Wi-Fi-change cycle without pressing BOOT.

Expected: every cycle returns online and keeps management heartbeat alive.
