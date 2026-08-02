# Lesson Non-Hardware Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the complete non-hardware lesson workflow to green, including the existing firmware host line-coverage gate at 100%, without weakening validation or claiming physical-robot results.

**Architecture:** Extend the existing host-native harness first, using current display, scheduler, renderer, and storage fakes for reachable paths. Add only compile-time host coverage controls for deterministic cJSON and nonce failures, then remove duplicated or unreachable reservation/handoff code after characterization tests freeze observable behavior. Finish with cross-repository lesson regression and production firmware size comparison.

**Tech Stack:** C++17, cJSON, ESP-IDF 5.5, clang/LLVM coverage, gcovr, Python/pytest, Node/Vitest, Vue CLI.

---

## File Map

- `main/lesson_handler.cc`: lesson protocol orchestration, host-only fail controls, and removal of proven duplicate/dead paths.
- `main/lesson_handler.h`: declarations for controls available only to host coverage builds.
- `tests/native/lesson_handler_host_test.cc`: behavior and failure-path characterization tests.
- `scripts/run_host_native_lesson_coverage.sh`: coverage build inputs and unchanged `--fail-under-line 100` gate.
- `tests/test_lesson_sample_download_guard_contract.py`: source-shape regression enforcing a single lesson reservation owner in `HandleLessonMessage`.
- `docs/superpowers/specs/2026-08-02-lesson-non-hardware-completion-design.md`: approved design contract.
- Backend, admin, and ESP server repositories: verification only; preserve their current reviewed changes.

## Task 1: Freeze The Coverage Baseline

**Files:**
- Inspect: `main/lesson_handler.cc`
- Inspect: `tests/native/lesson_handler_host_test.cc`
- Inspect: `scripts/run_host_native_lesson_coverage.sh`

- [ ] **Step 1: Run the exact baseline gate into a persistent build directory**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
export TBOT_HOST_NATIVE_COVERAGE_BUILD_DIR=/tmp/tbot-lesson-coverage-baseline
./scripts/run_host_native_lesson_coverage.sh --txt --print-summary
```

Expected: host test reports `1747 checks`, coverage exits nonzero at `93.6%`, and the missing-line list matches the approved design investigation.

- [ ] **Step 2: Save the machine-readable baseline**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
export TBOT_HOST_NATIVE_COVERAGE_BUILD_DIR=/tmp/tbot-lesson-coverage-baseline
./scripts/run_host_native_lesson_coverage.sh --json /tmp/tbot-lesson-coverage-baseline.json || test $? -eq 2
```

Expected: `/tmp/tbot-lesson-coverage-baseline.json` exists and the command fails only because line coverage is below 100%.

## Task 2: Cover Reachable Validation And Visual Completion Paths

**Files:**
- Modify: `tests/native/lesson_handler_host_test.cc`
- Test: `tests/native/lesson_handler_host_test.cc`

- [ ] **Step 1: Add failing characterization tests for reachable validators**

Add a function alongside the existing renderer-v2 tests:

```cpp
void test_renderer_v2_validation_alternatives_and_completion_mapping() {
    for (const char* layout : {"leftApproach", "rightApproach"}) {
        ResetObservable();
        FreshSession();
        LvglDisplay display;
        Board::GetInstance().display_ = &display;
        Handle(V2PrepareFrame(1));
        Handle(V2StartFrame(
            2,
            std::string("{\"preset\":\"flyLandWalkGreet\",\"policy\":") +
                "\"oncePerLessonSession\",\"layoutPreset\":\"" + layout +
                "\",\"backgroundAssetKey\":\"bg\",\"robotAssetKey\":\"robot\"," +
                "\"fallback\":\"staticGreet\"}"));
        require(FrameType(Sent().size() - 1) == "lesson_ack",
                "alternate supported opening layouts are accepted");
    }

    ResetObservable();
    FreshSession();
    Handle(ReplaceOnce(V2VisualFrame(1, "thinking", 1),
                       "\"visualGeneration\":1", "\"visualGeneration\":0"));
    require(FrameType(0) == "lesson_error",
            "non-positive visualGeneration fails closed");
}
```

