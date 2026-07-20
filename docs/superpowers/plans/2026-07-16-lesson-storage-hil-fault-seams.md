# Lesson Storage HIL Fault Seams Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a compile-time-isolated, MAC-bound HIL control plane that deterministically exercises lesson-storage failures on the attended ESP32-S3, then proves the same reviewed source is safe in a clean production build and 104-transition lesson soak.

**Architecture:** Firmware adds a default-off fixed-storage controller, allocation-free eviction hooks, sync staging hooks, safe disposable fixtures, and five user-only HIL tools. The ESP server permits those exact raw tools only for one configured live MAC, provides sanitized lab-only timeout handling, and adds a separate evidence orchestrator while keeping the existing Task 14 driver validation-only. HIL and production images are built from the same source commit in separate build directories; HIL evidence is followed by a mandatory production reflash and production-only lesson verification.

**Tech Stack:** C++17, ESP-IDF 5.5.2, FATFS, FreeRTOS, cJSON, native clang++/ASan/UBSan tests, Python 3/aiohttp/Pytest, Docker Compose, serial capture, real ESP32-S3 `lcdwiki-es3c35p` hardware.

---

## Repository Roots

- Firmware: `/Users/manhhodinh/.config/superpowers/worktrees/TBOT-Firmware/production-lesson-studio`
- ESP server: `/Users/manhhodinh/.config/superpowers/worktrees/esp32-server/production-lesson-studio`
- Root evidence harness: `/Users/manhhodinh/Documents/TBOT/robot`

Never edit rewards code, `manager-mobile`, or generated `main/manager-web/output/` in this plan.

## Locked Protocol Decisions

- `CONFIG_TBOT_HIL_STORAGE_FAULTS` defaults off and depends on ESP32-S3 plus `lcdwiki-es3c35p`.
- HIL keys use the existing canonical cache-key grammar and a slug beginning `hil-`.
- The ESP proxy allowlist is default-empty and contains exactly one normalized live MAC when enabled.
- `after_download_bytes` arms with both `byteThreshold` and `declaredAssetBytes`; the matching sync asset must declare the exact same `size` before the arm can be consumed.
- Eviction hooks do no dynamic allocation, JSON construction, exception allocation, or vector mutation after deletion starts.
- A pause uses yielding RTOS delay for 5-60 seconds and requires no resume MCP call.
- The existing Task 14 fault driver remains validation-only. A new HIL orchestrator performs injection.
- HIL tools remain absent from the normal MCP catalog and are invoked only through the authenticated raw internal proxy.
- A 5-75 second timeout override is accepted only for the five HIL tools or the two production trigger tools `self.lesson_assets.evict_cache_key` and `self.lesson_assets.sync_to_sd` when their arguments contain a canonical `hil-*` key and the resolved MAC is allowlisted.
- Raw paths, recursive deletion, generic filesystem mutation, and raw `rm` remain forbidden.

## File and Component Map

### Firmware

- Create `main/lesson_storage_hil_controller.h/.cc`: fixed-size one-shot arm state, validation, decisions, sequence tracking.
- Create `main/lesson_storage_hil_hooks.h/.cc`: allocation-free checkpoint execution, logging, pause, no-space, staging corruption.
- Create `main/lesson_storage_hil_fixture.h/.cc`: canonical `hil-*` fixture stage/inspect/cleanup with fixed sentinels.
- Create `main/lesson_storage_hil_mcp_tools.h/.cc`: five checked-cJSON user-only tools.
- Create `sdkconfig.defaults.hil-storage`: HIL-only config input.
- Create native tests/runners for controller and fixtures.
- Create `tests/test_lesson_storage_hil_contract.py`: static exclusion, ordering, and registration contracts.
- Create `scripts/assert_lesson_storage_hil_artifacts.py`: production/HIL build-manifest and symbol audit.
- Modify Kconfig, CMake, boot/system-info, eviction, download/staging, MCP validation, and existing native runners.

### ESP Server

- Modify `core/api/device_mcp_admin_handler.py`: exact HIL tool set, live-MAC gate, sanitized failures, bounded HIL timeout.
- Modify config loader/defaults/deployment env surfaces for `LESSON_STORAGE_HIL_DEVICE_ALLOWLIST`.
- Create `scripts/lesson_studio_task14_hil_storage.py`: attended HIL orchestrator.
- Create `scripts/lesson_studio_task14_build_identity.py`: dual-build manifest validator.
- Extend Task 14 evidence, soak, log-audit, and focused tests.

### Root Harness

- Modify `scripts/lesson_e2e_live_capture.py`: capture-only mode for raw HIL evidence.
- Modify its tests and synchronize the authoritative live matrix from the ESP worktree.

---

### Task 1: Define the Default-Off HIL Build Boundary

**Files:**
- Modify: `main/Kconfig.projbuild`
- Create: `sdkconfig.defaults.hil-storage`
- Modify: `main/CMakeLists.txt`
- Modify: `scripts/assert_lcdwiki_prod_config.py`
- Create: `scripts/generate_lesson_storage_hil_local_config.py`
- Create: `tests/test_lesson_storage_hil_contract.py`
- Create: `tests/test_lesson_storage_hil_local_config.py`
- Modify: `tests/test_lcdwiki_es3c35p_board.py`

- [ ] **Step 1: Write RED configuration and exclusion tests**

Add assertions equivalent to:

```python
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def test_hil_storage_config_is_default_off_and_board_restricted():
    kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
    block = kconfig[kconfig.index("config TBOT_HIL_STORAGE_FAULTS"):]
    block = block[:block.index("\nconfig ", 1)]
    assert "default n" in block
    assert "depends on IDF_TARGET_ESP32S3 && BOARD_TYPE_LCDWIKI_ES3C35P" in block

def test_production_defaults_never_include_hil_profile():
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS=y" not in (ROOT / "sdkconfig.defaults").read_text()
    assert "sdkconfig.defaults.hil-storage" not in (ROOT / "CMakeLists.txt").read_text()

def test_hil_sources_are_conditionally_compiled():
    cmake = (ROOT / "main/CMakeLists.txt").read_text()
    assert "if(CONFIG_TBOT_HIL_STORAGE_FAULTS)" in cmake
    assert '"lesson_storage_hil_controller.cc"' in cmake
    assert '"lesson_storage_hil_hooks.cc"' in cmake
    assert '"lesson_storage_hil_fixture.cc"' in cmake
    assert '"lesson_storage_hil_mcp_tools.cc"' in cmake

def run_generator(tmp_path, *, ota_url, websocket_url):
    output = tmp_path / "sdkconfig.defaults.hil-local"
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/generate_lesson_storage_hil_local_config.py"),
            "--ota-url", ota_url,
            "--websocket-url", websocket_url,
            "--output", str(output),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

def test_local_config_generator_rejects_unsafe_urls(tmp_path):
    result = run_generator(
        tmp_path,
        ota_url="http://user:pass@192.168.100.209:8003/tbot/ota/",
        websocket_url="ws://192.168.100.209:8000/tbot/v1/",
    )
    assert result.returncode != 0
```

