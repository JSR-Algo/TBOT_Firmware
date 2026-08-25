# AFE Wake Restart Synchronization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the AFE WakeNet fetch loop restart reliably after lesson asset-sync quiet pauses, and expose non-audio feed/fetch progress for hardware verification.

**Architecture:** Extend the WakeWord debug interface with monotonic AFE progress counters, then replace the indefinitely blocking AFE fetch with a bounded, generation-gated loop that acknowledges Stop before buffer reset. Keep application microphone ownership unchanged; existing asset-sync calls remain serialized, while correctness no longer depends on reducing their count.

**Tech Stack:** ESP-IDF C++, FreeRTOS event groups and atomics, Python source-contract tests, pytest, esptool and serial hardware verification.

---

## File Structure

- Modify `main/audio/wake_word.h`: define a generic non-audio wake progress snapshot.
- Modify `main/audio/wake_words/afe_wake_word.h`: hold feed/fetch counters, run generation, and stop acknowledgement state.
- Modify `main/audio/wake_words/afe_wake_word.cc`: implement bounded fetch, progress counters, generation gating, and synchronized Stop/Start.
- Modify `main/audio/audio_service.h` and `main/audio/audio_service.cc`: expose progress without leaking WakeWord ownership.
- Modify `main/application.cc`: append progress to existing periodic audio metrics.
- Modify `tests/test_realtime_voice_state.py`: lock metrics wiring.
- Modify `tests/test_wake_word_lifecycle.py`: lock AFE Stop/Start synchronization and bounded fetch.
- Review `main/mcp_server.cc` and `tests/test_lesson_sd_sync_worker_contract.py`: verify each MCP call already owns one complete quiet interval and no safe multi-asset batch boundary exists.

### Task 1: Add non-audio AFE progress telemetry

**Files:**
- Modify: `main/audio/wake_word.h`
- Modify: `main/audio/wake_words/afe_wake_word.h`
- Modify: `main/audio/wake_words/afe_wake_word.cc`
- Modify: `main/audio/audio_service.h`
- Modify: `main/audio/audio_service.cc`
- Modify: `main/application.cc`
- Modify: `tests/test_realtime_voice_state.py`

- [ ] **Step 1: Write the failing telemetry contract**

Add to `tests/test_realtime_voice_state.py`:

```python
def test_audio_metrics_report_wake_feed_fetch_progress_without_audio_content():
    wake = read("main/audio/wake_word.h")
    afe_h = read("main/audio/wake_words/afe_wake_word.h")
    afe_cc = read("main/audio/wake_words/afe_wake_word.cc")
    service_h = read("main/audio/audio_service.h")
    service_cc = read("main/audio/audio_service.cc")
    app = read("main/application.cc")

    assert "struct WakeWordProgress" in wake
    assert "uint32_t feed_count" in wake
    assert "uint32_t fetch_count" in wake
    assert "uint32_t run_generation" in wake
    assert "virtual WakeWordProgress GetProgress() const" in wake
    assert "std::atomic<uint32_t> feed_count_" in afe_h
    assert "std::atomic<uint32_t> fetch_count_" in afe_h
    assert "feed_count_.fetch_add(1" in afe_cc
    assert "fetch_count_.fetch_add(1" in afe_cc
    assert "WakeWordProgress GetWakeWordProgress();" in service_h
    assert "WakeWordProgress AudioService::GetWakeWordProgress()" in service_cc
    assert "wake_feed=%lu wake_fetch=%lu wake_gen=%lu" in app
    assert "pcm" not in app[app.index("wake_feed=%lu") : app.index("wake_feed=%lu") + 180]
```

- [ ] **Step 2: Run the test and verify RED**

Run: `python3 -m pytest tests/test_realtime_voice_state.py::test_audio_metrics_report_wake_feed_fetch_progress_without_audio_content -q`

Expected: FAIL because `WakeWordProgress` does not exist.

- [ ] **Step 3: Add the generic progress snapshot**

In `main/audio/wake_word.h`, before `class WakeWord`, add:

```cpp
struct WakeWordProgress {
    uint32_t feed_count = 0;
    uint32_t fetch_count = 0;
    uint32_t run_generation = 0;
};
```

Add to `WakeWord`:

```cpp
    virtual WakeWordProgress GetProgress() const { return {}; }
```

- [ ] **Step 4: Count complete AFE feed and successful fetch operations**

In `AfeWakeWord`, add:

```cpp
    WakeWordProgress GetProgress() const override;
    std::atomic<uint32_t> feed_count_{0};
    std::atomic<uint32_t> fetch_count_{0};
    std::atomic<uint32_t> run_generation_{0};
```

Immediately after each complete `afe_iface_->feed(...)`, add:

```cpp
        feed_count_.fetch_add(1, std::memory_order_relaxed);
```

After a non-null fetch with a non-failing return value, add:

```cpp
        fetch_count_.fetch_add(1, std::memory_order_relaxed);
```

Implement:

```cpp
WakeWordProgress AfeWakeWord::GetProgress() const {
    return {
        feed_count_.load(std::memory_order_relaxed),
        fetch_count_.load(std::memory_order_relaxed),
        run_generation_.load(std::memory_order_relaxed),
    };
}
```

- [ ] **Step 5: Expose and log the snapshot**

Add `WakeWordProgress GetWakeWordProgress();` to `AudioService`. Implement it using `TryAcquireAccess()`, `wake_word_control_mutex_`, and `wake_word_->GetProgress()`.

In the periodic metrics branch in `main/application.cc`, capture the snapshot and extend the existing log arguments:

```cpp
const auto wake_progress = audio_service_.GetWakeWordProgress();
```

```text
wake_feed=%lu wake_fetch=%lu wake_gen=%lu
```

Do not log PCM, RMS samples, buffered wake data, or phrase content.

- [ ] **Step 6: Run focused tests and verify GREEN**

Run:

```bash
python3 -m pytest \
  tests/test_realtime_voice_state.py \
  tests/test_wake_word_lifecycle.py -q
```

Expected: all selected tests pass.

- [ ] **Step 7: Commit**

```bash
git add main/audio/wake_word.h main/audio/wake_words/afe_wake_word.h \
  main/audio/wake_words/afe_wake_word.cc main/audio/audio_service.h \
  main/audio/audio_service.cc main/application.cc tests/test_realtime_voice_state.py
git commit -m "feat: report AFE wake progress"
```

### Task 2: Synchronize AFE Stop and Start

**Files:**
- Modify: `main/audio/wake_words/afe_wake_word.h`
- Modify: `main/audio/wake_words/afe_wake_word.cc`
- Modify: `tests/test_wake_word_lifecycle.py`

- [ ] **Step 1: Write failing lifecycle contracts**

Add to `tests/test_wake_word_lifecycle.py`:

```python
def test_afe_fetch_is_bounded_and_stop_acknowledges_before_reset():
    header = read("main/audio/wake_words/afe_wake_word.h")
    source = read("main/audio/wake_words/afe_wake_word.cc")
    stop = function_body(source, "void AfeWakeWord::Stop")
    task = function_body(source, "void AfeWakeWord::AudioDetectionTask")

    assert "DETECTION_STOPPED_EVENT" in source
    assert "kFetchWaitMs" in header
    assert "fetch_with_delay(afe_data_, pdMS_TO_TICKS(kFetchWaitMs))" in task
    assert "xEventGroupSetBits(event_group_, DETECTION_STOPPED_EVENT)" in task
    assert "xEventGroupWaitBits" in stop
    assert stop.index("xEventGroupWaitBits") < stop.index("reset_buffer")
    assert "afe stop acknowledgement timeout" in stop
    assert "xTaskGetCurrentTaskHandle() == audio_detection_task_handle_" in stop


def test_afe_discards_fetch_from_superseded_run_generation():
    source = read("main/audio/wake_words/afe_wake_word.cc")
    start = function_body(source, "void AfeWakeWord::Start")
    task = function_body(source, "void AfeWakeWord::AudioDetectionTask")

    assert "run_generation_.fetch_add(1" in start
    assert "const uint32_t fetch_generation" in task
    assert "fetch_generation != run_generation_.load" in task
    assert task.index("fetch_generation != run_generation_.load") < task.index(
        "StoreWakeWordData"
    )
```