Register the function in `main()` with the other renderer-v2 test calls.

- [ ] **Step 2: Run the host test and confirm RED through the coverage gate**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_coverage.sh --txt --print-summary
```

Expected: all assertions pass, coverage remains below 100%, but lines `332`, `353-354`, `358`, and `375` disappear from the missing list.

- [ ] **Step 3: Add rejected and phase-timeout LVGL callback cases**

Extend `test_renderer_v2_production_render_callback_reaches_worker_ack()` after the
existing visual-state loop with:

```cpp
Handle(V2VisualFrame(visual_sequence++, "retry", visual_generation++));
display.CompleteVisualState(LessonVisualApplyResult::kRejected, "unsupportedContract");
require(App().lesson_visual_queue.front().completion_result ==
            LessonVisualCompletionResult::kRejected,
        "rejected LVGL completion keeps its typed result");
App().DrainLessonVisualQueue();
require(!FrameBodyBool(Sent().size() - 1, "accepted", true) &&
            FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") ==
                "unsupportedContract",
        "rejected LVGL completion emits a stable negative ACK");

Handle(V2VisualFrame(visual_sequence++, "celebrate", visual_generation++));
display.CompleteVisualState(LessonVisualApplyResult::kPhaseTimeout, "phaseTimeout");
require(App().lesson_visual_queue.front().completion_result ==
            LessonVisualCompletionResult::kPhaseTimeout,
        "timed-out LVGL completion keeps its typed result");
App().DrainLessonVisualQueue();
require(FrameBodyBool(Sent().size() - 1, "accepted", false) &&
            FrameBodyBool(Sent().size() - 1, "degraded", false) &&
            FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") == "phaseTimeout",
        "timed-out LVGL completion emits a stable degraded ACK");
```

- [ ] **Step 4: Verify the focused host runner**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_handler_test.sh
```

Expected: `lesson host test OK` with a check count greater than 1747.

- [ ] **Step 5: Commit the reachable validation tests**

```bash
git add tests/native/lesson_handler_host_test.cc
git commit -m "test(firmware): cover lesson validation alternatives"
```

## Task 3: Cover Storage, ACK Replay, And Abandonment

**Files:**
- Modify: `tests/native/lesson_handler_host_test.cc`
- Test: `tests/native/lesson_handler_host_test.cc`

- [ ] **Step 1: Add storage abandonment cases**

Add a test that prepares a lesson, invokes `Application::GetInstance().AbandonLessonStorageSession(...)` for no-session, mismatched, and active-session identities, then checks mutation ownership:

```cpp
void test_abandon_lesson_storage_session_releases_only_the_matching_owner() {
    ResetObservable();
    require(!App().AbandonLessonStorageSession(),
            "abandon without an owned lesson generation is a no-op");

    FreshSession();
    Handle(PrepareFrame(1));
    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    require(!App().AbandonLessonStorageSession(),
            "abandon reports failure when coordinator ownership was already lost");

    FreshSession();
    Handle(PrepareFrame(1));
    require(App().AbandonLessonStorageSession(),
            "abandon releases the matching active lesson owner");
    auto released = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("released");
    require(static_cast<bool>(released), "matching abandon releases the lesson owner");
}
```

- [ ] **Step 2: Add ACK replay-window and pending-visual duplicate tests**

Drive more than `LessonSession::kAckReplayWindow` accepted frames, resend the prepare sequence, and assert its cached `assetPack` is replayed. While an LVGL callback is deferred, resend the pending visual sequence and assert no second render is scheduled and the eventual ACK body is identical.