- [ ] **Step 2: Run RED tests**

```bash
python3 -m pytest tests/test_lesson_storage_hil_contract.py \
  tests/test_lesson_storage_hil_local_config.py \
  tests/test_lcdwiki_es3c35p_board.py -q
```

Expected: FAIL because the Kconfig option, HIL defaults, and conditional source block do not exist.

- [ ] **Step 3: Add the minimal build boundary**

Add this Kconfig entry after the board choice:

```kconfig
config TBOT_HIL_STORAGE_FAULTS
    bool "Enable attended lesson-storage HIL fault controls"
    default n
    depends on IDF_TARGET_ESP32S3 && BOARD_TYPE_LCDWIKI_ES3C35P
    help
        Non-production attended HIL controls for disposable hil-* lesson cache keys.
        Never enable this option in production/default sdkconfig chains.
```

Create `sdkconfig.defaults.hil-storage` with exactly:

```text
CONFIG_TBOT_HIL_STORAGE_FAULTS=y
```

Add an initially empty conditional source block to `main/CMakeLists.txt`; later tasks populate the listed files. Extend `assert_lcdwiki_prod_config.py` so any production sdkconfig containing `CONFIG_TBOT_HIL_STORAGE_FAULTS=y` exits nonzero.

Implement the local-config generator with strict `http://` or `https://` OTA
and `ws://` or `wss://` WebSocket URLs. Reject credentials, query, fragment,
control characters, backslashes, and non-LAN hosts for this HIL script. It
writes exactly two escaped Kconfig assignments atomically to the requested
path and never prints URL credentials.

- [ ] **Step 4: Run GREEN tests and diff check**

```bash
python3 -m pytest tests/test_lesson_storage_hil_contract.py \
  tests/test_lesson_storage_hil_local_config.py \
  tests/test_lcdwiki_es3c35p_board.py -q
git diff --check
```

- [ ] **Step 5: Commit Task 1**

```bash
git add main/Kconfig.projbuild main/CMakeLists.txt sdkconfig.defaults.hil-storage \
  scripts/assert_lcdwiki_prod_config.py \
  scripts/generate_lesson_storage_hil_local_config.py \
  tests/test_lesson_storage_hil_contract.py \
  tests/test_lesson_storage_hil_local_config.py \
  tests/test_lcdwiki_es3c35p_board.py
git commit -m "test(firmware): isolate lesson storage hil profile"
```

### Task 2: Implement the Fixed-Storage One-Shot Controller

**Files:**
- Create: `main/lesson_storage_hil_controller.h`
- Create: `main/lesson_storage_hil_controller.cc`
- Create: `tests/native/lesson_storage_hil_controller_host_test.cc`
- Create: `scripts/run_host_native_lesson_storage_hil_controller_test.sh`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write the RED native controller contract**

Define tests around this exact public surface:

```cpp
enum class LessonStorageHilOperation { kEvict, kSync };
enum class LessonStorageHilCheckpoint {
    kBeforeFirstUnlink,
    kAfterUnlinks,
    kBeforeRmdir,
    kBeforeDownloadWrite,
    kAfterDownloadBytes,
    kBeforeChecksumVerify,
    kBeforeCommitRename,
};
enum class LessonStorageHilAction { kFail, kPause, kNoSpace, kCorruptStaging };

struct LessonStorageHilArmRequest {
    std::string cache_key;
    LessonStorageHilOperation operation;
    LessonStorageHilCheckpoint checkpoint;
    LessonStorageHilAction action;
    std::uint32_t threshold;
    std::uint32_t declared_asset_bytes;
    std::uint32_t pause_seconds;
};

struct LessonStorageHilDecision {
    bool matched;
    bool consumed;
    LessonStorageHilAction action;
    std::uint64_t sequence;
    std::uint32_t pause_seconds;
};

enum class LessonStorageHilArmCode {
    kArmed,
    kAlreadyArmed,
    kInvalidCacheKey,
    kInvalidCombination,
    kInvalidThreshold,
    kSequenceExhausted,
};

struct LessonStorageHilArmResult {
    LessonStorageHilArmCode code;
    bool armed;
    std::uint64_t arm_sequence;
};

struct LessonStorageHilStatus {
    bool armed;
    bool reached;
    bool consumed;
    LessonStorageHilOperation operation;
    LessonStorageHilCheckpoint checkpoint;
    LessonStorageHilAction action;
    std::uint32_t threshold;
    std::uint32_t declared_asset_bytes;
    std::uint32_t pause_seconds;
    std::uint64_t arm_sequence;
    std::uint64_t reached_sequence;
    std::uint64_t consumed_sequence;
};

class LessonStorageHilController {
public:
    static LessonStorageHilController& GetInstance();
    LessonStorageHilArmResult Arm(const LessonStorageHilArmRequest& request);
    LessonStorageHilStatus Status() const;
    void Reset();
    std::size_t LimitDownloadRead(
        const char* cache_key,
        std::size_t downloaded,
        std::size_t requested,
        std::size_t declared_asset_bytes
    ) noexcept;
    LessonStorageHilDecision Observe(
        const char* cache_key,
        LessonStorageHilOperation operation,
        LessonStorageHilCheckpoint checkpoint,
        std::uint32_t progress,
        std::uint32_t declared_asset_bytes
    ) noexcept;
};
```

Cover: canonical/non-HIL/embedded-NUL keys; every allowed and forbidden compatibility pair; threshold bounds; pause 5/60 accepted and 4/61 rejected; `declaredAssetBytes` exact-match consumption; foreign key/operation does not consume; one-shot behavior; reset; concurrent arm/observe; nonzero boot-scoped sequence without reuse/wrap.

- [ ] **Step 2: Add the runner and prove RED**

```bash
chmod +x scripts/run_host_native_lesson_storage_hil_controller_test.sh
scripts/run_host_native_lesson_storage_hil_controller_test.sh
```

Expected: compile failure because controller files do not exist.

- [ ] **Step 3: Implement fixed storage and closed validation**

Use `std::array<char, kLessonAssetCacheKeyMaxBytes + 1>` for the armed key and copy it before publishing active state. Use a short-held mutex, but release it before any pause/action execution. `Observe()` is `noexcept`, compares bytes without allocation, increments the sequence before returning a consumed decision, and clears the arm exactly once.

Compatibility must match the approved table; in particular:

