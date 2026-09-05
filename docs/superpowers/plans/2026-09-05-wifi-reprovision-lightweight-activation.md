# Wi-Fi Reprovision Lightweight Activation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make claimed-robot Wi-Fi reprovision recover WebSocket, heartbeat, and wake-word reliably under fragmented ESP32-S3 internal RAM.

**Architecture:** Claimed reprovision bypasses the cold-boot OTA/config-refresh worker and completes a lightweight activation from persisted runtime settings. Lesson asset sync completion uses a restartable one-shot timer, so wake-word is rearmed once after the sync burst rather than after every asset. The AFE fetch task retains its measured 4096-byte stack in PSRAM to reserve internal RAM for Wi-Fi/TLS.

**Tech Stack:** ESP-IDF 5.5, C++17, FreeRTOS, ESP timers, ESP-BluFi, Python pytest contract tests, Android ADB physical E2E.

---

### Task 1: Lightweight Claimed Reprovision Activation

**Files:**
- Modify: `main/application.h`
- Modify: `main/application.cc`
- Test: `tests/test_wifi_provisioning_brand.py`

- [ ] **Step 1: Write the failing contract test**

Add a test that extracts `Application::PromoteFromWifiConfigAfterProvisioning()` and requires claimed reprovision to call a lightweight helper without creating the full activation task:

```python
def test_claimed_blufi_reprovision_uses_lightweight_activation():
    header = read("main/application.h")
    source = read("main/application.cc")
    promote = function_body(source, "void Application::PromoteFromWifiConfigAfterProvisioning")

    assert "void CompleteClaimedWifiReprovisionActivation();" in header
    assert "CompleteClaimedWifiReprovisionActivation();" in promote
    assert "xTaskCreate" not in promote
    assert "ActivationTask();" not in promote
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
python3 -m pytest -q tests/test_wifi_provisioning_brand.py -k claimed_blufi_reprovision_uses_lightweight_activation
```

Expected: FAIL because the helper does not exist and `PromoteFromWifiConfigAfterProvisioning()` still creates the 8 KB activation worker.

- [ ] **Step 3: Implement the lightweight helper**

Declare the helper in `main/application.h`:

```cpp
void CompleteClaimedWifiReprovisionActivation();
```

Implement it in `main/application.cc` using persisted runtime configuration and the normal activation-done event:

```cpp
void Application::CompleteClaimedWifiReprovisionActivation() {
    if (!ota_) {
        ota_ = std::make_unique<Ota>();
    }
    ota_->MarkCurrentVersionValid();

    DoResetProtocol();
    InitializeProtocol();
    SystemInfo::PrintHeapCheckpoint("wifi_reprovision_activation.complete");
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}
```

Replace the claimed branch in `PromoteFromWifiConfigAfterProvisioning()` with:

```cpp
ESP_LOGI(TAG, "Claimed after BluFi Wi-Fi success: run lightweight activation");
CompleteClaimedWifiReprovisionActivation();
```

Keep the existing stale-state and `WifiManager::IsConnected()` guards and keep the unclaimed branch unchanged.

- [ ] **Step 4: Run focused provisioning and protocol tests**

Run:

```bash
python3 -m pytest -q \
  tests/test_wifi_provisioning_brand.py \
  tests/test_tbot_connect_config.py \
  tests/test_lesson_passive_websocket_contract.py
```

Expected: PASS.

- [ ] **Step 5: Commit the lightweight activation change**

```bash
git add main/application.h main/application.cc tests/test_wifi_provisioning_brand.py
git commit -m "fix: lighten claimed wifi reprovision activation"
```

### Task 2: Debounce Post-Sync Wake-Word Rearm

**Files:**
- Modify: `main/application.h`
- Modify: `main/application.cc`
- Test: `tests/test_lesson_sd_sync_worker_contract.py`

- [ ] **Step 1: Write the failing debounce contract**

Extend the sync lifecycle test with:

```python
assert "esp_timer_handle_t lesson_asset_sync_wake_rearm_timer_" in app_header
assert "void ScheduleLessonAssetSyncWakeRearm();" in app_header

begin = function_body(app_source, "bool Application::BeginLessonAssetSyncQuiet")
assert "esp_timer_stop(lesson_asset_sync_wake_rearm_timer_)" in begin

end = function_body(app_source, "void Application::EndLessonAssetSyncQuiet")
assert "ScheduleLessonAssetSyncWakeRearm();" in end
assert "RearmClaimedIdleWakeWord();" not in end

schedule = function_body(app_source, "void Application::ScheduleLessonAssetSyncWakeRearm")
assert "esp_timer_start_once" in schedule
assert "1500ULL * 1000ULL" in schedule
assert "self->Schedule" in schedule
assert "self->RearmClaimedIdleWakeWord();" in schedule
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
python3 -m pytest -q tests/test_lesson_sd_sync_worker_contract.py
```

Expected: FAIL because sync completion currently schedules immediate rearm.

- [ ] **Step 3: Add the one-shot timer lifecycle**

Add to `main/application.h`:

```cpp
esp_timer_handle_t lesson_asset_sync_wake_rearm_timer_ = nullptr;
void ScheduleLessonAssetSyncWakeRearm();
```

Stop and delete the timer in `Application::~Application()`. At the beginning of a valid new sync, cancel any pending rearm:

```cpp
if (lesson_asset_sync_wake_rearm_timer_ != nullptr) {
    esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
}
```

Implement the restartable timer:

```cpp
void Application::ScheduleLessonAssetSyncWakeRearm() {
    if (lesson_asset_sync_wake_rearm_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() { self->RearmClaimedIdleWakeWord(); });
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "asset_wake";
        args.skip_unhandled_events = true;
        if (esp_timer_create(&args, &lesson_asset_sync_wake_rearm_timer_) != ESP_OK) {
            lesson_asset_sync_wake_rearm_timer_ = nullptr;
            ESP_LOGE(TAG, "Failed to create lesson asset wake rearm timer");
            return;
        }
    }
    esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
    esp_timer_start_once(lesson_asset_sync_wake_rearm_timer_, 1500ULL * 1000ULL);
}
```

Call `ScheduleLessonAssetSyncWakeRearm()` from `EndLessonAssetSyncQuiet()` after clearing the quiet flag and validating claimed/idle state.

- [ ] **Step 4: Run focused lifecycle tests**

Run:

```bash
python3 -m pytest -q \
  tests/test_lesson_sd_sync_worker_contract.py \
  tests/test_audio_rearm_transaction_contract.py \
  tests/test_lesson_passive_websocket_contract.py
```

Expected: `42 passed` or higher after the new assertions.

- [ ] **Step 5: Commit the debounce change**

```bash
git add main/application.h main/application.cc tests/test_lesson_sd_sync_worker_contract.py
git commit -m "fix: debounce wake rearm after asset sync"
```

### Task 3: Reserve Internal RAM for Wi-Fi and TLS

**Files:**
- Modify: `main/audio/wake_words/afe_wake_word.cc`
- Test: `tests/test_realtime_voice_state.py`
- Test: `tests/test_audio_rearm_transaction_contract.py`

- [ ] **Step 1: Verify the PSRAM stack contract is present**

The test must require:

```python
assert "xTaskCreateWithCaps" in wake_task
assert '"audio_detection", 4096, this, tskIDLE_PRIORITY + 1' in wake_task
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in wake_task
```

- [ ] **Step 2: Verify transactional initialization remains enforced**

Run:

```bash
python3 -m pytest -q \
  tests/test_realtime_voice_state.py -k afe_background_tasks_keep_fetch_below_feed_but_above_idle \
  tests/test_audio_rearm_transaction_contract.py
```

Expected: PASS; initialization failure still destroys `afe_data_` and clears the task handle.

- [ ] **Step 3: Build the firmware**

Run:

```bash
IDF_PATH=/Users/manhhodinh/esp/esp-idf \
IDF_PYTHON_ENV_PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env \
/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python \
/Users/manhhodinh/esp/esp-idf/tools/idf.py build
```

Expected: `Project build complete` with no compiler or linker errors.

- [ ] **Step 4: Commit the AFE stack allocation change**

```bash
git add main/audio/wake_words/afe_wake_word.cc tests/test_realtime_voice_state.py tests/test_audio_rearm_transaction_contract.py
git commit -m "fix: reserve internal ram for wifi reprovision"
```

### Task 4: Automated Regression Verification

