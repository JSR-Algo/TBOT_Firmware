# Idle Microphone And WakeNet Telemetry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add privacy-safe aggregate microphone-energy and WakeNet-decision telemetry that identifies why spoken "Hi ESP" is not detected while the AFE pipeline remains live.

**Architecture:** Put allocation-free signal and decision accounting in a small header-only helper that can be exercised by a native host test. `AfeWakeWord` updates this helper only for complete mono chunks submitted to AFE and successful, generation-valid fetches; the existing `WakeWordProgress` snapshot carries interval metrics to the application, where the existing ten-second log publishes them. Snapshot publication atomically drains interval fields but retains monotonic counters and the most recent speech timestamp.

**Tech Stack:** ESP-IDF C++17, fixed-width integers and atomics, ESP-SR AFE/WakeNet enums, FreeRTOS, native C++ host tests, Python source/privacy contracts, pytest, ESP-IDF production build, esptool and serial hardware verification.

---

## File Structure

- Create `main/audio/wake_words/wake_word_telemetry.h`: pure, bounded microphone-energy calculation, WakeNet state categorization, interval snapshot, and reset semantics.
- Create `tests/native/wake_word_telemetry_test.cc`: executable tests for peak/RMS arithmetic, saturation boundaries, decision categories, index validation, and interval draining.
- Modify `scripts/run_host_native_wake_word_lifecycle_test.sh`: compile and run the new dependency-free native telemetry test.
- Modify `main/audio/wake_word.h`: extend the generic progress snapshot with aggregate-only telemetry fields.
- Modify `main/audio/wake_words/afe_wake_word.h`: own telemetry state and expose the enriched snapshot.
- Modify `main/audio/wake_words/afe_wake_word.cc`: observe complete feed chunks and successful generation-valid WakeNet results without altering detection policy.
- Modify `main/application.cc`: append aggregate fields to the existing rate-limited `audio_metrics` line.
- Modify `tests/test_realtime_voice_state.py`: lock placement, log wiring, privacy exclusions, index safety, and unchanged threshold/model policy.
- Modify `tests/test_afe_mono_channel_selection.py`: ensure metrics observe the actual mono chunk sent to AFE.

### Task 1: Build And Test The Allocation-Free Telemetry Primitive

**Files:**
- Create: `main/audio/wake_words/wake_word_telemetry.h`
- Create: `tests/native/wake_word_telemetry_test.cc`
- Modify: `scripts/run_host_native_wake_word_lifecycle_test.sh`

- [ ] **Step 1: Write the failing native test**

Create `tests/native/wake_word_telemetry_test.cc` with a local `Require` helper and cases using these exact inputs:

```cpp
#include "audio/wake_words/wake_word_telemetry.h"

#include <climits>
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Wake telemetry test failed: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    WakeWordTelemetry telemetry;
    const int16_t quiet[] = {0, 0, 0, 0};
    const int16_t speech[] = {-3000, 4000, -3000, 4000};
    const int16_t extremes[] = {INT16_MIN, INT16_MAX};

    telemetry.ObserveFeedChunk(quiet, 4, 100, 1000);
    telemetry.ObserveFeedChunk(speech, 4, 100, 2000);
    telemetry.ObserveFeedChunk(extremes, 2, 100, 3000);
    telemetry.ObserveWakeState(WakeDecisionCategory::kNone, 0, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kTransition, 0, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kDetected, 2, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kDetected, 4, 3);
    telemetry.ObserveWakeState(WakeDecisionCategory::kOther, 0, 3);

    const WakeTelemetrySnapshot first = telemetry.TakeSnapshot();
    Require(first.chunk_count == 3, "counts complete feed chunks");
    Require(first.rms_min == 0 && first.rms_max == 32767,
            "reports bounded interval RMS extrema");
    Require(first.peak_max == 32768, "handles INT16_MIN absolute peak safely");
    Require(first.above_floor_count == 2, "counts chunks above floor");
    Require(first.last_above_floor_us == 3000, "retains latest above-floor timestamp");
    Require(first.state_none == 1 && first.state_transition == 1 &&
                first.state_detected == 2 && first.state_other == 1,
            "categorizes every successful fetch state");
    Require(first.last_valid_model_index == 2,
            "retains only a validated non-idle model index");
    Require(first.invalid_model_index_count == 1,
            "counts invalid non-idle model indices");

    const WakeTelemetrySnapshot drained = telemetry.TakeSnapshot();
    Require(drained.chunk_count == 0 && drained.state_none == 0,
            "drains interval counters at publication");
    Require(drained.above_floor_total == 2 && drained.last_above_floor_us == 3000,
            "retains monotonic speech evidence across publications");
    std::cout << "Wake telemetry test OK\n";
}
```