```cpp
if (request.checkpoint == LessonStorageHilCheckpoint::kAfterDownloadBytes) {
    valid = request.operation == LessonStorageHilOperation::kSync &&
            request.threshold >= 1 &&
            request.declared_asset_bytes >= request.threshold &&
            request.declared_asset_bytes <= 512 * 1024;
}
```

- [ ] **Step 4: Run controller GREEN and sanitizer build**

```bash
scripts/run_host_native_lesson_storage_hil_controller_test.sh
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  scripts/run_host_native_lesson_storage_hil_controller_test.sh
```

Expected: nonzero check count and no sanitizer report.

- [ ] **Step 5: Commit Task 2**

```bash
git add main/lesson_storage_hil_controller.h main/lesson_storage_hil_controller.cc \
  main/CMakeLists.txt tests/native/lesson_storage_hil_controller_host_test.cc \
  scripts/run_host_native_lesson_storage_hil_controller_test.sh
git commit -m "feat(firmware): add one-shot lesson storage hil controller"
```

### Task 3: Add Allocation-Free Eviction Checkpoints

**Files:**
- Create: `main/lesson_storage_hil_hooks.h`
- Create: `main/lesson_storage_hil_hooks.cc`
- Modify: `main/lesson_asset_cache_evict.cc`
- Modify: `tests/native/lesson_asset_cache_evict_host_test.cc`
- Modify: `scripts/run_host_native_lesson_asset_cache_evict_test.sh`
- Modify: `tests/test_lesson_storage_hil_contract.py`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write RED eviction checkpoint tests**

Add native cases that arm a canonical `hil-task14/...` key and prove:

```cpp
ArmEvict(kBeforeFirstUnlink, kFail, 0);
auto before = EvictLessonAssetCacheKey(kHilKey, false);
Expect(before.code == LessonAssetCacheEvictCode::kUnlinkFailed, "pre-unlink fail");
Expect(before.file_count == 0 && AllFilesPresent(), "pre-unlink zero mutation");

ArmEvict(kAfterUnlinks, kFail, 1);
auto partial = EvictLessonAssetCacheKey(kHilKey, false);
Expect(partial.code == LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired,
       "after-one must be partial");
Expect(partial.file_count == 1, "partial count must be exact");

ArmEvict(kBeforeRmdir, kFail, 0);
auto pre_rmdir = EvictLessonAssetCacheKey(kHilKey, false);
Expect(pre_rmdir.code == LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired,
       "pre-rmdir after deletes must be partial");
```

Also prove a foreign key does not consume the arm, pause invokes a yielding host seam once, the mutation lease releases, and the existing allocation-failure truth tests remain green.

- [ ] **Step 2: Run RED eviction tests**

```bash
scripts/run_host_native_lesson_asset_cache_evict_test.sh
python3 -m pytest tests/test_lesson_storage_hil_contract.py -q
```

Expected: checkpoint assertions fail because no production hook exists.

- [ ] **Step 3: Implement `noexcept` hook execution**

Expose:

```cpp
enum class LessonStorageHilHookOutcome { kContinue, kFail, kNoSpace };

LessonStorageHilHookOutcome RunLessonStorageHilCheckpoint(
    const char* cache_key,
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    std::uint32_t progress,
    std::uint32_t declared_asset_bytes
) noexcept;
```

For ESP pause actions, log `HIL_STORAGE_CHECKPOINT_REACHED`, call `vTaskDelay(pdMS_TO_TICKS(seconds * 1000))`, then log `HIL_STORAGE_CHECKPOINT_CONTINUED`. Host tests inject a yielding callback. Literal fail/no-space outcomes allocate nothing.

Insert hooks only after all path/result allocation:

```cpp
if (index == 0 && RunLessonStorageHilCheckpoint(
        cache_key.c_str(), kEvict, kBeforeFirstUnlink, 0, 0) != kContinue) {
    return FinishMutationResult(std::move(mutation_result), kUnlinkFailed, deleted_count);
}
// unlink succeeds
++deleted_count;
if (RunLessonStorageHilCheckpoint(
        cache_key.c_str(), kEvict, kAfterUnlinks, deleted_count, 0) != kContinue) {
    return FinishMutationResult(std::move(mutation_result), kUnlinkFailed, deleted_count);
}
```

Call the pre-`rmdir` hook immediately before `rmdir()`. Keep all calls under the HIL/host-test compile guard.

- [ ] **Step 4: Run GREEN, ASan/UBSan, and adjacent gates**

```bash
scripts/run_host_native_lesson_asset_cache_evict_test.sh
scripts/run_host_native_lesson_asset_storage_coordinator_test.sh
scripts/run_host_native_lesson_storage_hil_controller_test.sh
python3 -m pytest tests/test_lesson_storage_hil_contract.py \
  tests/test_lesson_asset_cache_evict_contract.py -q
git diff --check
```

- [ ] **Step 5: Commit Task 3**

```bash
git add main/lesson_storage_hil_hooks.h main/lesson_storage_hil_hooks.cc \
  main/lesson_asset_cache_evict.cc main/CMakeLists.txt \
  tests/native/lesson_asset_cache_evict_host_test.cc \
  scripts/run_host_native_lesson_asset_cache_evict_test.sh \
  tests/test_lesson_storage_hil_contract.py
git commit -m "feat(firmware): add deterministic hil eviction checkpoints"
```

### Task 4: Add Sync Write, Checksum, and Commit Checkpoints

**Files:**
- Modify: `main/mcp_server.cc`
- Modify: `main/lesson_asset_download_staging.h`
- Modify: `main/lesson_asset_download_staging.cc`
- Modify: `tests/native/lesson_asset_download_staging_host_test.cc`
- Modify: `scripts/run_host_native_lesson_asset_download_staging_test.sh`
- Modify: `tests/test_lesson_sd_sync_attestation_contract.py`
- Modify: `tests/test_lesson_storage_hil_contract.py`

- [ ] **Step 1: Write RED sync checkpoint tests**

Add tests for:

- before-first-write fail/no-space leaves zero destination bytes and cleans `.tmp`/`.download`;
- exact byte threshold caps the HTTP read so progress equals the threshold;
- missing or mismatched manifest `size` does not consume an after-bytes arm;
- no-space after N bytes removes temporary state and preserves the prior destination;
- corrupt-staging flips one byte before SHA verification and produces checksum mismatch;
- pre-commit-rename fail restores the previous destination on ordinary failure;
- simulated power interruption after backup rename is repaired by the next staging constructor;
- sample sync cannot consume a canonical HIL arm.

- [ ] **Step 2: Run RED staging and contract tests**

```bash
scripts/run_host_native_lesson_asset_download_staging_test.sh
python3 -m pytest tests/test_lesson_sd_sync_attestation_contract.py \
  tests/test_lesson_storage_hil_contract.py -q
```

- [ ] **Step 3: Propagate canonical key and declared size**