- [ ] **Step 2: Run both tests and verify RED**

Run:

```bash
python3 -m pytest \
  tests/test_wake_word_lifecycle.py::test_afe_fetch_is_bounded_and_stop_acknowledges_before_reset \
  tests/test_wake_word_lifecycle.py::test_afe_discards_fetch_from_superseded_run_generation -q
```

Expected: both FAIL because fetch is unbounded and Stop resets immediately.

- [ ] **Step 3: Add bounded timing and stop acknowledgement state**

In `AfeWakeWord` add:

```cpp
    static constexpr uint32_t kFetchWaitMs = 100;
    static constexpr uint32_t kStopAckTimeoutMs = 500;
```

Add `DETECTION_STOPPED_EVENT` as the next unused event bit. Initialize the detection task in the stopped/acknowledged state.

- [ ] **Step 4: Implement generation-aware Start**

Implement `Start()` as:

```cpp
void AfeWakeWord::Start() {
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    xEventGroupClearBits(event_group_, DETECTION_STOPPED_EVENT);
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}
```

Repeated starts may advance the generation but remain safe and idempotent at the audio lifecycle level.

- [ ] **Step 5: Implement bounded Stop acknowledgement**

Implement the Stop ordering:

```cpp
void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (xTaskGetCurrentTaskHandle() != audio_detection_task_handle_) {
        const EventBits_t stopped = xEventGroupWaitBits(
            event_group_, DETECTION_STOPPED_EVENT, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(kStopAckTimeoutMs));
        if (!(stopped & DETECTION_STOPPED_EVENT)) {
            ESP_LOGW(TAG, "afe stop acknowledgement timeout generation=%lu feed=%lu fetch=%lu",
                     static_cast<unsigned long>(run_generation_.load()),
                     static_cast<unsigned long>(feed_count_.load()),
                     static_cast<unsigned long>(fetch_count_.load()));
            return;
        }
    } else {
        // Detection calls Stop() after a wake hit; it is already outside fetch.
        xEventGroupSetBits(event_group_, DETECTION_STOPPED_EVENT);
    }
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
}
```

- [ ] **Step 6: Implement the bounded fetch loop**

Replace the fetch portion of `AudioDetectionTask()` with this control flow, retaining the existing RMS calculation and callback body inside the final wake-detected branch:

```cpp
        while (!shutting_down_.load() &&
               (xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT)) {
            const uint32_t fetch_generation =
                run_generation_.load(std::memory_order_acquire);
            auto res = afe_iface_->fetch_with_delay(
                afe_data_, pdMS_TO_TICKS(kFetchWaitMs));
            const EventBits_t current_bits = xEventGroupGetBits(event_group_);
            if (shutting_down_.load() ||
                !(current_bits & DETECTION_RUNNING_EVENT) ||
                fetch_generation != run_generation_.load(std::memory_order_acquire)) {
                continue;
            }
            if (res == nullptr || res->ret_value == ESP_FAIL) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            fetch_count_.fetch_add(1, std::memory_order_relaxed);
            StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));
            if (res->wakeup_state == WAKENET_DETECTED) {
                // Keep the existing RMS log, Stop(), detected-word assignment,
                // and wake_word_detected_callback_ invocation here.
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        xEventGroupSetBits(event_group_, DETECTION_STOPPED_EVENT);
```

Set `DETECTION_STOPPED_EVENT` once before entering the outer `while (true)` so the first Stop is already acknowledged. Shutdown must still break the outer loop and set the existing detection-exited event.

- [ ] **Step 7: Run lifecycle and lesson quiet tests**

Run:

```bash
python3 -m pytest \
  tests/test_wake_word_lifecycle.py \
  tests/test_lesson_sd_sync_worker_contract.py \
  tests/test_realtime_voice_state.py -q
```