- [ ] **Step 3: Run coverage and verify the intended clusters disappear**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_coverage.sh --txt --print-summary
```

Expected: lines `1448`, `1474-1495`, `2351`, `2353-2354`, and `2375-2377` are covered.

- [ ] **Step 4: Commit storage and replay coverage**

```bash
git add tests/native/lesson_handler_host_test.cc
git commit -m "test(firmware): cover lesson storage and ack replay"
```

## Task 4: Cover Cinematic And Renderer-V2 Async Paths

**Files:**
- Modify: `tests/native/lesson_handler_host_test.cc`
- Test: `tests/native/lesson_handler_host_test.cc`

- [ ] **Step 1: Add cinematic phase and error mapping cases**

Use `V3RendererFake` and both active renderer setters to cover unsupported command type, all supported phase IDs, renderer open/decode/present errors, reservation conflicts, and rejected-prepare cleanup. Assert exact stable error codes such as `CINEMATIC_PARSER_FAILED`, `CINEMATIC_FILE_READ_FAILED`, `CINEMATIC_DECODE_FAILED`, and `CINEMATIC_PRESENT_FAILED`.

- [ ] **Step 2: Add renderer-v2 opening and async callback cases**

Use the existing verified-asset fixture helpers and `LvglDisplay` fake to cover:

```cpp
// Supported verified opening assets are moved into the persistent layers.
require(display.background_calls.size() == 1 && display.background_calls.back(),
        "verified opening background is installed");
require(display.overlay_calls.size() == 1 && display.overlay_calls.back(),
        "verified opening robot overlay is installed");

// Replacing the session before the deferred callback runs makes it stale.
Application::GetInstance().FlushScheduledCallbacks();
require(Sent().empty(), "stale visual callback cannot claim the replacement session");

// A plain Display has no LVGL overlay and must report deterministic degradation.
require(FrameBodyStr(Sent().size() - 1, nullptr, "degradedReason") == "missingOverlay",
        "non-LVGL visual completion degrades with the stable reason");
```

Also cover repeated renderer-v2 start, asset identity mismatch, and renderer-v2 resume restoring the teach state.

- [ ] **Step 3: Run the focused host and coverage gates**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_handler_test.sh
./scripts/run_host_native_lesson_coverage.sh --txt --print-summary
```

Expected: the host test passes and the reachable cinematic/V2 clusters are removed from the missing list.

- [ ] **Step 4: Commit cinematic and async coverage**

```bash
git add tests/native/lesson_handler_host_test.cc
git commit -m "test(firmware): cover cinematic and async lesson paths"
```

## Task 5: Add Deterministic Host-Only JSON Failure Controls

**Files:**
- Modify: `main/lesson_handler.h`
- Modify: `main/lesson_handler.cc`
- Modify: `tests/native/lesson_handler_host_test.cc`
- Test: `tests/native/lesson_handler_host_test.cc`

- [ ] **Step 1: Write the failing generic JSON cleanup test**

Add a loop that sets a generic lesson JSON fail counter before ordinary v1/v2 ACK and error construction, calls `Handle(...)`, and requires that no partial frame is sent and the session remains recoverable after resetting the counter.

```cpp
tbot::SetLessonJsonFailAfterForTest(0);
Handle(PrepareFrame(1));
require(Sent().empty(), "ordinary prepare ACK OOM sends no partial frame");
tbot::SetLessonJsonFailAfterForTest(-1);
Handle(PrepareFrame(1));
require(FrameType(0) == "lesson_ack", "session recovers after JSON failpoint reset");
```