Extend validated assets without weakening the production optional-size contract:

```cpp
struct ValidatedLessonAsset {
    const char* key;
    const char* path;
    const char* url;
    const char* sha256;
    const char* destination;
    bool has_declared_size;
    std::size_t declared_size;
};
```

Pass `cache_key`, `has_declared_size`, and `declared_size` through generic sync only. Sample sync passes an explicit empty HIL context and cannot match.

Before each `Read()`, call `LimitDownloadRead()` and cap `want`. Call the write checkpoint before the first `fwrite()` and the after-bytes checkpoint immediately after incrementing `bytes_out`.

Change the staging commit signature to:

```cpp
void CommitVerifiedLessonAssetDownload(
    LessonAssetDownloadStagingFile& staging,
    const char* cache_key,
    const std::string& destination,
    const std::string& expected_sha256
);
```

Run corruption immediately before `VerifyLessonAssetSha256()`. Run pre-commit-rename after moving an old destination to `.backup` and route ordinary injected failure through the existing restore branch.

- [ ] **Step 4: Run GREEN and sanitizer suites**

```bash
scripts/run_host_native_lesson_asset_download_staging_test.sh
scripts/run_host_native_lesson_asset_sync_path_test.sh
IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2 \
  scripts/run_host_native_mcp_cjson_oom_test.sh
python3 -m pytest tests/test_lesson_sd_sync_attestation_contract.py \
  tests/test_lesson_sample_download_guard_contract.py \
  tests/test_lesson_storage_hil_contract.py -q
git diff --check
```

- [ ] **Step 5: Commit Task 4**

```bash
git add main/mcp_server.cc main/lesson_asset_download_staging.h \
  main/lesson_asset_download_staging.cc \
  tests/native/lesson_asset_download_staging_host_test.cc \
  scripts/run_host_native_lesson_asset_download_staging_test.sh \
  tests/test_lesson_sd_sync_attestation_contract.py \
  tests/test_lesson_storage_hil_contract.py
git commit -m "feat(firmware): inject hil sync staging failures safely"
```

### Task 5: Implement Safe HIL Fixtures and Read-Only Fingerprints

**Files:**
- Create: `main/lesson_storage_hil_fixture.h`
- Create: `main/lesson_storage_hil_fixture.cc`
- Create: `tests/native/lesson_storage_hil_fixture_host_test.cc`
- Create: `scripts/run_host_native_lesson_storage_hil_fixture_test.sh`
- Modify: `main/CMakeLists.txt`
- Modify: `tests/test_lesson_storage_hil_contract.py`

- [ ] **Step 1: Write RED fixture safety tests**

Use this API:

```cpp
enum class LessonStorageHilFixture {
    kNestedDirectory,
    kLeafRegularFile,
    kPreservationSet,
};

enum class LessonStorageHilFixtureCode {
    kStaged,
    kCleaned,
    kInspected,
    kInvalidCacheKey,
    kInvalidSibling,
    kLeaseRefused,
    kUnexpectedExistingNode,
    kSentinelMismatch,
    kIoFailed,
};

struct LessonStorageHilFixtureResult {
    LessonStorageHilFixtureCode code;
    bool changed;
    std::string cache_key;
    std::string sibling_cache_key;
};

struct LessonStorageHilInspectionEntry {
    std::string label;
    std::string node_type;
    std::size_t bytes;
    std::string sha256;
};

struct LessonStorageHilInspection {
    std::string cache_key;
    std::string sibling_cache_key;
    bool truncated;
    std::vector<LessonStorageHilInspectionEntry> entries;
};

LessonStorageHilFixtureResult StageLessonStorageHilFixture(
    const LessonAssetMutationLease& mutation,
    const std::string& cache_key,
    LessonStorageHilFixture fixture,
    const std::string& sibling_cache_key
);
LessonStorageHilFixtureResult CleanupLessonStorageHilFixture(...);
LessonStorageHilInspection InspectLessonStorageHilStorage(
    const std::string& cache_key,
    const std::string& sibling_cache_key
);
```

Test canonical `hil-*` enforcement, same-slug/different-version sibling policy, fixed sentinel names/magic, no recursion, refusal on unknown sentinel content, cleanup isolation, root/slug/sibling preservation, and bounded fingerprints for exact leaf, sibling, `current.json`, PVG, and shared locations. Verify no absolute path appears in result strings.

- [ ] **Step 2: Run RED fixture runner**

```bash
chmod +x scripts/run_host_native_lesson_storage_hil_fixture_test.sh
scripts/run_host_native_lesson_storage_hil_fixture_test.sh
```

- [ ] **Step 3: Implement exact sentinels and bounded inspection**

Use fixed names such as `.tbot-hil-nested` and `.tbot-hil-sentinel`; cleanup removes only an empty known directory or a regular file whose exact magic bytes match. It never accepts a path and never recursively removes entries.

Inspection returns stable relative labels, node type, byte count, and SHA-256 for regular files. For fixed protected directories, enumerate only compiled direct-child labels with a hard entry cap; return `truncated=true` rather than walking recursively.

- [ ] **Step 4: Run GREEN, sanitizer, and eviction regression**

```bash
scripts/run_host_native_lesson_storage_hil_fixture_test.sh
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  scripts/run_host_native_lesson_storage_hil_fixture_test.sh
scripts/run_host_native_lesson_asset_cache_evict_test.sh
python3 -m pytest tests/test_lesson_storage_hil_contract.py -q
git diff --check
```

- [ ] **Step 5: Commit Task 5**

```bash
git add main/lesson_storage_hil_fixture.h main/lesson_storage_hil_fixture.cc \
  main/CMakeLists.txt tests/native/lesson_storage_hil_fixture_host_test.cc \
  scripts/run_host_native_lesson_storage_hil_fixture_test.sh \
  tests/test_lesson_storage_hil_contract.py
git commit -m "feat(firmware): add disposable lesson storage hil fixtures"
```

### Task 6: Register Five Checked HIL Tools and Build Identity

**Files:**
- Create: `main/lesson_storage_hil_mcp_tools.h`
- Create: `main/lesson_storage_hil_mcp_tools.cc`
- Modify: `main/mcp_server.cc`
- Modify: `main/boards/common/board.cc`
- Modify: `main/application.cc`
- Create: `scripts/assert_lesson_storage_hil_artifacts.py`
- Modify: `tests/test_lesson_storage_hil_contract.py`
- Modify: `tests/native/mcp_cjson_oom_host_test.cc`
- Modify: `scripts/run_host_native_mcp_cjson_oom_test.sh`

- [ ] **Step 1: Write RED registration, schema, OOM, and identity tests**

Require exactly these names under the compile guard:

```text
self.lesson_assets.hil.arm_fault
self.lesson_assets.hil.status
self.lesson_assets.hil.stage_fixture
self.lesson_assets.hil.cleanup_fixture
self.lesson_assets.hil.inspect
```

Assert every response uses checked-cJSON helpers and handles allocation failure without leaking controller/fixture mutation. Require the boot literal `TBOT_HIL_STORAGE_FAULTS_ENABLED non-production-image` and system-info `"lessonStorageHilFaults":true` only inside the compile guard.

- [ ] **Step 2: Run RED tests**

```bash
python3 -m pytest tests/test_lesson_storage_hil_contract.py -q
IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2 \
  scripts/run_host_native_mcp_cjson_oom_test.sh
```

- [ ] **Step 3: Implement checked tool envelopes**

Add `RegisterLessonStorageHilMcpTools(McpServer&)` and call it once from `AddUserOnlyTools()` under `#if CONFIG_TBOT_HIL_STORAGE_FAULTS`.

Use these exact argument shapes:

```text
arm_fault: required cacheKey, operation, checkpoint, action;
           integer threshold=0, declaredAssetBytes=0, pauseSeconds=0
status: no properties
stage_fixture: required cacheKey, fixture; optional siblingCacheKey=""
cleanup_fixture: required cacheKey, fixture; optional siblingCacheKey=""
inspect: required cacheKey; optional siblingCacheKey=""
```

Use stable exact result fields. For example, arm returns:

```json
{
  "cacheKey": "hil-task14/v1-dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
  "status": "armed",
  "operation": "evict",
  "checkpoint": "after_unlinks",
  "action": "fail",
  "threshold": 1,
  "pauseSeconds": 0,
  "armSequence": 1
}
```

Reject unknown fields/strings, bool-as-integer values, invalid key pairs, and incompatible actions before arming or filesystem access. Sanitize all errors.

Add a boot warning before normal initialization and a compile-gated top-level system-info capability. Implement the artifact auditor to hash and inspect bin/ELF/map/archive, `project_description.json`, sdkconfig, config-default chain, symbols, and tool literals for profile `production` or `hil`.

The auditor writes `lesson-storage-hil-build.json` and
`lesson-storage-hil-build.sha256` into the selected build directory only after
all checks pass. A failed audit must not leave a PASS manifest.

- [ ] **Step 4: Run GREEN firmware software gates**

```bash
scripts/run_host_native_lesson_storage_hil_controller_test.sh
scripts/run_host_native_lesson_storage_hil_fixture_test.sh
scripts/run_host_native_lesson_asset_cache_evict_test.sh
scripts/run_host_native_lesson_asset_download_staging_test.sh
IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2 \
  scripts/run_host_native_mcp_cjson_oom_test.sh
python3 -m pytest tests/test_lesson_storage_hil_contract.py \
  tests/test_lesson_asset_cache_evict_contract.py \
  tests/test_lesson_sd_sync_attestation_contract.py -q
git diff --check
```

- [ ] **Step 5: Commit Task 6**

```bash
git add main/lesson_storage_hil_mcp_tools.h main/lesson_storage_hil_mcp_tools.cc \
  main/mcp_server.cc main/boards/common/board.cc main/application.cc \
  scripts/assert_lesson_storage_hil_artifacts.py \
  tests/test_lesson_storage_hil_contract.py tests/native/mcp_cjson_oom_host_test.cc \
  scripts/run_host_native_mcp_cjson_oom_test.sh
git commit -m "feat(firmware): expose attended lesson storage hil controls"
```

### Task 7: Restrict Raw HIL Calls to One Resolved Live MAC

**Files (ESP server):**
- Modify: `main/tbot-server/core/api/device_mcp_admin_handler.py`
- Modify: `main/tbot-server/config.yaml`
- Modify: `main/tbot-server/config/config_loader.py`
- Modify: `main/tbot-server/docker-compose.yml`
- Modify: `main/tbot-server/docker-compose_all.yml`
- Modify: `deploy/docker-compose.prod.yml`
- Modify: `deploy/.env.example`
- Modify: `main/tbot-server/tests/test_device_mcp_admin_handler.py`
- Modify: `main/tbot-server/tests/test_config_loader_edges.py`
- Modify: `main/tbot-server/tests/test_config_loader_lesson_env_overrides.py`

- [ ] **Step 1: Write RED proxy/config tests**

Cover empty/malformed/multiple/nonmatching allowlists, mixed-case normalization, UUID route identity versus resolved connection MAC, exact-five-tool success, unknown HIL-prefix rejection, `allowUnlisted=false`, raw-call-only dispatch, bounded `timeoutSeconds` 5-75, bool/zero/negative/excessive timeout rejection, and exception/JWT/path redaction. Add trigger-tool cases proving timeout override is accepted only for exact eviction/generic sync with a canonical `hil-*` cache key and rejected for non-HIL/foreign/malformed keys. Preserve unchanged non-HIL behavior when no override is requested.

- [ ] **Step 2: Run RED tests**

```bash
cd main/tbot-server
python3 -m pytest tests/test_device_mcp_admin_handler.py \
  tests/test_config_loader_edges.py \
  tests/test_config_loader_lesson_env_overrides.py -q
```

- [ ] **Step 3: Implement the default-empty resolved-MAC gate**

Define the exact HIL set and prefix. After `_find_connection()`, derive the actual MAC with the existing connection helper and require it equals the sole normalized allowlist entry. Unknown prefixed names fail before dispatch.

For HIL tools, require `allowUnlisted is True`. Permit an exact integer `timeoutSeconds` from 5 through 75 only after the resolved-MAC gate. Apply the same bounded override to the two exact trigger tools only when their structured arguments expose a canonical `hil-*` key; do not trust a separate boolean claim from the caller. Map HIL-path errors to stable responses:

```text
HIL_TOOL_FORBIDDEN
HIL_DEVICE_NOT_ALLOWLISTED
HIL_MCP_TIMEOUT
HIL_MCP_FAILED
```

Never return `str(exc)` on any HIL path. Add `lesson.storage_hil_device_allowlist: []`, parse `LESSON_STORAGE_HIL_DEVICE_ALLOWLIST`, reject more than one MAC, and preserve the local restriction during manager-config merge. Pass both `LESSON_STORAGE_HIL_DEVICE_ALLOWLIST` and `TBOT_PUBLIC_WEBSOCKET_URL` through the local/deployment Compose surfaces.

- [ ] **Step 4: Run GREEN and adjacent proxy gates**

```bash
python3 -m pytest tests/test_device_mcp_admin_handler.py \
  tests/test_config_loader_edges.py \
  tests/test_config_loader_lesson_env_overrides.py \
  tests/test_http_server.py -q
python3 -m compileall core config
ruff check core/api/device_mcp_admin_handler.py config/config_loader.py \
  --select E9,F63,F7,F82
git diff --check
```