Expected: all selected tests pass.

- [ ] **Step 8: Commit**

```bash
git add main/audio/wake_words/afe_wake_word.h \
  main/audio/wake_words/afe_wake_word.cc tests/test_wake_word_lifecycle.py
git commit -m "fix: synchronize AFE wake restart"
```

### Task 3: Verify asset-sync quiet boundaries

**Files:**
- Review: `main/mcp_server.cc`
- Modify: `tests/test_lesson_sd_sync_worker_contract.py`

- [ ] **Step 1: Add a contract documenting the actual batch boundary**

Add:

```python
def test_each_asset_sync_worker_owns_one_complete_quiet_interval():
    source = read("main/mcp_server.cc")
    start = function_body(source, "bool McpServer::StartLessonAssetSyncTask")
    worker = function_body(source, "void McpServer::LessonAssetSyncTaskBody")

    assert start.count("BeginLessonAssetSyncQuiet") == 1
    assert "lesson_asset_sync_in_flight_.exchange(true)" in start
    assert worker.count("EndLessonAssetSyncQuiet") == 2
    assert "context->tool->Call(context->arguments)" in worker
    assert worker.index("context->tool->Call(context->arguments)") < worker.index(
        "EndLessonAssetSyncQuiet"
    )
```

This establishes that the current MCP API supplies one asset operation per worker and has no multi-asset batch boundary to coalesce without changing the protocol.

- [ ] **Step 2: Run the contract**

Run: `python3 -m pytest tests/test_lesson_sd_sync_worker_contract.py::test_each_asset_sync_worker_owns_one_complete_quiet_interval -q`

Expected: PASS. If it fails, update the plan before changing production behavior; do not invent a batching protocol.

- [ ] **Step 3: Commit the documented boundary**

```bash
git add tests/test_lesson_sd_sync_worker_contract.py
git commit -m "test: document asset sync quiet boundary"
```

### Task 4: Full verification, production build, and hardware proof

**Files:**
- Generated: `build-production-afe-wake/`
- Create evidence under: `/Users/manhhodinh/Documents/TBOT/.codex_tmp/afe-wake-20260825/`

- [ ] **Step 1: Run the complete firmware test suite**

Run: `python3 -m pytest tests -q`

Expected: all tests pass with only the existing intentional skip.

- [ ] **Step 2: Build a clean production image**

Run:

```bash
export PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py -B build-production-afe-wake \
  -D IDF_TARGET=esp32s3 \
  -D SDKCONFIG=build-production-afe-wake/sdkconfig reconfigure
idf.py -B build-production-afe-wake build
```

Expected: exit 0, production `esp.tjbot.vn` endpoints, local course mode disabled, and `xiaozhi.bin` fits the app partition.

- [ ] **Step 3: Back up NVS and flash generated regions only**

Read `0x9000` size `0x4000`, then flash the five offsets from generated `flash_args`. Never use erase-flash and never write `0x9000`.

- [ ] **Step 4: Validate post-flash NVS semantically**

Compare parsed `(namespace,key)` values. Permit only expected runtime rotation of `websocket/token`; require Wi-Fi, claim identity, and all other keys to match. Require NVS integrity CRC checks to pass.

- [ ] **Step 5: Verify AFE progress after quiet churn**

Capture boot through completion of startup asset sync. Require:

```text
claimed_idle_wake_word_rearmed running=1
wake_feed=<increasing> wake_fetch=<increasing> wake_gen=<stable while running>
```

Feed and fetch must both continue increasing after the final quiet end.

- [ ] **Step 6: Verify spoken-only wake end to end**

With no button input, say "Hi ESP" close to the microphone. Require:

```text
Wake DETECTED
Wake word detected: Hi ESP
StateMachine: State: idle -> connecting
```

or the existing-open-channel equivalent, followed by audio send success and `listening -> speaking`.

- [ ] **Step 7: Record verification**

Save serial logs and hashes in the evidence directory. Confirm the worktree is clean; do not commit generated builds or hardware logs.