**Files:**
- Verify: `tests/`
- Verify mobile repository: `/Users/manhhodinh/Documents/TBOT/tbot-mobile`

- [ ] **Step 1: Run all firmware project tests**

```bash
python3 -m pytest -q tests
```

Expected: all project tests pass. Do not use repository-root pytest collection because managed LVGL tests require unavailable `doxygen`.

- [ ] **Step 2: Run mobile tests**

```bash
npm test -- --runInBand
```

Expected baseline: 234 suites passed, 1 skipped; 2819 tests passed, 19 skipped, plus any newly added tests.

- [ ] **Step 3: Run mobile static checks**

```bash
npm run typecheck
npm run lint
```

Expected: both commands exit 0.

- [ ] **Step 4: Review the complete diff**

```bash
git diff --check
git status --short --branch
git diff -- main/application.h main/application.cc main/audio/wake_words/afe_wake_word.cc tests
```

Expected: no whitespace errors and no unrelated files added to firmware commits.

### Task 5: Flash and Physical Android-to-Robot E2E

**Files:**
- Flash artifact: `build/xiaozhi.bin`
- Device: Android serial `efc5314f`
- Robot serial: `/dev/cu.usbmodem1101`

- [ ] **Step 1: Flash the application image**

```bash
IDF_PATH=/Users/manhhodinh/esp/esp-idf \
IDF_PYTHON_ENV_PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env \
/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python \
/Users/manhhodinh/esp/esp-idf/tools/idf.py -p /dev/cu.usbmodem1101 app-flash
```

Expected: image hash verified and hard reset succeeds.

- [ ] **Step 2: Start serial evidence capture and restore Metro forwarding**

```bash
/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  -m serial.tools.miniterm /dev/cu.usbmodem1101 115200 --raw

/Users/manhhodinh/Library/Android/sdk/platform-tools/adb \
  -s efc5314f reverse tcp:8081 tcp:8081
```

Expected: robot boots, passive WebSocket opens, heartbeat returns 204, and `wake_running=1` after the initial sync burst.

- [ ] **Step 3: Run three consecutive correct-credential cycles**

For each cycle, use the phone UI: Device tab → Change Wi-Fi → wait for automatic robot setup → select the target AP → enter the authorized password without printing it → Connect.

Require for every cycle:

```text
System command: wifi_setup
Recv STA SSID (len=21)
Recv STA PASSWORD
Got IP: 192.168.100.13
passive_lesson_websocket_opened
Heartbeat accepted (HTTP 204)
claimed_idle_wake_word_rearmed running=1
```

The app must leave the connecting screen only after a fresh backend status reports the exact requested SSID.

- [ ] **Step 4: Run cancel-and-retry**

Enter Change Wi-Fi, cancel/back out after robot scan begins, then re-enter without pressing BOOT. Verify BLE re-advertises, the scan list returns, and correct credentials recover to Device Home.

- [ ] **Step 5: Run wrong-password recovery**

Submit a deliberately wrong password once. Verify the app does not report success from the previous SSID and the robot returns/remains available for setup. Submit correct credentials afterward and require the full success evidence without pressing BOOT.

- [ ] **Step 6: Observe post-connect stability**

After the final success, observe serial logs for at least 60 seconds. Require no panic, reboot, assertion, repeating `heap_alloc_failed`, liveness loop, or leaked AFE retries; require advancing wake feed/fetch counters and continued heartbeat 204.

### Task 6: Final Review and Integration

**Files:**
- Review all modified firmware/mobile files from this task.

- [ ] **Step 1: Run verification-before-completion checks**

Repeat the full firmware suite/build, mobile suite/typecheck/lint, `git diff --check`, and inspect final hardware evidence before stating the fix is complete.

- [ ] **Step 2: Commit remaining cohesive changes**

```bash
git add main/application.h main/application.cc main/audio/wake_words/afe_wake_word.cc \
  tests/test_audio_rearm_transaction_contract.py \
  tests/test_lesson_sd_sync_worker_contract.py \
  tests/test_realtime_voice_state.py \
  tests/test_wifi_provisioning_brand.py
git commit -m "fix: stabilize wifi reprovision recovery"
```

- [ ] **Step 3: Push only after physical green**

```bash
git push origin main
```

Expected: push succeeds. Report measured test counts and remaining environmental risks; do not claim absolute absence of future bugs.