- [ ] **Step 5: Commit Task 7 in ESP server**

```bash
git add main/tbot-server/core/api/device_mcp_admin_handler.py \
  main/tbot-server/config.yaml main/tbot-server/config/config_loader.py \
  main/tbot-server/docker-compose.yml main/tbot-server/docker-compose_all.yml \
  deploy/docker-compose.prod.yml \
  deploy/.env.example main/tbot-server/tests/test_device_mcp_admin_handler.py \
  main/tbot-server/tests/test_config_loader_edges.py \
  main/tbot-server/tests/test_config_loader_lesson_env_overrides.py
git commit -m "fix(server): restrict lesson storage hil mcp calls"
```

### Task 8: Add the Attended HIL Orchestrator and Build Validator

**Files (ESP server):**
- Create: `main/tbot-server/scripts/lesson_studio_task14_hil_storage.py`
- Create: `main/tbot-server/scripts/lesson_studio_task14_build_identity.py`
- Create: `main/tbot-server/tests/test_lesson_studio_task14_hil_storage.py`
- Modify: `main/tbot-server/scripts/lesson_studio_task14_fault_driver.py`
- Modify: `main/tbot-server/tests/test_lesson_studio_task14_evidence.py`

- [ ] **Step 1: Write RED orchestrator and identity tests**

Mock HTTP/serial and prove exact response schemas, extra/missing field rejection, bool-as-int rejection, foreign key rejection, serial checkpoint polling, stable timeout, credential redaction, cleanup ordering, power-loss missing-response classification, volatile arm cleared after reboot, and HIL-to-production build ordering.

Require build manifests to bind:

```json
{
  "sourceCommit": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "profile": "hil",
  "configEnabled": true,
  "sdkconfigSha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  "binarySha256": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
  "elfSha256": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
  "mapSha256": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
  "archiveSha256": "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
  "binaryBytes": 1,
  "appPartitionFreeBytes": 1
}
```

- [ ] **Step 2: Run RED tests**

```bash
cd main/tbot-server
python3 -m pytest tests/test_lesson_studio_task14_hil_storage.py \
  tests/test_lesson_studio_task14_evidence.py -q
```

- [ ] **Step 3: Implement a separate injection orchestrator**

Provide CLI commands `preflight`, `run-scenario`, and `run-matrix`. Call only the existing internal raw MCP route. Implement typed helpers for arm/status/stage/cleanup/inspect; never write the mint secret into evidence. Write all JSON/text artifacts atomically and create `SHA256SUMS` last.

Each ordinary scenario directory contains exactly:

```text
command.txt
serial.log
server.log
timeline.log
build-manifest.json
build-manifest.sha256
status-before.json
inspect-before.json
stage-response.json
arm-response.json
trigger-response.json
status-after.json
inspect-after.json
cleanup-response.json
result.json
evidence.json
validator-exit-code.txt
SHA256SUMS
```

Power-loss scenarios replace `trigger-response.json` with
`checkpoint-reached-utc.txt`, `power-removed-utc.txt`, `reboot-serial.log`, and
`post-reboot-inspect.json` because response loss is expected and must never be
interpreted as success.

Keep the existing fault driver validation-only. Add `HIL_STORAGE_SCENARIOS` and require exact increasing arm/reached/consumed sequences plus build identity. For power loss, require reached marker, response loss, reboot capture, cleared arm, inspection, coherent retry/resync, and no success marker before loss.

- [ ] **Step 4: Run GREEN and Task 14 self-tests**

```bash
python3 -m pytest tests/test_lesson_studio_task14_hil_storage.py \
  tests/test_lesson_studio_task14_evidence.py -q
python3 scripts/lesson_studio_task14_fault_driver.py --self-test
python3 -m compileall scripts
ruff check scripts/lesson_studio_task14_hil_storage.py \
  scripts/lesson_studio_task14_build_identity.py --select E9,F63,F7,F82
git diff --check
```

- [ ] **Step 5: Commit Task 8 in ESP server**

```bash
git add main/tbot-server/scripts/lesson_studio_task14_hil_storage.py \
  main/tbot-server/scripts/lesson_studio_task14_build_identity.py \
  main/tbot-server/scripts/lesson_studio_task14_fault_driver.py \
  main/tbot-server/tests/test_lesson_studio_task14_hil_storage.py \
  main/tbot-server/tests/test_lesson_studio_task14_evidence.py
git commit -m "feat(server): orchestrate attended lesson storage hil faults"
```

### Task 9: Add Raw Capture Mode and Require 104 Production Transitions

**Files:**
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/scripts/lesson_e2e_live_capture.py`
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/tests/test_lesson_e2e_live_capture.py`
- Modify: `main/tbot-server/scripts/lesson_studio_task14_soak.py`
- Modify: `main/tbot-server/scripts/lesson_studio_task14_log_audit.py`
- Modify: `main/tbot-server/tests/test_lesson_studio_task14_evidence.py`
- Modify: `main/tbot-server/docs/lesson-studio-task14-live-matrix.md`
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/docs/lesson-studio-task14-live-matrix.md`

- [ ] **Step 1: Write RED capture/soak contracts**

Add tests proving `--capture-only` still requires both serial and server sources, fails on early source exit, skips lesson-specific marker requirements, and emits a timeline. Require live soak/audit CLI to use `--minimum-transitions 104`, reject 103, and bind a production build manifest.

- [ ] **Step 2: Run RED tests in both repositories**

```bash
cd /Users/manhhodinh/Documents/TBOT/robot
python3 -m pytest tests/test_lesson_e2e_live_capture.py -q

cd /Users/manhhodinh/.config/superpowers/worktrees/esp32-server/production-lesson-studio/main/tbot-server
python3 -m pytest tests/test_lesson_studio_task14_evidence.py -q
```

- [ ] **Step 3: Implement capture-only and synchronize the matrix**

Add `--capture-only` without weakening default verification. Parameterize soak/audit minimum transitions, include the required value in output metrics, and require profile `production` in the build manifest. Replace the shorter root live matrix with the reviewed ESP matrix plus the new HIL/reflash ordering; do not maintain divergent instructions.

- [ ] **Step 4: Run GREEN and commit per owning repository**

```bash
cd /Users/manhhodinh/Documents/TBOT/robot
python3 -m pytest tests/test_lesson_e2e_live_capture.py -q

cd /Users/manhhodinh/.config/superpowers/worktrees/esp32-server/production-lesson-studio/main/tbot-server
python3 -m pytest tests/test_lesson_studio_task14_evidence.py -q
python3 scripts/lesson_studio_task14_soak.py --self-test
python3 scripts/lesson_studio_task14_log_audit.py --self-test
```

Commit ESP-owned changes:

```bash
git add main/tbot-server/scripts/lesson_studio_task14_soak.py \
  main/tbot-server/scripts/lesson_studio_task14_log_audit.py \
  main/tbot-server/tests/test_lesson_studio_task14_evidence.py \
  main/tbot-server/docs/lesson-studio-task14-live-matrix.md