- [ ] **Step 2: Run the focused test and verify RED**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_handler_test.sh
```

Expected: compilation fails because `SetLessonJsonFailAfterForTest` is not declared.

- [ ] **Step 3: Add the host-only declaration**

In `main/lesson_handler.h`:

```cpp
#ifdef TBOT_HOST_NATIVE_COVERAGE
namespace tbot {
void SetLessonJsonFailAfterForTest(int successful_operations_before_failure);
}
#endif
```

- [ ] **Step 4: Add minimal host-only wrappers**

In `main/lesson_handler.cc`, define a counter only under `TBOT_HOST_NATIVE_COVERAGE`; production wrappers call cJSON directly:

```cpp
#ifdef TBOT_HOST_NATIVE_COVERAGE
std::atomic<int> g_lesson_json_fail_after{-1};
bool LessonJsonOperationAllowed() {
    int remaining = g_lesson_json_fail_after.load(std::memory_order_relaxed);
    while (remaining >= 0) {
        if (remaining == 0) return false;
        if (g_lesson_json_fail_after.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed)) return true;
    }
    return true;
}
cJSON* LessonJsonCreateObject() {
    return LessonJsonOperationAllowed() ? cJSON_CreateObject() : nullptr;
}
char* LessonJsonPrintUnformatted(const cJSON* value) {
    return LessonJsonOperationAllowed() ? cJSON_PrintUnformatted(value) : nullptr;
}
bool LessonJsonAddString(cJSON* object, const char* key, const char* value) {
    return LessonJsonOperationAllowed() &&
           cJSON_AddStringToObject(object, key, value) != nullptr;
}
bool LessonJsonAddNumber(cJSON* object, const char* key, double value) {
    return LessonJsonOperationAllowed() &&
           cJSON_AddNumberToObject(object, key, value) != nullptr;
}
bool LessonJsonAddBool(cJSON* object, const char* key, bool value) {
    return LessonJsonOperationAllowed() &&
           cJSON_AddBoolToObject(object, key, value) != nullptr;
}
bool LessonJsonAddItem(cJSON* object, const char* key, cJSON* value) {
    return LessonJsonOperationAllowed() && cJSON_AddItemToObject(object, key, value);
}
#else
#define LessonJsonCreateObject cJSON_CreateObject
#define LessonJsonPrintUnformatted cJSON_PrintUnformatted
#define LessonJsonAddString(object, key, value) \
    (cJSON_AddStringToObject((object), (key), (value)) != nullptr)
#define LessonJsonAddNumber(object, key, value) \
    (cJSON_AddNumberToObject((object), (key), (value)) != nullptr)
#define LessonJsonAddBool(object, key, value) \
    (cJSON_AddBoolToObject((object), (key), (value)) != nullptr)
#define LessonJsonAddItem(object, key, value) \
    cJSON_AddItemToObject((object), (key), (value))
#endif

#ifdef TBOT_HOST_NATIVE_COVERAGE
namespace tbot {
void SetLessonJsonFailAfterForTest(int successful_operations_before_failure) {
    g_lesson_json_fail_after.store(successful_operations_before_failure,
                                   std::memory_order_relaxed);
}
}
#endif
```

Replace the matching direct cJSON operations in `BuildFrame`, `MakeErrorBody`, and
visual-completion ACK construction with these wrapper names. Do not change cinematic
wrappers in this task.

- [ ] **Step 5: Run RED/GREEN verification and production compile**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_handler_test.sh
export PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py build
```

Expected: host test passes; ESP-IDF build passes with `TBOT_HOST_NATIVE_COVERAGE` absent.

- [ ] **Step 6: Commit the JSON failure controls**

```bash
git add main/lesson_handler.h main/lesson_handler.cc tests/native/lesson_handler_host_test.cc
git commit -m "test(firmware): expose host-only lesson json failures"
```

## Task 6: Make Defensive Nonce And Enum Paths Deterministic

**Files:**
- Modify: `main/lesson_handler.h`
- Modify: `main/lesson_handler.cc`
- Modify: `tests/native/lesson_handler_host_test.cc`

- [ ] **Step 1: Add a failing nonce-wrap test**

```cpp
tbot::SetLessonVisualCompletionNonceForTest(UINT64_MAX);
ResetObservable();
FreshSession();
Handle(V2VisualFrame(1, "thinking", 1));
require(!App().lesson_visual_queue.empty() &&
            App().lesson_visual_queue.back().visual_nonce != 0,
        "visual completion nonce skips zero after wraparound");
```

- [ ] **Step 2: Verify RED**

Run `./scripts/run_host_native_lesson_handler_test.sh` and expect a compile failure for the missing setter.

- [ ] **Step 3: Add a host-only nonce setter**

Declare and implement under `TBOT_HOST_NATIVE_COVERAGE`:

```cpp
void SetLessonVisualCompletionNonceForTest(std::uint64_t value) {
    g_visual_completion_nonce.store(value, std::memory_order_relaxed);
}
```

- [ ] **Step 4: Cover exhaustive enum fallbacks without changing production semantics**

Drive callback fakes with `static_cast<LessonVisualApplyResult>(0xff)` and cinematic fakes with an invalid `LessonCinematicError` value. Assert both map to fail-closed rejected/error responses.