- [ ] **Step 2: Add the native runner command and verify RED**

Append to `scripts/run_host_native_wake_word_lifecycle_test.sh`:

```bash
"${CXX:-c++}" -std=c++17 -pthread \
    -I"${ROOT_DIR}/main" \
    "${ROOT_DIR}/tests/native/wake_word_telemetry_test.cc" \
    -o "${BUILD_DIR}/wake_word_telemetry_test"

"${BUILD_DIR}/wake_word_telemetry_test"
```

Run: `scripts/run_host_native_wake_word_lifecycle_test.sh`

Expected: compilation fails because `wake_word_telemetry.h` does not exist.

- [ ] **Step 3: Implement the minimal telemetry helper**

Create `main/audio/wake_words/wake_word_telemetry.h` with:

```cpp
#ifndef WAKE_WORD_TELEMETRY_H
#define WAKE_WORD_TELEMETRY_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

enum class WakeDecisionCategory : uint8_t { kNone, kTransition, kDetected, kOther };

struct WakeTelemetrySnapshot {
    uint32_t chunk_count = 0;
    uint32_t rms_min = 0;
    uint32_t rms_max = 0;
    uint32_t peak_max = 0;
    uint32_t above_floor_count = 0;
    uint32_t above_floor_total = 0;
    int64_t last_above_floor_us = 0;
    uint32_t state_none = 0;
    uint32_t state_transition = 0;
    uint32_t state_detected = 0;
    uint32_t state_other = 0;
    int32_t last_valid_model_index = 0;
    uint32_t invalid_model_index_count = 0;
};

class WakeWordTelemetry {
public:
    void ObserveFeedChunk(const int16_t* samples, size_t count,
                          uint32_t speech_floor_rms, int64_t now_us) {
        if (samples == nullptr || count == 0) return;
        uint64_t sum_squares = 0;
        uint32_t peak = 0;
        for (size_t i = 0; i < count; ++i) {
            const int32_t sample = samples[i];
            const uint32_t magnitude = sample < 0
                ? static_cast<uint32_t>(-sample) : static_cast<uint32_t>(sample);
            peak = std::max(peak, magnitude);
            sum_squares += static_cast<uint64_t>(magnitude) * magnitude;
        }
        const uint32_t rms = static_cast<uint32_t>(
            std::sqrt(static_cast<double>(sum_squares / count)));
        chunk_count_.fetch_add(1, std::memory_order_relaxed);
        UpdateMin(rms_min_, rms);
        UpdateMax(rms_max_, rms);
        UpdateMax(peak_max_, peak);
        if (rms >= speech_floor_rms) {
            above_floor_count_.fetch_add(1, std::memory_order_relaxed);
            above_floor_total_.fetch_add(1, std::memory_order_relaxed);
            last_above_floor_us_.store(now_us, std::memory_order_relaxed);
        }
    }

    void ObserveWakeState(WakeDecisionCategory category, int model_index,
                          int model_count);
    WakeTelemetrySnapshot TakeSnapshot();

private:
    static void UpdateMin(std::atomic<uint32_t>& target, uint32_t value);
    static void UpdateMax(std::atomic<uint32_t>& target, uint32_t value);
    std::atomic<uint32_t> chunk_count_{0};
    std::atomic<uint32_t> rms_min_{std::numeric_limits<uint32_t>::max()};
    std::atomic<uint32_t> rms_max_{0};
    std::atomic<uint32_t> peak_max_{0};
    std::atomic<uint32_t> above_floor_count_{0};
    std::atomic<uint32_t> above_floor_total_{0};
    std::atomic<int64_t> last_above_floor_us_{0};
    std::atomic<uint32_t> state_none_{0}, state_transition_{0}, state_detected_{0}, state_other_{0};
    std::atomic<int32_t> last_valid_model_index_{0};
    std::atomic<uint32_t> invalid_model_index_count_{0};
};

#endif
```