git commit -m "test(server): bind task14 evidence to production build identity"
```

The root harness is not currently a Git repository. Update its capture script,
tests, and synchronized matrix in place, and record their SHA-256 values in the
evidence bundle. Do not claim a root commit that cannot exist.

### Task 10: Run Full Software Review and Build Both Profiles

**Files:**
- Modify only if a software/build review exposes a real defect.

- [ ] **Step 1: Run all firmware software gates**

```bash
scripts/run_host_native_lesson_storage_hil_controller_test.sh
scripts/run_host_native_lesson_storage_hil_fixture_test.sh
scripts/run_host_native_lesson_asset_storage_coordinator_test.sh
scripts/run_host_native_lesson_asset_cache_evict_test.sh
scripts/run_host_native_lesson_asset_sync_path_test.sh
scripts/run_host_native_lesson_asset_download_staging_test.sh
scripts/run_host_native_lesson_handler_test.sh
IDF_PATH=/Users/manhhodinh/esp/esp-idf-v5.5.2 \
  scripts/run_host_native_mcp_cjson_oom_test.sh
python3 -m pytest tests -q
git diff --check
```

- [ ] **Step 2: Run full ESP server gates**

```bash
cd /Users/manhhodinh/.config/superpowers/worktrees/esp32-server/production-lesson-studio/main/tbot-server
python3 -m pytest -q
python3 -m compileall core config scripts
ruff check core/api/device_mcp_admin_handler.py config/config_loader.py \
  scripts/lesson_studio_task14_hil_storage.py \
  scripts/lesson_studio_task14_build_identity.py --select E9,F63,F7,F82
git diff --check
```

- [ ] **Step 3: Build production in a new immutable directory**

```bash
cd /Users/manhhodinh/.config/superpowers/worktrees/TBOT-Firmware/production-lesson-studio
test -z "$(git status --porcelain)"
test ! -e build-task14-production
test ! -e build-task14-hil
git rev-parse HEAD
source /Users/manhhodinh/esp/esp-idf-v5.5.2/export.sh
idf.py -B build-task14-production \
  -D SDKCONFIG=build-task14-production/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local' \
  set-target esp32s3 build
python3 scripts/assert_lcdwiki_prod_config.py build-task14-production/sdkconfig
python3 scripts/assert_lesson_storage_hil_artifacts.py \
  --profile production --build-dir build-task14-production
```

Record binary size, partition free bytes/percent, source commit, sdkconfig hash, and SHA-256 for bin/ELF/map/archive. Do not rebuild this directory after recording its manifest.

- [ ] **Step 4: Build HIL in a separate immutable directory**

```bash
mkdir -p build-task14-hil
python3 scripts/generate_lesson_storage_hil_local_config.py \
  --ota-url http://192.168.100.209:8003/tbot/ota/ \
  --websocket-url ws://192.168.100.209:8000/tbot/v1/ \
  --output build-task14-hil/sdkconfig.defaults.hil-local

idf.py -B build-task14-hil \
  -D SDKCONFIG=build-task14-hil/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local;sdkconfig.defaults.hil-storage;/Users/manhhodinh/.config/superpowers/worktrees/TBOT-Firmware/production-lesson-studio/build-task14-hil/sdkconfig.defaults.hil-local' \
  set-target esp32s3 build
python3 scripts/assert_lesson_storage_hil_artifacts.py \
  --profile hil --build-dir build-task14-hil
```

Require the same source commit, different binary hashes, HIL symbols/tools only in HIL, and no unsupported FAT APIs in either image.

Record the generated local input SHA-256:

```bash
shasum -a 256 build-task14-hil/sdkconfig.defaults.hil-local
```

The generated file stays inside the ignored build directory and is not
committed. The HIL build manifest must include its hash and the full defaults
chain. Preserve it until both build manifests and hardware evidence have been
finalized.

- [ ] **Step 5: Run fresh spec and quality reviews**

Dispatch independent reviewers over all firmware and ESP commits. Any finding returns to the owning implementer with RED/GREEN regression coverage, then re-review until both approve.

### Task 11: Flash the HIL Image and Run the Storage Fault Matrix

**Files:**
- Evidence only under the runtime-selected `/Users/manhhodinh/Documents/TBOT/.codex_tmp/task14-live-$RUN_ID/` directory.

- [ ] **Step 0: Freeze the evidence directory before touching hardware**

```bash
export RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
export EVIDENCE_ROOT="/Users/manhhodinh/Documents/TBOT/.codex_tmp/task14-live-$RUN_ID"
mkdir -p "$EVIDENCE_ROOT"/{preflight,flash,storage-hil,production-reflash,soak}
```

- [ ] **Step 1: Build and recreate the current ESP server for the lab robot**

```bash
cd /Users/manhhodinh/.config/superpowers/worktrees/esp32-server/production-lesson-studio
export TBOT_SERVER_IMAGE_TAG="hil-$(git rev-parse --short=12 HEAD)"
./deploy/build-local.sh --tag "$TBOT_SERVER_IMAGE_TAG" --only server --no-latest

export LESSON_RUNTIME_ENABLED=true
export LESSON_MOTION_PRESETS_ENABLED=true
export LESSON_PLAYFUL_INTERACTIONS_ENABLED=true
export LESSON_ROLLOUT_DEVICE_ALLOWLIST=28:84:85:85:1a:80
export LESSON_STORAGE_HIL_DEVICE_ALLOWLIST=28:84:85:85:1a:80
export COURSE_BACKEND_URL=http://192.168.100.209:3100/v1
export LESSON_ASSET_ORIGIN_BASE=http://192.168.100.209:8102/tvideo-demo
export LESSON_ASSET_PUBLIC_BASE_URL=http://192.168.100.209:8102/tvideo-demo
export LESSON_ASSET_DELIVERY_MODE=sd_pack
export TBOT_PUBLIC_WEBSOCKET_URL=ws://192.168.100.209:8000/tbot/v1/
test -n "$TBOT_DEVICE_MINT_SECRET"

