# Offline Wake-Word Rearm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep "Hi ESP" detection active in safe claimed idle state even when the passive production WebSocket has not connected.

**Architecture:** Rearm the already-prewarmed WakeNet engine at the activation-to-idle boundary instead of waiting for passive WebSocket success. Preserve all existing lesson, provisioning, quiet-sync, generation, and deferred-wake ownership gates; passive WebSocket remains a background optimization.

**Tech Stack:** ESP-IDF/C++ firmware, Python source-contract tests with pytest, esptool hardware flashing and serial verification.

---

## File Structure

- Modify `main/application.cc`: rearm wake detection at the safe activation boundary and restore it after passive connection failure/watchdog backoff.
- Modify `tests/test_lesson_passive_websocket_contract.py`: lock the activation-before-WebSocket behavior and passive failure recovery.
- Modify `tests/test_realtime_voice_state.py`: lock watchdog recovery without weakening lesson safety.
- Create `build-production-offline-wake/`: generated production build output; do not commit it.

### Task 1: Lock activation-time wake rearm

**Files:**
- Modify: `tests/test_lesson_passive_websocket_contract.py`
- Test: `tests/test_lesson_passive_websocket_contract.py`

- [ ] **Step 1: Write the failing activation contract**

Add:

```python
def test_claimed_activation_rearms_wake_before_passive_websocket_success():
    source = read("main/application.cc")
    activation = function_body(source, "void Application::HandleActivationDoneEvent")

    assert "SetDeviceState(kDeviceStateIdle);" in activation
    assert "IsDeviceClaimed()" in activation
    assert "!lesson_asset_sync_quiet_.load()" in activation
    assert "audio_service_.EnableWakeWordDetection(true);" in activation
    assert activation.index("SetDeviceState(kDeviceStateIdle);") < activation.index(
        "audio_service_.EnableWakeWordDetection(true);"
    )
    assert "protocol_->IsAudioChannelOpened()" not in activation
```

- [ ] **Step 2: Run the test and verify RED**

Run: `python3 -m pytest tests/test_lesson_passive_websocket_contract.py::test_claimed_activation_rearms_wake_before_passive_websocket_success -q`

Expected: FAIL because `HandleActivationDoneEvent()` does not call `EnableWakeWordDetection(true)`.

- [ ] **Step 3: Implement the minimal activation rearm**

In `HandleActivationDoneEvent()`, immediately after `SetDeviceState(kDeviceStateIdle);`, add:

```cpp
    if (IsDeviceClaimed() && !lesson_asset_sync_quiet_.load()) {
        audio_service_.EnableWakeWordDetection(true);
        ESP_LOGI(TAG, "claimed_idle_wake_word_rearmed running=%d",
                 audio_service_.IsWakeWordRunning() ? 1 : 0);
    }
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run: `python3 -m pytest tests/test_lesson_passive_websocket_contract.py::test_claimed_activation_rearms_wake_before_passive_websocket_success -q`

Expected: `1 passed`.

- [ ] **Step 5: Commit**

```bash
git add main/application.cc tests/test_lesson_passive_websocket_contract.py
git commit -m "fix: rearm claimed wake word before passive websocket"
```

### Task 2: Preserve wake detection through passive failures

**Files:**
- Modify: `tests/test_lesson_passive_websocket_contract.py`
- Modify: `tests/test_realtime_voice_state.py`
- Modify: `main/application.cc`

- [ ] **Step 1: Write failing passive failure contracts**

Add to `tests/test_lesson_passive_websocket_contract.py`:

```python
def test_passive_websocket_failure_restores_safe_idle_wake_detection():
    source = read("main/application.cc")
    open_task = function_body(source, "void Application::OpenChannelTask")
    failure = open_task[open_task.index('ESP_LOGW(TAG, "passive_lesson_websocket_failed")') :]

    assert "IsDeviceClaimed()" in failure
    assert "!lesson_runtime_active_.load()" in failure
    assert "!lesson_asset_sync_quiet_.load()" in failure
    assert "audio_service_.EnableWakeWordDetection(true);" in failure
    assert failure.index("audio_service_.EnableWakeWordDetection(true);") < failure.index(
        "SchedulePassiveLessonReconnect();"
    )
```

Add to `tests/test_realtime_voice_state.py`:

```python
def test_passive_connect_watchdog_keeps_safe_idle_wake_detection_available():
    app_cc = read("main/application.cc")
    start = app_cc.index("void Application::HandleConnectWatchdog")
    end = app_cc.index("void Application::ScheduleReconnect", start)
    watchdog = app_cc[start:end]
    passive = watchdog[
        watchdog.index('ESP_LOGW(TAG, "passive_lesson_connect_watchdog_timeout -> passive backoff")') :
        watchdog.index("if (lesson_runtime_active_.load())")
    ]

    assert "IsDeviceClaimed()" in passive
    assert "!lesson_asset_sync_quiet_.load()" in passive
    assert "audio_service_.EnableWakeWordDetection(true);" in passive
```

- [ ] **Step 2: Run both tests and verify RED**

Run:

```bash
python3 -m pytest \
  tests/test_lesson_passive_websocket_contract.py::test_passive_websocket_failure_restores_safe_idle_wake_detection \
  tests/test_realtime_voice_state.py::test_passive_connect_watchdog_keeps_safe_idle_wake_detection_available -q