Define the short inline methods in the same header. `TakeSnapshot()` must use `exchange` for interval fields, translate the untouched RMS-min sentinel to zero, and use `load` for `above_floor_total`, `last_above_floor_us`, and `last_valid_model_index`. `ObserveWakeState()` increments exactly one category; it accepts a model index only for non-idle categories when `model_index >= 1 && model_index <= model_count`, otherwise increments `invalid_model_index_count_`. Use compare/exchange loops for min/max so no mutex or allocation enters the hot path.

- [ ] **Step 4: Run the native test and verify GREEN**

Run: `scripts/run_host_native_wake_word_lifecycle_test.sh`

Expected: `Wake word lifecycle controller test OK`, `AFE run synchronization test OK`, and `Wake telemetry test OK`.

- [ ] **Step 5: Commit**

```bash
git add main/audio/wake_words/wake_word_telemetry.h \
  tests/native/wake_word_telemetry_test.cc \
  scripts/run_host_native_wake_word_lifecycle_test.sh
git commit -m "test: define privacy-safe wake telemetry"
```

### Task 2: Wire Feed And WakeNet Decisions Into The Snapshot

**Files:**
- Modify: `main/audio/wake_word.h`
- Modify: `main/audio/wake_words/afe_wake_word.h`
- Modify: `main/audio/wake_words/afe_wake_word.cc`
- Modify: `tests/test_realtime_voice_state.py`
- Modify: `tests/test_afe_mono_channel_selection.py`

- [ ] **Step 1: Write failing source contracts**

Add tests asserting all of the following exact contracts:

```python
assert "WakeTelemetrySnapshot telemetry" in read("main/audio/wake_word.h")
assert "WakeWordTelemetry telemetry_;" in read("main/audio/wake_words/afe_wake_word.h")
assert "kDiagnosticSpeechFloorRms" in read("main/audio/wake_words/afe_wake_word.cc")

feed = function_body(afe_cc, "void AfeWakeWord::Feed")
assert "telemetry_.ObserveFeedChunk(input_buffer_.data(), chunk_size," in feed
assert feed.index("telemetry_.ObserveFeedChunk") < feed.index("afe_iface_->feed")
assert "esp_timer_get_time()" in feed

task = function_body(afe_cc, "void AfeWakeWord::AudioDetectionTask")
assert task.index("fetch_count_.fetch_add") < task.index("telemetry_.ObserveWakeState")
assert "WAKENET_DETECTED" in task
assert "WAKENET_CHANNEL_VERIFIED" in task
assert "WakeDecisionCategory::kOther" in task
assert "res->wakenet_model_index >= 1" in task
assert "res->wakenet_model_index <= static_cast<int>(wake_words_.size())" in task
```

Also assert in `tests/test_afe_mono_channel_selection.py` that observation reads `input_buffer_.data()` after optional downmix and immediately before the same buffer is fed.

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```bash
python3 -m pytest \
  tests/test_realtime_voice_state.py \
  tests/test_afe_mono_channel_selection.py -q
```