docker compose -f main/tbot-server/docker-compose.yml up -d --force-recreate
docker inspect tbot-esp32-server --format '{{.Config.Image}} {{.State.Status}}'
```

Require the inspected image tag equals `$TBOT_SERVER_IMAGE_TAG`, the container
is running, and local health/metrics endpoints respond. Stop if a stale image or
old `192.168.1.25`/`192.168.100.230` endpoint remains.

- [ ] **Step 2: Put the board in download mode and verify identity**

Use actual port `/dev/cu.usbmodem101`. The operator holds BOOT, taps RESET, then releases BOOT.

```bash
/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/manhhodinh/esp/esp-idf-v5.5.2/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodem101 read-mac
```

Expected MAC: `28:84:85:85:1a:80`. Stop on mismatch.

- [ ] **Step 3: Flash without rebuilding**

```bash
cd /Users/manhhodinh/.config/superpowers/worktrees/TBOT-Firmware/production-lesson-studio/build-task14-hil
/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/manhhodinh/esp/esp-idf-v5.5.2/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodem101 \
  --baud 460800 --before default-reset --after hard-reset \
  write-flash "@flash_args"
```

Capture exit code and verify the flashed binary hash equals the frozen HIL build manifest.

- [ ] **Step 4: Attest HIL boot and current server**

Require MAC `28:84:85:85:1a:80`, UUID `fce7bec8-8478-4ab4-817f-7b87c41c1f91`, board `lcdwiki-es3c35p`, the HIL boot warning, `lessonStorageHilFaults=true`, current ESP server commit, local LAN endpoints `192.168.100.209`, one live connection, and the exact MAC allowlist.

```bash
curl -fsS http://127.0.0.1:8003/internal/lesson-runtime/metrics \
  | jq -e '.connections == 1 and (.devices | length) == 1'
```

- [ ] **Step 5: Run the complete HIL storage matrix**

```bash
cd /Users/manhhodinh/.config/superpowers/worktrees/esp32-server/production-lesson-studio/main/tbot-server
python3 scripts/lesson_studio_task14_hil_storage.py run-matrix \
  --device-id 28:84:85:85:1a:80 \
  --device-uuid fce7bec8-8478-4ab4-817f-7b87c41c1f91 \
  --serial-port /dev/cu.usbmodem101 \
  --esp-base-url http://127.0.0.1:8003 \
  --asset-url http://192.168.100.209:8102/tvideo-demo/esp-tft/barn-192.png \
  --asset-sha256 0bc9825de6b18c76990127d0ced5ff8c93dfd0bd931aa5689b3ff46e9d812679 \
  --asset-bytes 42986 \
  --build-manifest /Users/manhhodinh/.config/superpowers/worktrees/TBOT-Firmware/production-lesson-studio/build-task14-hil/lesson-storage-hil-build.json \
  --evidence-dir "$EVIDENCE_ROOT/storage-hil"
```

The operator performs attended SD removal and power removal only after the exact reached marker. Stop on any non-HIL path, unknown entry, false success, unbounded pause, secret leakage, reset outside the named power-loss scenario, or validator failure.

### Task 12: Reflash Production and Complete Task 14

**Files:**
- Evidence bundle and authoritative test matrices only after real PASS evidence exists.

- [ ] **Step 1: Re-enter download mode and flash the frozen production image**

```bash
cd /Users/manhhodinh/.config/superpowers/worktrees/TBOT-Firmware/production-lesson-studio/build-task14-production
/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/manhhodinh/esp/esp-idf-v5.5.2/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodem101 \
  --baud 460800 --before default-reset --after hard-reset \
  write-flash "@flash_args"
```

Require production profile, same source commit as HIL, different binary hash, HIL config false, HIL tool strings/symbols absent, expected MAC/UUID, and flash timestamp after the final HIL scenario.

- [ ] **Step 2: Run production cold/warm/offline/fault recovery/rollback**

Follow the synchronized live matrix. HIL fixture/fault tools must now be unavailable. Only coherent production `evicted` or `not_found` permits cold assignment creation, and cold must prove `downloadedCount > 0`.

- [ ] **Step 3: Capture preview parity**

Capture current admin preview and attended 480x320 hardware image for the same immutable step. Compare layer rectangles, English word, selected branch outcome, and motion timeline. Run the preview-parity validator to PASS.

- [ ] **Step 4: Run 13 fresh 8-minute sessions for 104 transitions**

```bash
python3 scripts/lesson_studio_task14_soak.py \
  "$EVIDENCE_ROOT/soak/serial.log" "$EVIDENCE_ROOT/soak/server.log" \
  --timeline-log "$EVIDENCE_ROOT/soak/timeline.log" \
  --minimum-transitions 104 \
  --build-manifest "$EVIDENCE_ROOT/production-reflash/build-manifest.json" \
  --fixture-version 2026-07-11.1 \
  --course-id production-farm-english-358 \
  --lesson-id pip-farm-8m \
  --output "$EVIDENCE_ROOT/soak/report.json"

python3 scripts/lesson_studio_task14_log_audit.py \
  "$EVIDENCE_ROOT/soak/serial.log" "$EVIDENCE_ROOT/soak/server.log" \
  --timeline-log "$EVIDENCE_ROOT/soak/timeline.log" \
  --minimum-transitions 104 \
  --build-manifest "$EVIDENCE_ROOT/production-reflash/build-manifest.json" \
  --fixture-version 2026-07-11.1 \
  --course-id production-farm-english-358 \
  --lesson-id pip-farm-8m \
  --output "$EVIDENCE_ROOT/soak/audit.json"
```

Require both JSON files report PASS, all seven heap markers repeat, PSRAM loss is not monotonic beyond 64 KiB, internal SRAM minimum is at least 20 KiB, and no reset/watchdog/panic/allocation/decode/audio/sequence/duplicate-progress marker appears.

- [ ] **Step 5: Restore the stable production OTA/bootstrap state**

After all local Task 14 evidence is complete, stop the local ESP server so the
NVS local OTA URL cannot open, reset the production firmware once, and allow the
compiled stable fallback `https://esp.tjbot.vn/tbot/ota/` to recover and persist
itself. Capture serial proof of the failed local URL, successful stable fallback,
production WebSocket configuration, and healthy reconnect. Never erase NVS.

Restart the robot a second time with the local server still stopped and prove it
uses the stable production OTA URL immediately rather than retrying
`192.168.100.209` or the obsolete `192.168.1.25` address.

- [ ] **Step 6: Update evidence matrices and commit owning-repo docs**

Record exact commands, source commits, configs, binary hashes, MAC/UUID, UTC timestamps, screenshots, heap metrics, raw-bundle paths, and SHA-256 values. Keep raw/private artifacts outside Git. Never commit `main/manager-web/output/`.

---

## Final Acceptance Gate

This supporting plan is complete only when:

- firmware and ESP spec/quality reviews approve;
- production and HIL builds share one reviewed source commit and pass opposite symbol/config audits;
- the full HIL storage matrix validates on the bound robot;
- the production image is reflashed afterward and HIL tools are unavailable;
- current production cold/warm/offline/rollback and preview parity pass;
- 104 production transitions pass heap/log audit;
- authoritative Task 14 matrices contain real evidence rather than synthetic self-test output.
