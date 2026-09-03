# Audio Rearm Transaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make audio restoration after BluFi provisioning allocate the 28 KiB Opus worker first, roll back any partial startup, retry once after idle-task reclamation, and report success only when all three base audio workers are alive.

**Architecture:** Add a header-only, host-testable transaction coordinator that fixes worker order and bounds rearm to two attempts without depending on ESP-IDF. `AudioService` remains the owner of FreeRTOS task creation, handles, stop signaling, timer state, and heap telemetry; it supplies callbacks to the coordinator, validates every FreeRTOS result, and publishes a separate healthy-running flag only after the complete handle invariant and timer are proven. Existing provisioning generation ownership stays unchanged and is consumed before the internal retry loop.

**Tech Stack:** C++17, ESP-IDF 5.5.4, FreeRTOS tasks/event groups, ESP heap capabilities, host-native Clang tests with ASan/UBSan/TSan, Python 3/pytest source contracts, LCDWiki ESP32-S3 production build, esptool, Android BluFi application.

---

## File Map

- Create `main/audio/audio_worker_start_transaction.h`: dependency-free worker ordering and bounded rearm coordinator with injected callbacks.
- Create `tests/native/audio_worker_start_transaction_test.cc`: deterministic success, failure-at-each-worker, rollback, retry, exhaustion, and complete-worker-invariant tests.
- Create `scripts/run_host_native_audio_worker_start_transaction_test.sh`: normal, ASan/UBSan, and available TSan host-native lanes.
- Create `tests/test_audio_rearm_transaction_contract.py`: source-level contracts for checked FreeRTOS creation, timer commit ordering, internal-memory diagnostics, reclaim delay, retry bound, and fail-closed return values.
- Modify `main/audio/audio_service.h:129`: change `Start()` to return `bool`, add the transaction include, explicit startup/healthy-state atomics, and private checked-start helpers.
- Modify `main/audio/audio_service.cc:137`: create Opus/input/output through checked helpers, roll back partial startup, publish running state only after all handles exist, and retry rearm once.
- Modify `tests/test_wake_word_lifecycle_contract.py:82`: preserve exact-token ownership while expecting checked rearm rather than a fire-and-forget `Start()`.
- Modify `tests/test_audio_stack_metrics_contract.py:148`: retain the exact 28 KiB internal Opus stack contract after task creation moves into a helper.
- Modify `tests/test_tbot_claim_runtime_contract.py:832`: update the duplicate-start contract for the separate startup-ownership and healthy-running atomics.
- Modify `.github/workflows/build.yml:32`: add the native transaction runner and source contract to the host lifecycle gate.
- Create `docs/qa/ad-hoc/2026-09-03-audio-rearm-transaction.md`: credential-free automated, build, and physical rearm evidence.

### Task 1: Add The RED Host-Native Transaction Tests

**Files:**
- Create: `tests/native/audio_worker_start_transaction_test.cc`
- Create: `scripts/run_host_native_audio_worker_start_transaction_test.sh`

- [ ] **Step 1: Add the deterministic transaction test**

Create `tests/native/audio_worker_start_transaction_test.cc`:

```cpp
#include "audio/audio_worker_start_transaction.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Transaction = AudioWorkerStartTransaction;
using Worker = Transaction::Worker;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "audio worker start transaction test failed: "
                  << message << "\n";
        std::exit(1);
    }
}

const char* Name(Worker worker) {
    switch (worker) {
        case Worker::kOpusCodec: return "opus";
        case Worker::kAudioInput: return "input";
        case Worker::kAudioOutput: return "output";
    }
    return "unknown";
}

void CreatesLargestWorkerFirst() {
    std::vector<std::string> events;
    const bool started = Transaction::StartOnce(
        [&](Worker worker) {
            events.emplace_back(Name(worker));
            return true;
        },
        [&]() { events.emplace_back("rollback"); });

    Require(started, "complete creation succeeds");
    Require(events == std::vector<std::string>({"opus", "input", "output"}),
            "Opus is requested before smaller workers");
}

void FailureAtEachCreationPointRollsBack() {
    for (int fail_index = 0; fail_index < 3; ++fail_index) {
        std::vector<std::string> events;
        int create_index = 0;
        const bool started = Transaction::StartOnce(
            [&](Worker worker) {
                events.emplace_back(Name(worker));
                return create_index++ != fail_index;
            },
            [&]() { events.emplace_back("rollback"); });

        Require(!started, "injected creation failure fails closed");
        Require(events.back() == "rollback",
                "every creation failure invokes rollback");
        Require(static_cast<int>(events.size()) == fail_index + 2,
                "creation stops at the failed worker");
    }
}

void OneFailureRetriesAfterReclaim() {
    std::vector<std::string> events;
    const bool rearmed = Transaction::Rearm(
        [&](uint32_t delay_ms) {
            events.emplace_back("delay:" + std::to_string(delay_ms));
        },
        [&](uint32_t attempt) {
            events.emplace_back("attempt:" + std::to_string(attempt));
            if (attempt == 1) {
                events.emplace_back("cleanup");
                return false;
            }
            events.emplace_back("complete");
            return true;
        });

    Require(rearmed, "second complete attempt succeeds");
    Require(events == std::vector<std::string>({
        "delay:10", "attempt:1", "cleanup", "delay:10",
        "attempt:2", "complete"}),
        "retry occurs only after cleanup and idle reclaim delay");
}

void SecondFailureRemainsStopped() {
    std::vector<std::string> events;
    const bool rearmed = Transaction::Rearm(
        [&](uint32_t delay_ms) {
            events.emplace_back("delay:" + std::to_string(delay_ms));
        },
        [&](uint32_t attempt) {
            events.emplace_back("attempt:" + std::to_string(attempt));
            events.emplace_back("cleanup");
            return false;
        });

    Require(!rearmed, "two failed attempts return false");
    Require(events == std::vector<std::string>({
        "delay:10", "attempt:1", "cleanup", "delay:10",
        "attempt:2", "cleanup"}),
        "rearm is bounded to two attempts");
}

void IncompleteHandleSetCannotReportSuccess() {
    bool opus = false;
    bool input = false;
    bool output = false;
    int attempt_count = 0;

    const bool rearmed = Transaction::Rearm(
        [](uint32_t) {},
        [&](uint32_t) {
            ++attempt_count;
            opus = true;
            input = true;
            output = attempt_count == 2;
            return opus && input && output;
        });

    Require(rearmed, "complete retry reports success");
    Require(attempt_count == 2, "incomplete first set triggers one retry");
    Require(opus && input && output,
            "success is coupled to the complete worker invariant");
}

}  // namespace

int main() {
    CreatesLargestWorkerFirst();
    FailureAtEachCreationPointRollsBack();
    OneFailureRetriesAfterReclaim();
    SecondFailureRemainsStopped();
    IncompleteHandleSetCannotReportSuccess();
    std::cout << "audio worker start transaction test OK\n";
    return 0;
}
```

- [ ] **Step 2: Add the native and sanitizer runner**

Create `scripts/run_host_native_audio_worker_start_transaction_test.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tbot-audio-rearm.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

compiler="${CXX:-$(command -v clang++ || command -v c++)}"
source_file="${repo_root}/tests/native/audio_worker_start_transaction_test.cc"

run_variant() {
  local label="$1"
  shift
  "${compiler}" -std=c++17 -pthread -Wall -Wextra -Werror "$@" \
    -I"${repo_root}/main" "${source_file}" -o "${build_dir}/${label}"
  "${build_dir}/${label}"
}

run_variant normal
run_variant asan_ubsan -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer
if printf '%s\n' 'int main() { return 0; }' | "${compiler}" -x c++ \
    -std=c++17 -pthread -fsanitize=thread - -o "${build_dir}/tsan_probe" \
    >/dev/null 2>&1; then
  run_variant tsan -O1 -g -fsanitize=thread -fno-omit-frame-pointer
fi
```

Make the runner executable:

```bash
chmod +x scripts/run_host_native_audio_worker_start_transaction_test.sh
```

- [ ] **Step 3: Run the native test and verify RED**

Run:

```bash
scripts/run_host_native_audio_worker_start_transaction_test.sh
```

Expected: compilation fails with `audio/audio_worker_start_transaction.h` not found. This is the intended RED state; no production file has changed yet.

- [ ] **Step 4: Commit the RED test and runner**

```bash
git add tests/native/audio_worker_start_transaction_test.cc \
  scripts/run_host_native_audio_worker_start_transaction_test.sh
git commit -m "test(audio): reproduce partial rearm startup"
```

### Task 2: Implement The Dependency-Free Transaction Coordinator

**Files:**
- Create: `main/audio/audio_worker_start_transaction.h`
- Test: `tests/native/audio_worker_start_transaction_test.cc`

- [ ] **Step 1: Add the fixed worker order and bounded retry helper**

Create `main/audio/audio_worker_start_transaction.h`:

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <utility>

class AudioWorkerStartTransaction {
public:
    enum class Worker : uint8_t {
        kOpusCodec,
        kAudioInput,
        kAudioOutput,
    };

    static constexpr uint32_t kIdleReclaimDelayMs = 10;
    static constexpr uint32_t kMaxRearmAttempts = 2;

    template <typename CreateWorker, typename Rollback>
    static bool StartOnce(CreateWorker&& create_worker,
                          Rollback&& rollback) {
        constexpr std::array<Worker, 3> kCreationOrder = {
            Worker::kOpusCodec,
            Worker::kAudioInput,
            Worker::kAudioOutput,
        };
        for (const Worker worker : kCreationOrder) {
            if (!create_worker(worker)) {
                rollback();
                return false;
            }
        }
        return true;
    }