Expected: new telemetry wiring assertions fail.

- [ ] **Step 3: Extend the generic progress snapshot**

Include `audio/wake_words/wake_word_telemetry.h` from `main/audio/wake_word.h` and add:

```cpp
    WakeTelemetrySnapshot telemetry{};
```

Add `WakeWordTelemetry telemetry_;` to `AfeWakeWord`. Update `GetProgress()` to return the existing three monotonic lifecycle counters plus `telemetry_.TakeSnapshot()`. Remove `const` from the virtual and override `GetProgress()` signatures because interval publication is a drain operation.

- [ ] **Step 4: Observe only complete chunks actually submitted to AFE**

Add a diagnostic constant without changing `kHiEspWakeThreshold`:

```cpp
constexpr uint32_t kDiagnosticSpeechFloorRms = 100;
```

Inside the existing complete-chunk loop, immediately before `afe_iface_->feed`, add:

```cpp
telemetry_.ObserveFeedChunk(input_buffer_.data(), chunk_size,
                            kDiagnosticSpeechFloorRms, esp_timer_get_time());
```

This uses the post-downmix `input_buffer_`, performs no allocation, and does not change microphone ownership or the submitted samples.

- [ ] **Step 5: Categorize successful generation-valid WakeNet results**

After lifecycle validation and `fetch_count_` increment, map the SDK state with a `switch`. Map `WAKENET_NO_DETECT` to `kNone`, `WAKENET_CHANNEL_VERIFIED` to `kTransition`, `WAKENET_DETECTED` to `kDetected`, and `default` to `kOther`. Pass the returned model index and `wake_words_.size()` to `ObserveWakeState`.

Before indexing `wake_words_` in the existing detection branch, require:

```cpp
if (res->wakenet_model_index < 1 ||
    res->wakenet_model_index > static_cast<int>(wake_words_.size())) {
    ESP_LOGW(TAG, "Wake detection returned invalid model index=%d count=%u",
             res->wakenet_model_index, static_cast<unsigned>(wake_words_.size()));
    continue;
}
```

Unknown states and invalid indices must not invoke `Stop()` or the wake callback. Do not change AFE mode, threshold, model selection, VAD values, or callback flow for a valid detection.

- [ ] **Step 6: Run focused native and Python tests**

Run:

```bash
scripts/run_host_native_wake_word_lifecycle_test.sh
python3 -m pytest \
  tests/test_realtime_voice_state.py \
  tests/test_afe_mono_channel_selection.py -q
```

Expected: all selected tests pass.

- [ ] **Step 7: Commit**

```bash
git add main/audio/wake_word.h main/audio/wake_words/afe_wake_word.h \
  main/audio/wake_words/afe_wake_word.cc tests/test_realtime_voice_state.py \
  tests/test_afe_mono_channel_selection.py
git commit -m "feat: collect idle wake signal telemetry"
```

### Task 3: Publish Aggregate Metrics And Lock Privacy Boundaries

**Files:**
- Modify: `main/application.cc`
- Modify: `tests/test_realtime_voice_state.py`

- [ ] **Step 1: Write the failing publication and privacy contract**

Add a test that extracts only the `audio_metrics` statement and requires these fields:

```python
for field in (
    "wake_chunks=%lu", "wake_rms_min=%lu", "wake_rms_max=%lu",
    "wake_peak_max=%lu", "wake_above_floor=%lu", "wake_above_total=%lu",
    "wake_last_above_us=%lld", "wn_none=%lu", "wn_transition=%lu",
    "wn_detected=%lu", "wn_other=%lu", "wn_model=%ld", "wn_bad_model=%lu",
):
    assert field in metrics

for forbidden in ("pcm", "sample=", "transcript", "phrase", "spectrum", "audio_buffer"):
    assert forbidden not in metrics.lower()
```

Also assert the telemetry source/header contain no `std::vector`, `std::string`, file I/O, socket/API calls, or logging statements, and that the application obtains one `GetWakeWordProgress()` snapshot before logging.