```

Expected: both FAIL because passive failure/backoff does not explicitly restore WakeNet.

- [ ] **Step 3: Add a safe local helper**

Declare in `main/application.h` near the connection helpers:

```cpp
    void RearmClaimedIdleWakeWord();
```

Define in `main/application.cc` before `HandleActivationDoneEvent()`:

```cpp
void Application::RearmClaimedIdleWakeWord() {
    if (!IsDeviceClaimed() || lesson_runtime_active_.load() ||
        lesson_asset_sync_quiet_.load() || GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    audio_service_.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "claimed_idle_wake_word_rearmed running=%d",
             audio_service_.IsWakeWordRunning() ? 1 : 0);
}
```

Replace Task 1's inline activation block with `RearmClaimedIdleWakeWord();`.

In passive worker failure, before `SchedulePassiveLessonReconnect();`, add:

```cpp
                    self->RearmClaimedIdleWakeWord();
```

In the passive watchdog backoff branch, before `SchedulePassiveLessonReconnect();`, add:

```cpp
        RearmClaimedIdleWakeWord();
```

- [ ] **Step 4: Run focused contracts and verify GREEN**

Run:

```bash
python3 -m pytest \
  tests/test_lesson_passive_websocket_contract.py \
  tests/test_realtime_voice_state.py \
  tests/test_lesson_sd_sync_worker_contract.py -q
```

Expected: all selected tests pass with no lesson or quiet-sync regression.

- [ ] **Step 5: Commit**

```bash
git add main/application.h main/application.cc \
  tests/test_lesson_passive_websocket_contract.py \
  tests/test_realtime_voice_state.py
git commit -m "fix: preserve idle wake word during passive reconnect"
```

### Task 3: Run firmware verification and build production image

**Files:**
- Generated: `build-production-offline-wake/`

- [ ] **Step 1: Run the full Python firmware suite**

Run: `python3 -m pytest tests -q`

Expected: all tests pass.

- [ ] **Step 2: Configure a clean production build**

Run:

```bash
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py -B build-production-offline-wake fullclean
idf.py -B build-production-offline-wake reconfigure
```

Expected: generated config contains:

```text
CONFIG_OTA_URL="https://esp.tjbot.vn/tbot/ota/"
CONFIG_WEBSOCKET_URL="wss://esp.tjbot.vn/tbot/v1/"
# CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT is not set
```

- [ ] **Step 3: Build the firmware**

Run: `idf.py -B build-production-offline-wake build`

Expected: exit code 0 and `build-production-offline-wake/xiaozhi.bin` exists.

- [ ] **Step 4: Verify endpoint and image identity**

Run:

```bash
rg '^CONFIG_(OTA_URL|WEBSOCKET_URL)|CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT' \
  build-production-offline-wake/sdkconfig
shasum -a 256 build-production-offline-wake/xiaozhi.bin
```

Expected: production endpoints only and a recorded SHA-256.

### Task 4: Flash while preserving NVS and verify hardware

**Files:**
- Create: `.codex_tmp/offline-wake-20260825/nvs-before.bin`
- Create: `.codex_tmp/offline-wake-20260825/nvs-after.bin`
- Create: `.codex_tmp/offline-wake-20260825/serial.log`

- [ ] **Step 1: Back up NVS**

Run:

```bash
mkdir -p ../../.codex_tmp/offline-wake-20260825
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 460800 \
  read-flash 0x9000 0x4000 ../../.codex_tmp/offline-wake-20260825/nvs-before.bin
```

Expected: 16 KiB backup created.

- [ ] **Step 2: Flash only generated image regions**

From `build-production-offline-wake`, run the offsets in its generated `flash_args`; confirm no entry writes `0x9000`.

Expected: esptool verifies every written region hash and resets the robot.

- [ ] **Step 3: Verify NVS preservation**

Read `0x9000`/`0x4000` again to `nvs-after.bin`, then run:

```bash
cmp ../../.codex_tmp/offline-wake-20260825/nvs-before.bin \
    ../../.codex_tmp/offline-wake-20260825/nvs-after.bin
```

Expected: exit code 0.

- [ ] **Step 4: Verify wake before WebSocket success**

Capture serial from reset and require this ordering:

```text
StateMachine: State: activating -> idle
claimed_idle_wake_word_rearmed running=1
```

The rearm marker must appear without requiring `WS: Session ID:` first.

- [ ] **Step 5: Verify spoken wake end-to-end**

Say "Hi ESP" while the robot is idle. Require serial markers showing:

```text
Wake word detected: Hi ESP
StateMachine: State: idle -> connecting
StateMachine: State: connecting -> listening
StateMachine: State: listening -> speaking
```

Expected: local WakeNet triggers, the foreground channel connects, audio sends successfully, and the robot speaks a response.

- [ ] **Step 6: Commit final test/code state if verification required adjustments**

```bash
git status --short
git add main/application.h main/application.cc tests
git commit -m "test: verify offline wake word recovery"
```

Skip this commit when the worktree is already clean after Tasks 1-2.