- [ ] **Step 5: Verify coverage for nonce and enum clusters**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_coverage.sh --txt --print-summary
```

Expected: lines `410-413`, `611-612`, and `1781` are no longer missing.

- [ ] **Step 6: Commit defensive-path coverage**

```bash
git add main/lesson_handler.h main/lesson_handler.cc tests/native/lesson_handler_host_test.cc
git commit -m "test(firmware): cover defensive lesson state paths"
```

## Task 7: Remove The Duplicate Reservation And Unreachable Handoff Cleanup

**Files:**
- Modify: `main/lesson_handler.cc`
- Modify: `tests/native/lesson_handler_host_test.cc`
- Modify: `tests/test_lesson_sample_download_guard_contract.py`

- [ ] **Step 1: Add a source-shape RED test for single reservation ownership**

In `tests/test_lesson_sample_download_guard_contract.py`, extract `HandleLessonMessage` with the existing brace-balanced helper and assert:

```python
assert handler_body.count("TryBeginLessonSession(") == 1
```

- [ ] **Step 2: Run the contract and verify RED**

```bash
python3 -m pytest -q tests/test_lesson_sample_download_guard_contract.py
```

Expected: FAIL because `HandleLessonMessage` currently reserves the same prepare session twice.

- [ ] **Step 3: Remove the second reservation block**

Delete the second `if (is_prepare) { TryBeginLessonSession(...) }` block around the current lines 2261-2300 and replace it with:

```cpp
if (is_prepare) {
    g_session.lesson_asset_generation = prepare_asset_generation;
}
```

Keep the first reservation/error mapping and `release_new_lesson_asset_session` ownership unchanged.

- [ ] **Step 4: Add handoff characterization before deleting unreachable cleanup**

Extend `test_cinematic_cross_renderer_handoff_releases_old_resources()` to assert successful v3-to-v4 and v4-to-v3 handoff, mutation blocking while active, and release after terminal control. Run it before editing the handoff branches and record PASS.

- [ ] **Step 5: Remove only cleanup branches that depend on `DiscardSession()` failing**

Delete the post-discard `kSessionReleaseFailed` branches whose active renderer implementations always reset/release. Retain fail-closed handling for externally returned `kSessionReleaseFailed` errors.

- [ ] **Step 6: Verify focused behavior and coverage**

```bash
python3 -m pytest -q tests/test_lesson_sample_download_guard_contract.py
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_handler_test.sh
./scripts/run_host_native_lesson_coverage.sh --txt --print-summary
```

Expected: contract and host tests pass; duplicate reservation and unreachable handoff lines disappear from the denominator rather than being excluded.

- [ ] **Step 7: Commit dead-path removal**

```bash
git add main/lesson_handler.cc tests/native/lesson_handler_host_test.cc tests/test_lesson_sample_download_guard_contract.py
git commit -m "fix(firmware): remove duplicate lesson reservation paths"
```

## Task 8: Close The Coverage Gate And Prove Zero Production Seam Cost

**Files:**
- Modify: `tests/native/lesson_handler_host_test.cc`
- Verify: `scripts/run_host_native_lesson_coverage.sh`

- [ ] **Step 1: Run coverage and inspect the remaining exact lines**

```bash
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_coverage.sh --txt --print-summary
```

Expected: remaining lines belong to reachable behavior already classified in the approved design. Add one named behavior test per reported cluster, rerun after each test, and stop only when the missing-line list is empty. Do not add source exclusions.

- [ ] **Step 2: Require the unchanged 100% gate**

```bash
rg -n -- '--fail-under-line 100' scripts/run_host_native_lesson_coverage.sh
./scripts/run_host_native_lesson_coverage.sh --print-summary
```

Expected: exit `0`, `lines: 100.0%`, and the threshold remains exactly 100.

- [ ] **Step 3: Compare production section sizes**

Capture the existing build size, rebuild without `TBOT_HOST_NATIVE_COVERAGE`, and compare:

```bash
export PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py size-components > /tmp/tbot-size-after.txt
rg 'lesson_handler|Total image size|Total' /tmp/tbot-size-after.txt
```

Expected: host-only control symbols are absent from `build/xiaozhi.map`; `.data` and `.bss` do not increase because of test counters, and any `.text` change is explained by removal of duplicate production branches.

- [ ] **Step 4: Commit final reachable coverage tests**

```bash
git add tests/native/lesson_handler_host_test.cc
git commit -m "test(firmware): complete lesson host coverage"
```

## Task 9: Run Complete Non-Hardware Lesson Regression

**Files:**
- Verify only: backend, admin/ESP server, firmware, and Servant firmware worktrees.

- [ ] **Step 1: Verify firmware tests and builds**

```bash
export PATH=/Users/manhhodinh/.nvm/versions/node/v20.20.2/bin:$PATH
python3 -m pytest tests -q
export IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2
./scripts/run_host_native_lesson_handler_test.sh
./scripts/run_host_native_lesson_renderer_trace_test.sh \
  /Users/manhhodinh/Documents/TBOT/robot/esp32-server/main/manager-web/tests/fixtures/renderer-v2-manifest.json