    template <typename Delay, typename StartAttempt>
    static bool Rearm(Delay&& delay, StartAttempt&& start_attempt) {
        delay(kIdleReclaimDelayMs);
        for (uint32_t attempt = 1; attempt <= kMaxRearmAttempts; ++attempt) {
            if (start_attempt(attempt)) {
                return true;
            }
            if (attempt < kMaxRearmAttempts) {
                delay(kIdleReclaimDelayMs);
            }
        }
        return false;
    }
};
```

Keep this seam free of `freertos/*`, `esp_*`, heap allocation, mutexes, and worker handles. It defines order and retry count only; `AudioService` owns all platform cleanup and validation.

- [ ] **Step 2: Run the native test and all sanitizer variants**

Run:

```bash
scripts/run_host_native_audio_worker_start_transaction_test.sh
```

Expected: each supported variant prints `audio worker start transaction test OK` and exits zero. The event assertions prove Opus-first ordering, rollback at all three failure points, exactly one retry after a 10 ms delay, and failure after the second attempt.

- [ ] **Step 3: Inspect the header boundary**

Run:

```bash
rg -n "freertos|esp_|std::function|new |malloc" \
  main/audio/audio_worker_start_transaction.h
git diff --check
```

Expected: `rg` returns no matches and `git diff --check` prints nothing.

- [ ] **Step 4: Commit the transaction coordinator**

```bash
git add main/audio/audio_worker_start_transaction.h
git commit -m "feat(audio): define checked worker start transaction"
```

### Task 3: Add RED Source Contracts For Checked Audio Startup

**Files:**
- Create: `tests/test_audio_rearm_transaction_contract.py`
- Modify: `tests/test_wake_word_lifecycle_contract.py:82`
- Modify: `tests/test_audio_stack_metrics_contract.py:148`
- Modify: `tests/test_tbot_claim_runtime_contract.py:832`

- [ ] **Step 1: Add the checked-start and rollback contracts**

Create `tests/test_audio_rearm_transaction_contract.py`:

```python
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_audio_start_returns_checked_complete_worker_result():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")
    start = function_body(source, "bool AudioService::Start()")
    checked = function_body(source, "bool AudioService::StartWorkers")

    assert "bool Start();" in header
    assert "bool StartWorkers(uint32_t attempt);" in header
    assert "bool HasCompleteWorkerSet();" in header
    assert "return StartWorkers(1);" in start
    assert "AudioWorkerStartTransaction::StartOnce" in checked
    assert "HasCompleteWorkerSet()" in checked
    assert checked.index("HasCompleteWorkerSet()") < checked.index(
        "esp_timer_start_periodic"
    )
    assert checked.index("esp_timer_start_periodic") < checked.index(
        "service_running_.store(true, std::memory_order_release)"
    )
    assert "service_running_.store(true, std::memory_order_release)" in checked
    assert checked.index("HasCompleteWorkerSet()") < checked.index(
        "service_running_.store(true, std::memory_order_release)"
    )


def test_each_audio_worker_creation_is_checked_and_opus_remains_internal():
    source = read("main/audio/audio_service.cc")
    create = function_body(source, "bool AudioService::CreateAudioWorker")

    assert create.count("created == pdPASS && task_handle != nullptr") == 3
    assert create.index("case AudioWorker::kOpusCodec") < create.index(
        "case AudioWorker::kAudioInput"
    )
    assert '"opus_codec", kOpusCodecTaskStackBytes, this' in create
    assert "xTaskCreateWithCaps" not in create
    assert "MALLOC_CAP_SPIRAM" not in create
    assert "kOpusCodecTaskStackBytes / sizeof" not in create


def test_creation_failure_logs_safe_internal_heap_diagnostics():
    source = read("main/audio/audio_service.cc")
    failure = function_body(source, "void AudioService::LogWorkerCreateFailure")

    assert "worker_name" in failure
    assert "created" in failure
    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)" in failure
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)" in failure
    for forbidden in ("ssid", "password", "credential", "audio payload"):
        assert forbidden not in failure.lower()


def test_failed_start_stops_waits_and_leaves_service_stopped():
    source = read("main/audio/audio_service.cc")
    rollback = function_body(source, "bool AudioService::RollbackWorkerStart")

    assert rollback.index("Stop();") < rollback.index(
        "WaitForServiceWorkersStopped(kProvisioningWorkerStopTimeoutMs)"
    )
    assert "service_stopped_.store(true, std::memory_order_release)" in rollback
    assert "service_running_.store(false, std::memory_order_release)" in rollback
    assert "return false;" in rollback


def test_provisioning_rearm_consumes_once_then_retries_once_after_reclaim():
    source = read("main/audio/audio_service.cc")
    rearm = function_body(source, "bool AudioService::EndWifiProvisioningAndRearm")

    lifecycle = rearm.index("wake_word_lifecycle_.EndProvisioningAndRearm(token)")
    consume = rearm.index("provisioning_audio_workers_.Consume(token.generation)")
    retry = rearm.index("AudioWorkerStartTransaction::Rearm")
    assert lifecycle < consume < retry
    assert "vTaskDelay(pdMS_TO_TICKS(delay_ms))" in rearm
    assert "return StartWorkers(attempt);" in rearm
    assert "if (!completion.restart_required)" in rearm
    assert "return true;" in rearm
    assert "return rearmed;" in rearm


def test_ci_runs_audio_rearm_transaction_gates():
    workflow = read(".github/workflows/build.yml")
    assert "scripts/run_host_native_audio_worker_start_transaction_test.sh" in workflow
    assert "tests/test_audio_rearm_transaction_contract.py" in workflow
```

- [ ] **Step 2: Update the existing exact-token source contract**

Replace `test_wifi_provisioning_restarts_only_workers_owned_by_current_token()` in `tests/test_wake_word_lifecycle_contract.py` with:

```python
def test_wifi_provisioning_restarts_only_workers_owned_by_current_token():
    source = read("main/audio/audio_service.cc")
    end = source[source.index("bool AudioService::EndWifiProvisioningAndRearm"):]
    end = end[:end.index("void AudioService::EnableVoiceProcessing")]

    lifecycle = end.index("wake_word_lifecycle_.EndProvisioningAndRearm(token)")
    consume = end.index("provisioning_audio_workers_.Consume(token.generation)")
    retry = end.index("AudioWorkerStartTransaction::Rearm")
    assert lifecycle < consume < retry
    assert "if (!completion.accepted)" in end
    assert "if (!completion.restart_required)" in end
    assert "return StartWorkers(attempt);" in end
```

This keeps the existing stale/duplicate generation boundary explicit: the ledger is consumed once before either internal startup attempt.

- [ ] **Step 3: Keep the stack-budget test pointed at the checked helper**

In `tests/test_audio_stack_metrics_contract.py`, retain all existing stack-budget assertions and append this exact assertion to `test_opus_worker_stack_uses_the_measured_named_budget()`:

```python
assert '"opus_codec", kOpusCodecTaskStackBytes, this' in source
assert "kOpusCodecTaskStackBytes = 28 * 1024" in header
assert "xTaskCreateWithCaps" not in function_body(
    source, "bool AudioService::CreateAudioWorker"
)
```

- [ ] **Step 4: Update the duplicate-start contract for explicit startup ownership**

Replace `test_audio_service_start_is_idempotent_while_workers_are_running()` in `tests/test_tbot_claim_runtime_contract.py` with:

```python
def test_audio_service_start_is_idempotent_while_workers_are_running():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")
    start_workers = function_body(source, "bool AudioService::StartWorkers")

    assert "std::atomic<bool> start_in_progress_{false};" in header
    assert "std::atomic<bool> service_running_{false};" in header
    claim = start_workers.index("start_in_progress_.compare_exchange_strong(")
    running = start_workers.index("if (IsRunning())", claim)
    first_creation = start_workers.index(
        "AudioWorkerStartTransaction::StartOnce", running
    )
    assert claim < running < first_creation
    duplicate = start_workers[running:first_creation]
    assert "start_in_progress_.store(false" in duplicate
    assert "return true;" in duplicate
    assert "std::memory_order_acq_rel" in start_workers
```

- [ ] **Step 5: Run the contracts and verify RED**

Run:

```bash
python3 -m pytest -q \
  tests/test_audio_rearm_transaction_contract.py \
  tests/test_wake_word_lifecycle_contract.py \
  tests/test_audio_stack_metrics_contract.py \
  tests/test_provisioning_success_teardown_contract.py -x
```

Expected: the first new contract fails because `AudioService::Start()` still returns `void`, no checked helpers exist, task results are ignored, the timer starts before worker validation, and rearm still discards the startup result.

- [ ] **Step 6: Commit the RED contracts**

```bash
git add tests/test_audio_rearm_transaction_contract.py \
  tests/test_wake_word_lifecycle_contract.py \
  tests/test_audio_stack_metrics_contract.py \
  tests/test_tbot_claim_runtime_contract.py
git commit -m "test(audio): require transactional provisioning rearm"
```

### Task 4: Make Audio Startup Transactional And Rearm Bounded

**Files:**
- Modify: `main/audio/audio_service.h:129-268`
- Modify: `main/audio/audio_service.cc:1-268`
- Modify: `main/audio/audio_service.cc:846-860`
- Test: `tests/test_audio_rearm_transaction_contract.py`
- Test: `tests/test_wake_word_lifecycle_contract.py`
- Test: `tests/test_audio_stack_metrics_contract.py`

- [ ] **Step 1: Declare checked startup without changing the Opus budget**

Add the transaction include beside the existing audio lifecycle includes in `main/audio/audio_service.h`:

```cpp
#include "audio_worker_start_transaction.h"
```

Inside `AudioService`, add the alias, change the public return type, and declare the helpers:

```cpp
using AudioWorker = AudioWorkerStartTransaction::Worker;

bool Start();

private:
    bool StartWorkers(uint32_t attempt);
    bool CreateAudioWorker(AudioWorker worker);
    bool RollbackWorkerStart();
    bool HasCompleteWorkerSet();
    void LogWorkerCreateFailure(const char* worker_name,
                                BaseType_t created);
```

Keep `service_stopped_` as the worker stop signal, but add distinct atomics beside it so an in-progress or partial start is never observable as a healthy running service:

```cpp
std::atomic<bool> service_stopped_{true};
std::atomic<bool> service_running_{false};
std::atomic<bool> start_in_progress_{false};
```

Change `IsRunning()` to:

```cpp
bool IsRunning() const {
    return service_running_.load(std::memory_order_acquire);
}
```

Do not change:

```cpp
static constexpr uint32_t kOpusCodecTaskStackBytes = 28 * 1024;
```

Existing callers may ignore a returned `bool`; no call-site rewrite is needed unless the compiler exposes a signature assumption.

- [ ] **Step 2: Add safe heap failure diagnostics and the complete-worker check**

Add this include to `main/audio/audio_service.cc`:

```cpp
#include <esp_heap_caps.h>
```

Add:

```cpp
void AudioService::LogWorkerCreateFailure(const char* worker_name,
                                          BaseType_t created) {
    ESP_LOGE(TAG,
             "Audio worker create failed worker=%s result=%ld "
             "internal_free=%u internal_largest=%u",
             worker_name,
             static_cast<long>(created),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

bool AudioService::HasCompleteWorkerSet() {
    std::lock_guard<std::mutex> lock(task_handle_mutex_);
    return opus_codec_task_handle_ != nullptr &&
           audio_input_task_handle_ != nullptr &&
           audio_output_task_handle_ != nullptr;
}
```

These messages contain only worker/result/heap metadata; never add Wi-Fi or audio contents.

- [ ] **Step 3: Move each existing task body into checked creation cases**

Implement `CreateAudioWorker(AudioWorker worker)` as a `switch`. Keep each existing lambda body, core, priority, entry method, and stack byte count unchanged. Use a local result and validate both the return code and protected handle. The Opus case must be:

```cpp
case AudioWorker::kOpusCodec: {
    BaseType_t created;
    {
        std::lock_guard<std::mutex> lock(task_handle_mutex_);
        created = xTaskCreate([](void* arg) {
            AudioService* audio_service = static_cast<AudioService*>(arg);
            audio_service->OpusCodecTask();
            {
                std::lock_guard<std::mutex> handle_lock(
                    audio_service->task_handle_mutex_);
                audio_service->opus_codec_task_handle_ = nullptr;
            }
            vTaskDelete(NULL);
        }, "opus_codec", kOpusCodecTaskStackBytes, this, 2,
        &opus_codec_task_handle_);
        TaskHandle_t task_handle = opus_codec_task_handle_;
        if (created == pdPASS && task_handle != nullptr) {
            return true;
        }
    }
    LogWorkerCreateFailure("opus_codec", created);
    return false;
}
```

The `kAudioInput` case preserves the current task body, stack, priority, and core:

```cpp
case AudioWorker::kAudioInput: {
    BaseType_t created;
    TaskHandle_t task_handle;
    {
        std::lock_guard<std::mutex> lock(task_handle_mutex_);
#if CONFIG_USE_AUDIO_PROCESSOR
        created = xTaskCreatePinnedToCore([](void* arg) {
            AudioService* audio_service = static_cast<AudioService*>(arg);
            audio_service->AudioInputTask();
            {
                std::lock_guard<std::mutex> handle_lock(
                    audio_service->task_handle_mutex_);
                audio_service->audio_input_task_handle_ = nullptr;
            }
            vTaskDelete(NULL);
        }, "audio_input", 2048 * 5, this, 8,
        &audio_input_task_handle_, 0);
#else
        created = xTaskCreate([](void* arg) {
            AudioService* audio_service = static_cast<AudioService*>(arg);
            audio_service->AudioInputTask();
            {
                std::lock_guard<std::mutex> handle_lock(
                    audio_service->task_handle_mutex_);
                audio_service->audio_input_task_handle_ = nullptr;
            }
            vTaskDelete(NULL);
        }, "audio_input", 2048 * 2, this, 8,
        &audio_input_task_handle_);
#endif
        task_handle = audio_input_task_handle_;
        if (created == pdPASS && task_handle != nullptr) {
            return true;
        }
    }
    LogWorkerCreateFailure("audio_input", created);
    return false;
}
```

The `kAudioOutput` case likewise preserves the current task body and budgets:

```cpp
case AudioWorker::kAudioOutput: {
    BaseType_t created;
    TaskHandle_t task_handle;
    {
        std::lock_guard<std::mutex> lock(task_handle_mutex_);
#if CONFIG_USE_AUDIO_PROCESSOR
        created = xTaskCreate([](void* arg) {
            AudioService* audio_service = static_cast<AudioService*>(arg);
            audio_service->AudioOutputTask();
            {
                std::lock_guard<std::mutex> handle_lock(
                    audio_service->task_handle_mutex_);
                audio_service->audio_output_task_handle_ = nullptr;
            }
            vTaskDelete(NULL);
        }, "audio_output", 2048 * 2, this, 4,
        &audio_output_task_handle_);
#else
        created = xTaskCreate([](void* arg) {
            AudioService* audio_service = static_cast<AudioService*>(arg);
            audio_service->AudioOutputTask();
            {
                std::lock_guard<std::mutex> handle_lock(
                    audio_service->task_handle_mutex_);
                audio_service->audio_output_task_handle_ = nullptr;
            }
            vTaskDelete(NULL);
        }, "audio_output", 2048, this, 4,
        &audio_output_task_handle_);
#endif
        task_handle = audio_output_task_handle_;
        if (created == pdPASS && task_handle != nullptr) {
            return true;
        }
    }
    LogWorkerCreateFailure("audio_output", created);
    return false;
}
```

For all three workers, hold `task_handle_mutex_` across creation and copy the corresponding member to a local `TaskHandle_t task_handle` for validation, then call `LogWorkerCreateFailure` after releasing the mutex. Each success condition is exactly:

```cpp
if (created == pdPASS && task_handle != nullptr)
```

where `task_handle` is the local copy of the corresponding protected member handle. Do not introduce PSRAM task creation or reduce any stack.

- [ ] **Step 4: Replace fire-and-forget `Start()` with a checked transaction**

Replace the current `void AudioService::Start()` body with:

```cpp
bool AudioService::Start() {
    return StartWorkers(1);
}

bool AudioService::RollbackWorkerStart() {
    Stop();
    if (!WaitForServiceWorkersStopped(kProvisioningWorkerStopTimeoutMs)) {
        ESP_LOGE(TAG, "Audio worker startup rollback timed out");
    }
    service_stopped_.store(true, std::memory_order_release);
    service_running_.store(false, std::memory_order_release);
    return false;
}

bool AudioService::StartWorkers(uint32_t attempt) {
    bool expected = false;
    if (!start_in_progress_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "Audio service start already in progress");
        return false;
    }
    if (IsRunning()) {
        start_in_progress_.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "Audio service already running; ignoring duplicate start");
        return true;
    }

    ESP_LOGI(TAG, "Audio worker start attempt=%lu",
             static_cast<unsigned long>(attempt));
    service_running_.store(false, std::memory_order_release);
    service_stopped_.store(false, std::memory_order_release);
    xEventGroupClearBits(
        event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    const bool created = AudioWorkerStartTransaction::StartOnce(
        [this](AudioWorker worker) { return CreateAudioWorker(worker); },
        [this]() { RollbackWorkerStart(); });
    if (!created) {
        start_in_progress_.store(false, std::memory_order_release);
        return false;
    }

    const bool complete = HasCompleteWorkerSet();
    ESP_LOGI(TAG,
             "Audio worker start attempt=%lu complete_workers=%d",
             static_cast<unsigned long>(attempt),
             static_cast<int>(complete));
    if (!complete) {
        const bool result = RollbackWorkerStart();
        start_in_progress_.store(false, std::memory_order_release);
        return result;
    }

    if (esp_timer_start_periodic(
            audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "Audio power timer start failed");
        const bool result = RollbackWorkerStart();
        start_in_progress_.store(false, std::memory_order_release);
        return result;
    }
    service_running_.store(true, std::memory_order_release);
    start_in_progress_.store(false, std::memory_order_release);
    return true;
}
```

Update `Stop()` to set `service_running_` false before setting the worker stop signal. During creation `start_in_progress_` owns duplicate-start exclusion and `service_stopped_` is cleared only so worker loops can execute; `IsRunning()` remains false until the three protected handles and timer are valid. Every failed path calls `Stop()`, wakes queue/event waits, waits for handles to clear, and leaves `service_stopped_ == true`, `service_running_ == false`, and no worker handles.

- [ ] **Step 5: Add the pre-attempt reclaim delay and one retry after cleanup**

Replace `EndWifiProvisioningAndRearm()` with:

```cpp
bool AudioService::EndWifiProvisioningAndRearm(
        WifiProvisioningToken token) {
    if (!wake_word_lifecycle_.EndProvisioningAndRearm(token)) {
        return false;
    }
    const auto completion =
        provisioning_audio_workers_.Consume(token.generation);
    if (!completion.accepted) {
        ESP_LOGE(TAG,
                 "Audio provisioning worker completion token was not owned");
        return false;
    }
    if (!completion.restart_required) {
        return true;
    }

    const bool rearmed = AudioWorkerStartTransaction::Rearm(
        [](uint32_t delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        },
        [this](uint32_t attempt) {
            return StartWorkers(attempt);
        });
    ESP_LOGI(TAG, "Audio provisioning rearm complete_workers=%d",
             static_cast<int>(rearmed));
    return rearmed;
}
```

Do not move `Consume()` inside the retry callback. A stale or duplicate external token must still be rejected before any delay or allocation, and a service that was stopped before provisioning must return success without starting workers.

- [ ] **Step 6: Run focused GREEN verification**

Run:

```bash
scripts/run_host_native_audio_worker_start_transaction_test.sh
scripts/run_host_native_wake_word_lifecycle_test.sh
python3 -m pytest -q \
  tests/test_audio_rearm_transaction_contract.py \
  tests/test_wake_word_lifecycle_contract.py \
  tests/test_audio_stack_metrics_contract.py \
  tests/test_provisioning_success_teardown_contract.py \
  tests/test_tbot_claim_runtime_contract.py -x
```

Expected: both native runners exit zero and all focused pytest tests pass. Specifically, stale/duplicate tokens allocate nothing, a previously stopped service stays stopped, Opus remains 28 KiB and internal, and `EndWifiProvisioningAndRearm()` returns `false` after two incomplete attempts.

- [ ] **Step 7: Commit the transactional startup**

```bash
git add main/audio/audio_service.h main/audio/audio_service.cc
git commit -m "fix(audio): make provisioning rearm transactional"
```

### Task 5: Put The New Gates In CI And Run Full Regression

**Files:**
- Modify: `.github/workflows/build.yml:32-48`
- Verify: `tests/`
- Verify: `build/xiaozhi.bin`

- [ ] **Step 1: Add both new gates to the host lifecycle job**

In `.github/workflows/build.yml`, add the runner after the wake-word lifecycle runner:

```yaml
          scripts/run_host_native_wake_word_lifecycle_test.sh
          scripts/run_host_native_audio_worker_start_transaction_test.sh
          scripts/run_host_native_blufi_transition_gate_test.sh
```

Add the source contract to the pytest list:

```yaml
            tests/test_audio_rearm_transaction_contract.py \
            tests/test_wake_word_lifecycle_contract.py \
```

- [ ] **Step 2: Run the focused audio/BluFi/Wi-Fi pytest set**

Run:

```bash
python3 -m pytest -q \
  tests/test_audio*.py \
  tests/test_wake_word*.py \
  tests/test_provisioning_success_teardown_contract.py \
  tests/test_blufi*.py \
  tests/test_wifi*.py
```

Expected: zero failures and only repository-declared skips.

- [ ] **Step 3: Run the native lifecycle, transaction, and Wi-Fi recovery gates**

Run:

```bash
scripts/run_host_native_audio_worker_start_transaction_test.sh
scripts/run_host_native_wake_word_lifecycle_test.sh
scripts/run_host_native_blufi_transition_gate_test.sh
scripts/run_host_native_blufi_wifi_scan_controller_test.sh
scripts/run_host_native_blufi_wifi_scan_lease_timer_test.sh
scripts/run_host_native_wifi_manager_recovery_test.sh
```

Expected: all normal and supported sanitizer lanes exit zero.

- [ ] **Step 4: Run the full firmware suite**

Run:

```bash
python3 -m pytest -q tests
```

Expected: zero failures and only repository-declared skips. Record the pass/skip counts and duration in the QA file later; never copy credentials into it.

- [ ] **Step 5: Build the LCDWiki ESP32-S3 production image**

Run:

```bash
PATH="/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH" \
  ./build-lcdwiki.sh --no-flash
```

Expected: the board guard confirms `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`, ESP-IDF reports `Project build complete`, partition sizing succeeds, and `build/xiaozhi.bin` exists.

- [ ] **Step 6: Inspect scope, whitespace, and artifact identity**

Run:

```bash
git diff --check
git status --short
git log --oneline -8
shasum -a 256 build/xiaozhi.bin
```

Expected: no whitespace errors; only intentional uncommitted QA evidence may remain; commits are limited to this audio rearm work; record the checksum without any network secrets.

- [ ] **Step 7: Commit CI wiring**

```bash
git add .github/workflows/build.yml
git commit -m "ci(audio): gate transactional rearm"
```

### Task 6: Flash And Prove Three Physical Rearms

**Files:**
- Create: `docs/qa/ad-hoc/2026-09-03-audio-rearm-transaction.md`

- [ ] **Step 1: Confirm the exact robot port and free it for flashing**

Run:

```bash
ls -l /dev/cu.usbmodem101
lsof /dev/cu.usbmodem101
```

Expected: `/dev/cu.usbmodem101` exists and no stale monitor holds it. Stop an existing monitor cleanly with Ctrl+] before flashing; do not kill unrelated processes.

- [ ] **Step 2: Flash only the application partition**

Run:

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

Expected: esptool verifies the application image and resets the robot while preserving NVS and assets.

- [ ] **Step 3: Capture a credential-free serial log**

Open a 115200-baud monitor and redact SSIDs, passwords, and credential payload bytes from any saved excerpt. For each successful provisioning teardown require this order:

```text
Audio worker stacks released for WiFi config
Successful provisioning teardown requested
Audio worker start attempt=1
Audio worker start attempt=1 complete_workers=1
Audio provisioning rearm complete_workers=1
Successful provisioning teardown complete: ... rearmed=1
```

After the next periodic metrics sample require non-negative values for all three base workers:

```text
stack_audio_input_min=[non-negative integer]
stack_audio_output_min=[non-negative integer]
stack_opus_codec_min=[non-negative integer]
```

Reject the build if `rearmed=1` appears with any missing base worker, if a failed attempt does not show cleanup before attempt 2, or if a second failed attempt leaves the service reported as running.

- [ ] **Step 4: Run three consecutive Android-to-robot provisioning/rearm cycles**

Use the connected Android device and the robot's advertised `TBOT-...` BLE device. For each cycle: enter automatic BluFi mode without BOOT, scan, select an authorized Wi-Fi network, enter the credential only in the app UI, submit, wait for backend connectivity, and confirm the app reaches its success/device screen.

Expected for all three cycles: no 28 KiB allocation failure, `complete_workers=1`, teardown logs `rearmed=1`, and input/output/Opus stack metrics are present. Do not place network names or passwords in shell commands, saved logs, screenshots intended for commit, or the QA document.

- [ ] **Step 5: Exercise allocation failure recovery if an existing safe injection seam is available**

Use only an already-established non-production test injection mechanism. Force one worker creation failure, verify rollback clears all three handles and reports `rearmed=0` if both attempts fail, then remove the injection and verify a later controlled provisioning cycle starts all workers. If no safe physical injection exists, record this case as covered by the host-native deterministic test and do not add a production fault hook.

- [ ] **Step 6: Confirm Wi-Fi and BluFi behavior did not regress**

During the physical run verify automatic BluFi entry, visible Wi-Fi scan results, exact selected-credential station start, one invalid-password attempt followed by correction, and one BLE interruption followed by rediscovery. Expected: no BOOT press is required, no saved-network auto-connect races the exact credential frame, and failure remains recoverable.

- [ ] **Step 7: Record and commit credential-free QA evidence**

Create `docs/qa/ad-hoc/2026-09-03-audio-rearm-transaction.md` only after the gates finish. Use the title `Audio Rearm Transaction QA` and include these exact sections: `Artifact`, `Automated gates`, `Physical gates`, and `Observed memory and stacks`. Record the actual 40-character firmware commit, actual application SHA-256, pytest pass/skip counts, build result, three physical cycle results, and a three-row table containing the observed largest internal block plus input/output/Opus high-water marks. State explicitly that credential material retained in evidence is `none`; do not write example or dummy values.

```bash
rg -n "TBD|TODO|REPLACE_ME|password|ssid" \
  docs/qa/ad-hoc/2026-09-03-audio-rearm-transaction.md
git add docs/qa/ad-hoc/2026-09-03-audio-rearm-transaction.md
git commit -m "docs(qa): record transactional audio rearm evidence"
```

Expected: the placeholder/secret scan returns no matches before the QA commit.

### Task 7: Final Review Before Integration

**Files:**
- Verify: all files changed since `6a783a6`

- [ ] **Step 1: Review the exact change set**

Run:

```bash
git diff --stat 6a783a6..HEAD
git diff --check 6a783a6..HEAD
git status --short --branch
```

Expected: only the transaction seam, audio service, focused tests/runners, CI workflow, and QA evidence changed; no whitespace errors; worktree is clean.

- [ ] **Step 2: Re-run the release-critical gates from the final commit**

Run:

```bash
scripts/run_host_native_audio_worker_start_transaction_test.sh
scripts/run_host_native_wake_word_lifecycle_test.sh
python3 -m pytest -q tests
PATH="/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH" \
  ./build-lcdwiki.sh --no-flash
```

Expected: all commands exit zero and the rebuilt artifact matches the source commit recorded in QA. If the binary digest changes after the QA record, update the QA artifact section and commit that correction before integration.

- [ ] **Step 3: Request two-stage review**

Request specification review first, covering every constraint and success criterion in `docs/superpowers/specs/2026-09-03-audio-rearm-transaction-design.md`. After spec approval, request code-quality review focused on lock ordering, task-handle races, rollback timeout behavior, timer state, bounded retries, and credential-free logging.

Expected: both reviews approve or all findings are fixed with focused regression reruns and separate commits.

- [ ] **Step 4: Confirm integration readiness**

Run:

```bash
git status --short --branch
git log --oneline --decorate -10
```

Expected: branch `fix/blufi-audio-memory-quiesce` is clean, all implementation/review fixes are committed, and the branch is ready for the coordinated firmware/backend/mobile integration and final `main` physical E2E.