- [ ] **Step 2: Run the publication test and verify RED**

Run: `python3 -m pytest tests/test_realtime_voice_state.py -q`

Expected: the new `audio_metrics` fields are absent.

- [ ] **Step 3: Extend the existing rate-limited log**

Append the exact aggregate fields from Step 1 to the current ten-second `audio_metrics` format string. Supply values from `wake_progress.telemetry`, casting 32-bit values to `unsigned long`, the timestamp to `long long`, and the model index to `long`. Do not create another timer, log site, transport, payload, or persistent store.

- [ ] **Step 4: Verify privacy and interval semantics**

Run:

```bash
python3 -m pytest tests/test_realtime_voice_state.py -q
scripts/run_host_native_wake_word_lifecycle_test.sh
git diff --check
```

Expected: all tests pass and `git diff --check` prints nothing.

- [ ] **Step 5: Commit**

```bash
git add main/application.cc tests/test_realtime_voice_state.py
git commit -m "feat: publish aggregate WakeNet diagnostics"
```

### Task 4: Regression, Production Build, Artifact Review, And Hardware Boundary Test

**Files:**
- Modify only if verification exposes a defect in the scoped telemetry implementation.
- Evidence: `/Users/manhhodinh/Documents/TBOT/.codex_tmp/idle-wake-telemetry-20260825/`

- [ ] **Step 1: Run the complete automated suite**

Run:

```bash
scripts/run_host_native_wake_word_lifecycle_test.sh
python3 -m pytest -q
```

Expected: native lifecycle/synchronization/telemetry tests pass; Python suite has no failures (the existing environment-dependent skip is acceptable).

- [ ] **Step 2: Build production firmware from a clean build directory**

Use the repository's documented ESP-IDF environment and production configuration to build into `build-production-idle-wake-telemetry`. Record the app SHA-256, partition usage/headroom, compiled OTA host, compiled WebSocket host, and confirmation that the local course endpoint remains disabled.

Expected: build succeeds, production endpoints match `esp.tjbot.vn:443`, and the app fits its partition.

- [ ] **Step 3: Review generated artifacts before flashing**

Verify the flash manifest identifies exactly the generated bootloader, partition table, OTA data, model, and application regions. Compare pre-flash NVS semantically and confirm no generated write targets `0x9000`. Check `lsof /dev/cu.usbmodem1101`; if any unrelated process owns the port, stop and report it without killing the process.

- [ ] **Step 4: Flash while preserving NVS and verify hashes**

Flash only the five reviewed regions. Read each region back and compare its hash to the generated artifact. Re-read NVS, validate CRC/structure, and report semantic differences; `websocket/token` rotation is acceptable, but other unexpected changes stop acceptance.

- [ ] **Step 5: Run the adult-operated spoken-only window**

After boot reaches claimed idle and the final lesson quiet interval ends, capture at least one silent metrics interval, then speak "Hi ESP" naturally several times over 120 seconds without pressing the robot button. Do not record PCM or retain raw audio. Require feed/fetch progression and stable running generation throughout.

Interpret the aggregate evidence exactly as follows:

- near-silent RMS/peak and no above-floor growth: microphone channel, gain, wiring, or idle codec configuration boundary;
- clear RMS/peak and above-floor growth with only `wn_none`: model asset, selected model, or pronunciation compatibility boundary;
- `wn_transition` grows without `wn_detected`: verification/threshold configuration boundary;
- `wn_detected` grows: verify the existing callback, listening, and speaking flow end-to-end.

- [ ] **Step 6: Final verification and commit discipline**

Run `git status --short`, `git diff --check`, the focused native runner, and the focused Python telemetry tests once more. Expected: the worktree contains only intentional evidence-excluded files or is clean, all checks pass, and no threshold/model/config change was introduced without a new evidence-backed task.