./scripts/run_host_native_lesson_coverage.sh --print-summary
export PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin:$PATH
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py build
```

Expected: firmware pytest, host runners, 100% coverage, and build all exit 0.

- [ ] **Step 2: Verify Servant firmware build**

Run `idf.py build` in `/Users/manhhodinh/Documents/TBOT/robot/TBOT-Servant-Firmware` with the same ESP-IDF environment. Expected: exit 0.

- [ ] **Step 3: Verify ESP server full pytest**

```bash
cd /Users/manhhodinh/Documents/TBOT/robot/esp32-server/.worktrees/google-live-tvideo-journey/main/tbot-server
TBOT_BACKEND_REPO=/Users/manhhodinh/Documents/TBOT/tbot-backend \
  /Users/manhhodinh/Documents/TBOT/robot/esp32-server/main/tbot-server/.venv311/bin/python \
  -m pytest -q
```

Expected: at least `3287 passed`, zero failures.

- [ ] **Step 4: Verify backend**

```bash
cd /Users/manhhodinh/Documents/TBOT/.worktrees/backend-google-live-tvideo-journey
export PATH=/Users/manhhodinh/.nvm/versions/node/v20.20.2/bin:$PATH
npm test
npm run lint
npm run typecheck
npm run build
```

Expected: zero failures. Do not touch the untracked `pnpm-lock.yaml` or
`pnpm-workspace.yaml`.

- [ ] **Step 5: Verify admin lesson suites and build**

```bash
cd /Users/manhhodinh/Documents/TBOT/robot/esp32-server/.worktrees/google-live-tvideo-journey/main/manager-web
export PATH=/Users/manhhodinh/.nvm/versions/node/v20.20.2/bin:$PATH
npm run test:course-admin-ui
npm run test:course-robot-e2e-ui
npm run test:tvideo-template
npm run test:tvideo-journey-editor
npm run test:tvideo-journey-browser
npm run test:lesson-editor-ui
npm run test:flattened-cinematic-preview
npm run test:flattened-derivative-status
npm run test:lesson-sd-sync-ui
npm run test:lesson-visual-selection
npm run test:lesson-builder-logic
npm run test:rewards-admin-browser-runner
npm run test:lesson-visual-library-ui
npm run test:lesson-visual-library-browser
npm run test:lesson-builder-browser
npm run test:nest-auth-mode
npm run test:admin-proxy-key-wiring
npm run test:lesson-studio-compose
npm run test:web-cache-policy
npm run build
```

Expected: all commands exit 0. Skip `test:e2e:rewards-admin:live`,
`test:e2e:lesson-studio`, reset runners that mutate service state, and all
robot/BLE/serial suites.

- [ ] **Step 6: Independent review and diff hygiene**

Run `git diff --check` in every changed repository and dispatch independent review for production firmware changes, test strength, coverage integrity, and absence of hardware-result claims.

- [ ] **Step 7: Produce the final software/hardware boundary report**

Report exact pass counts, builds, coverage, binary sizes, changed files, commits, and the deferred robot checklist: flash/boot, TFT layers, audio/microphone, BLE/Wi-Fi claim, physical SD sync, Google Live interaction, RAM/PSRAM, and full on-device lesson playback.
