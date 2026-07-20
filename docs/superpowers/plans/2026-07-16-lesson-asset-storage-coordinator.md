# Lesson Asset Storage Coordinator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Safely coordinate lesson asset synchronization, lesson prepare/runtime, and attended exact cache eviction on ESP-IDF 5.5.2 FATFS, including truthful partial-deletion recovery and Task 14 evidence handling.

**Architecture:** A process-wide coordinator grants either one non-waiting mutation lease or one prepared/running lesson-session reservation. Every lesson-assets writer holds a mutation lease; `lesson_prepare` reserves before any asset read and releases on every terminal/abandoned path. Eviction uses FAT-supported pathname APIs under that internal-concurrency boundary, reports partial mutation truthfully, and the ESP server rejects partial results as cold evidence.

**Tech Stack:** C++17, ESP-IDF 5.5.2, FreeRTOS application/lesson tasks, FAT VFS, cJSON, native `clang++` host tests, Python/Pytest contract tests, aiohttp ESP server.

**Repository roots:**

- Firmware: `/Users/manhhodinh/.config/superpowers/worktrees/TBOT-Firmware/production-lesson-studio`
- ESP server: `/Users/manhhodinh/.config/superpowers/worktrees/esp32-server/production-lesson-studio`

---

### Task 0: Replace the Unsafe Draft Contract

**Files:**
- Modify: `main/lesson_asset_cache_evict.h`
- Modify: `main/lesson_asset_cache_evict.cc`
- Modify: `tests/native/lesson_asset_cache_evict_host_test.cc`
- Modify: `tests/test_lesson_asset_cache_evict_contract.py`

- [ ] **Step 1: Write RED target-compatibility tests**

Add contract assertions that the production helper contains none of:

```python
for unsupported in (
    "lstat(", "openat(", "fstatat(", "fdopendir(", "unlinkat(",
    "st_ino", "st_dev", "S_ISLNK",
):
    assert unsupported not in helper_source
```

Extend the native invalid matrix with exact strings for `v+1`, leading `../`,
`file://`, `https://`, `%2f`, doubled `/`, dotted slug, and an embedded NUL
constructed with an explicit length. Add public target-compatible limits of 128
slug bytes and 10 version digits, and prove each maximum is accepted while
maximum plus one is rejected before substring allocation.

- [ ] **Step 2: Run RED tests**

Run:

```bash
scripts/run_host_native_lesson_asset_cache_evict_test.sh
python3 -m pytest tests/test_lesson_asset_cache_evict_contract.py -q
```

Expected: FAIL because the unsafe draft still uses `lstat`, inode/device
comparison, symlink-specific result/tests, and does not mirror the full parser
matrix.

- [ ] **Step 3: Change the public result contract**

Replace the draft enum with:

```cpp
enum class LessonAssetCacheEvictCode {
    kEvicted,
    kNotFound,
    kInvalidCacheKey,
    kLessonSessionActive,
    kPathMismatch,
    kNestedDirectory,
    kUnexpectedNodeType,
    kScanFailed,
    kUnlinkFailed,
    kRmdirFailed,
    kPartialEvictRecoveryRequired,
};
```

Keep the existing result fields and public parser/name/evict functions. Remove
all unsupported target APIs and inode claims. Use `stat`, `opendir`, `readdir`,
`unlink`, and `rmdir` only.

- [ ] **Step 4: Re-run the focused tests**

Run the commands from Step 2.

Expected: parser and target-compatibility tests pass; behavioral eviction tests
that require the coordinator remain RED.

- [ ] **Step 5: Do not commit yet**

Task 0 deliberately leaves the eviction helper incomplete until the
coordinator exists. Confirm `git diff --check` is clean and continue to Task 1.

### Task 1: Implement the Reservation Coordinator Core

**Files:**
- Create: `main/lesson_asset_storage_coordinator.h`
- Create: `main/lesson_asset_storage_coordinator.cc`
- Create: `tests/native/lesson_asset_storage_coordinator_host_test.cc`
- Create: `scripts/run_host_native_lesson_asset_storage_coordinator_test.sh`
- Modify: `main/CMakeLists.txt`
- Modify: `tests/test_lesson_asset_cache_evict_contract.py`

- [ ] **Step 1: Write RED native coordinator tests**

Cover these exact behaviors:

```cpp
auto mutation = coordinator.TryBeginMutation("evict");
Expect(static_cast<bool>(mutation), "first mutation must acquire");
Expect(!coordinator.TryBeginMutation("sync"), "second mutation must refuse");
Expect(
    coordinator.TryBeginLessonSession("assignment-a", "session-a").code ==
        LessonAssetReservationCode::kMutationActive,
    "lesson prepare must not overlap mutation"
);
```

Also test move construction/assignment, destructor release, same-session
idempotence, foreign-session refusal, foreign release refusal, exact owner
release, force teardown, mutation versus prepare barrier races using two host
threads, and no leaked reservation after exceptions/early returns. Add empty,
embedded-NUL, 128-byte, and 129-byte identity cases. Prove force/reacquire with
the same IDs issues a fresh generation and both sequential and concurrent stale
terminal releases cannot release the replacement. Exercise 64-bit exhaustion
with a host-only seam and require fail-closed behavior without zero or reuse.

- [ ] **Step 2: Add the host runner and confirm RED**

Create an executable runner that compiles only the coordinator and host test
with `-std=c++17 -pthread -Wall -Wextra -Werror` into a temporary directory.

Run:

```bash
chmod +x scripts/run_host_native_lesson_asset_storage_coordinator_test.sh
scripts/run_host_native_lesson_asset_storage_coordinator_test.sh
```

Expected: FAIL because the coordinator files do not yet exist.

- [ ] **Step 3: Implement the coordinator API**

Use this public surface:

```cpp
enum class LessonAssetReservationCode {
    kAcquired,
    kMutationActive,
    kLessonSessionActive,
    kLessonSessionMismatch,
    kInvalidIdentity,
    kGenerationExhausted,
};

struct LessonAssetSessionResult {
    LessonAssetReservationCode code;
    bool acquired;
    bool idempotent;
    std::uint64_t generation;
};

class LessonAssetMutationLease {
public:
    LessonAssetMutationLease(LessonAssetMutationLease&& other) noexcept;
    LessonAssetMutationLease& operator=(LessonAssetMutationLease&& other) noexcept;
    ~LessonAssetMutationLease();
    explicit operator bool() const;
    LessonAssetReservationCode code() const;

    LessonAssetMutationLease(const LessonAssetMutationLease&) = delete;
    LessonAssetMutationLease& operator=(const LessonAssetMutationLease&) = delete;
};
```

The singleton stores a short-held `std::mutex`, mutation-active flag, operation
label for diagnostics, and current assignment/session IDs. The lease does not
hold the mutex while filesystem work runs; it owns the reservation flag and
releases it under the mutex exactly once.

Session identities are 1 through 128 bytes with no embedded NUL. Validate and
copy them before locking, then swap them into state before publishing active.
Each new session receives a never-reused nonzero 64-bit generation; duplicate
prepare returns the same generation, `EndLessonSession` requires IDs plus that
generation, and exhaustion refuses rather than wrapping.

- [ ] **Step 4: Add CMake wiring and static contracts**

Add `"lesson_asset_storage_coordinator.cc"` beside the eviction source. Assert
the lease is move-only, acquisition is non-waiting at the reservation level,
and no secret/path is stored in the operation label.

- [ ] **Step 5: Run GREEN and commit Task 1**

Run:

```bash
scripts/run_host_native_lesson_asset_storage_coordinator_test.sh
python3 -m pytest tests/test_lesson_asset_cache_evict_contract.py -q
git diff --check
```

Expected: native runner reports a non-zero check count; Pytest passes.

Commit:

```bash
git add main/lesson_asset_storage_coordinator.h \
  main/lesson_asset_storage_coordinator.cc main/CMakeLists.txt \
  tests/native/lesson_asset_storage_coordinator_host_test.cc \
  scripts/run_host_native_lesson_asset_storage_coordinator_test.sh \
  tests/test_lesson_asset_cache_evict_contract.py \
  main/lesson_asset_cache_evict.h main/lesson_asset_cache_evict.cc \
  tests/native/lesson_asset_cache_evict_host_test.cc
git commit -m "feat(firmware): coordinate lesson asset storage reservations"
```

### Task 1A: Align Cache-Key Limits Across All Producers and Consumers

**Files (Nest backend repository):**
- Create: `src/lessons/lesson-cache-key.contract.ts`
- Modify: `src/lessons/authoring/lesson-authoring.dto.ts`
- Modify: `src/lessons/authoring/lesson-authoring.controller.ts`
- Modify: `src/lessons/authoring/lesson-authoring.service.ts`
- Modify: `src/lessons/dto/create-assignment.dto.ts`
- Modify: `src/lessons/dto/create-assignment.dto.spec.ts`
- Create: `src/lessons/authoring/lesson-cache-key-contract.spec.ts`
- Create: `src/lessons/authoring/lesson-cache-key-http-boundary.spec.ts`

**Files (ESP server repository):**
- Modify: `main/tbot-server/core/lesson/sd_pack_evict.py`
- Modify: `main/tbot-server/tests/test_lesson_sd_pack_evict.py`

- [ ] **Step 1: Write RED shared-limit tests**

Apply one exact protocol contract everywhere:

```text
slug bytes: 1..128 ASCII
version digits: 1..10, first digit 1..9
checksum bytes: exactly 64 lowercase hex
complete cache key bytes: <=205
```

Nest tests accept a 128-byte canonical slug and reject 129 bytes, uppercase,
edge/doubled hyphens, non-ASCII, whitespace, URI/percent/traversal strings,
empty IDs, and non-string values. ESP tests accept version `9999999999` and
reject 11 digits, accept a 128-byte slug and reject 129, and retain every
existing invalid path case.

The Nest HTTP/controller test must invoke the real create-lesson endpoint path,
not only instantiate `CreateLessonDraftDto`. It proves the anonymous authoring
body cannot bypass the production validator and that the service performs the
same check before any database write.

- [ ] **Step 2: Run RED tests**

Run the focused Nest authoring/assignment DTO tests and ESP
`tests/test_lesson_sd_pack_evict.py`.

Expected: one or more upstream boundaries accept values firmware refuses.

- [ ] **Step 3: Implement the shared boundary**

Create one Nest pure contract helper exporting the canonical regex, 128-byte
limit, predicate, and an assertion that throws the existing sanitized bad-input
error. Nest DTOs use `@IsString`, `@IsNotEmpty`, `@MaxLength(128)`, and the
shared ASCII regex on both `CreateLessonDraftDto.lessonKey` and
`CreateAssignmentDto.lessonId`. The live authoring controller/service calls the
shared assertion before persistence because the current endpoint's anonymous
`@Body()` type is not a runtime validation boundary. Do not normalize or
truncate. ESP checks ASCII byte counts before regex matching and preserves
sanitized invalid-key errors.

- [ ] **Step 4: Audit existing published keys**

Run a read-only database query or fixture audit listing published `lesson_key`
values outside the new contract. Expected before rollout: zero rows. If rows
exist, stop and create an explicit migration; never rewrite identifiers inside
this task.

- [ ] **Step 5: Run GREEN and commit per repository**

Run focused and adjacent assignment/authoring and ESP eviction suites. Commit
Nest and ESP changes separately:

```bash
git commit -m "fix(lessons): enforce robot cache key limits"
git commit -m "fix(server): align lesson cache key limits"
```

### Task 2: Reserve the Lesson Session Before Asset Reads

**Files:**
- Modify: `main/lesson_handler.cc`
- Modify: `tests/native/lesson_handler_host_test.cc`
- Modify: `tests/test_lesson_dispatch_backward_compat.py`
- Modify: `tests/test_realtime_voice_state.py`
- Modify: `scripts/run_host_native_lesson_handler_test.sh`

- [ ] **Step 1: Write RED lesson lifecycle tests**

Add native cases proving:

- prepare acquires before `BuildAssetPackAck` and file reads;
- mutation-active prepare emits a stable retryable rejection and does no asset
  I/O;
- duplicate prepare for the same IDs is idempotent;
- a foreign prepare cannot replace the owner;
- malformed/failed prepare after acquisition releases its reservation;
- stop, error, cancel, terminal completion, and reset/disconnect teardown release
  the exact session;
- a foreign terminal frame cannot release the owner.

Add static ordering assertions such as:

```python
prepare = lesson_handler[prepare_start:prepare_end]
assert prepare.index("TryBeginLessonSession(") < prepare.index("BuildAssetPackAck(body)")
```

- [ ] **Step 2: Run RED lesson gates**

Run:

```bash
scripts/run_host_native_lesson_handler_test.sh
python3 -m pytest \
  tests/test_lesson_dispatch_backward_compat.py \
  tests/test_realtime_voice_state.py -q
```

Expected: new reservation/lifecycle tests fail.

- [ ] **Step 3: Integrate the coordinator**

After pure envelope/identity validation and before `BuildAssetPackAck`, call:

```cpp
const auto reservation =
    LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
        assignment_id, session_id);
```

Handle every `!reservation.acquired` result, including mutation active, foreign
lesson session, invalid identity, and generation exhaustion. Emit a stable
retryable or validation error and return before asset access. Store the returned
non-zero generation. Track whether the current prepare newly acquired the
reservation so every later rejection releases only what it acquired.

Release using the exact assignment/session IDs and coordinator-issued
generation on terminal paths. Zero, stale, or wrong generations must not
release the active session. Use
`ForceEndLessonSession()` only on connection/device teardown that invalidates
all lesson state.

- [ ] **Step 4: Run GREEN and commit Task 2**

Run the commands from Step 2 plus:

```bash
python3 -m pytest tests/test_lesson_background_visibility_contract.py -q
git diff --check
```

Commit:

```bash
git add main/lesson_handler.cc tests/native/lesson_handler_host_test.cc \
  tests/test_lesson_dispatch_backward_compat.py tests/test_realtime_voice_state.py \
  scripts/run_host_native_lesson_handler_test.sh
git commit -m "fix(firmware): reserve lesson assets across runtime"
```

### Task 3: Guard Every Sync Writer and Bind Paths to the Cache Key

**Files:**
- Modify: `main/mcp_server.cc`
- Modify: `tests/test_lesson_asset_cache_evict_contract.py`
- Modify: `tests/test_lesson_sd_sync_attestation_contract.py`
- Create: `tests/native/lesson_asset_sync_path_host_test.cc`
- Create: `scripts/run_host_native_lesson_asset_sync_path_test.sh`

- [ ] **Step 1: Write RED writer-coverage and path tests**

Add contract tests that enumerate all lesson-assets mutation functions and
assert each user tool acquires a mutation lease before the first `mkdir`,
`remove`, write-open, or `rename`.

Add native path cases for an asset pack with canonical cache key and reject:

```text
/sdcard/tbot/lesson-assets/current.json
/sdcard/tbot/lesson-assets/<slug>/pvg.json
/sdcard/tbot/lesson-assets/<foreign-key>/poster.jpg
/sdcard/tbot/lesson-assets/<cache-key>/nested/poster.jpg
/sdcard/tbot/lesson-assets/<cache-key>/../poster.jpg
/sdcard/tbot/lesson-assets/<cache-key>/
```

Accept only a direct basename below the exact key. Explicitly reject reserved
internal suffixes supplied by the caller (`.tmp`, `.download`).

- [ ] **Step 2: Run RED sync tests**

Run:

```bash
scripts/run_host_native_lesson_asset_sync_path_test.sh
python3 -m pytest \
  tests/test_lesson_sd_sync_attestation_contract.py \
  tests/test_lesson_asset_cache_evict_contract.py -q
```

Expected: broad prefix-only validation and unleased writers fail.

- [ ] **Step 3: Implement exact path binding and leases**

Extract a pure helper that builds the one permitted prefix:

```cpp
const std::string expected_prefix =
    std::string(kLessonAssetPackRoot) + canonical_cache_key + "/";
```

Require the remaining suffix to be one non-empty basename containing no slash,
backslash, dot-segment, or internal temporary suffix. Reconstruct and byte
compare the final path.

Acquire one mutation lease at the beginning of each complete sync tool
callback. A lesson-session conflict returns a stable public refusal before any
filesystem call. Keep the lease through all downloads, verification, renames,
and cleanup.

- [ ] **Step 4: Run GREEN and commit Task 3**

Run the commands from Step 2 and:

```bash
python3 -m pytest tests/test_mcp_tools_pagination_contract.py -q
git diff --check
```

Commit:

```bash
git add main/mcp_server.cc tests/test_lesson_asset_cache_evict_contract.py \
  tests/test_lesson_sd_sync_attestation_contract.py \
  tests/native/lesson_asset_sync_path_host_test.cc \
  scripts/run_host_native_lesson_asset_sync_path_test.sh
git commit -m "fix(firmware): guard and bind lesson asset sync writes"
```

### Task 4: Implement FAT-Safe Exact Eviction and Partial Recovery

**Files:**
- Modify: `main/lesson_asset_cache_evict.h`
- Modify: `main/lesson_asset_cache_evict.cc`
- Modify: `tests/native/lesson_asset_cache_evict_host_test.cc`
- Modify: `scripts/run_host_native_lesson_asset_cache_evict_test.sh`
- Modify: `tests/test_lesson_asset_cache_evict_contract.py`
- Modify: `main/mcp_server.cc`

- [ ] **Step 1: Write RED coordinator and partial-result tests**

Add native tests for:

- prepared lesson returns `kLessonSessionActive` before scan;
- concurrent sync-style mutation lease returns the same refusal;
- exact leaf with multiple regular files deletes and reports the exact count;
- distinct sibling version, slug parent, `current.json`, PVG directory, and
  shared store remain byte-for-byte present;
- nested directory/FIFO/unexpected node refuses with zero mutation;
- injected scan failure refuses with zero mutation;
- injected first unlink failure returns `kUnlinkFailed`, count zero;
- injected later unlink failure returns `kPartialEvictRecoveryRequired` with
  exact deleted count;
- rmdir failure after deletion returns partial with the deleted count;
- a retry completes a partial leaf;
- final absence verification failure never reports `evicted`;
- accidental root/slug/leaf disappearance or SD errors never become false
  `not_found` unless the exact leaf lookup itself authoritatively returns
  `ENOENT` while root and slug remain valid.

- [ ] **Step 2: Run RED eviction tests**

Run:

```bash
scripts/run_host_native_lesson_asset_cache_evict_test.sh
python3 -m pytest tests/test_lesson_asset_cache_evict_contract.py -q
```

Expected: coordinator conflicts and partial semantics fail.

- [ ] **Step 3: Implement the deletion transaction**

The function order is exact:

1. validate and reconstruct the cache key;
2. acquire a mutation lease;
3. verify root and slug directory topology;
4. check the exact leaf (`ENOENT` here is idempotent not-found);
5. scan every direct child with no mutation and require regular files only;
6. reopen/rescan to reject topology changes before deletion;
7. unlink direct validated basenames while counting successful deletes;
8. classify any later failure as partial when the count is non-zero;
9. remove the exact leaf;
10. verify final `stat` is `ENOENT` before claiming success.

Do not use recursion, symlink/inode claims, normalization, `realpath`, or any
unsupported `*at` API. Host-only deterministic failure seams must be compiled
out under `ESP_PLATFORM`.

- [ ] **Step 4: Register the exact user-only response**

Keep `self.lesson_assets.evict_cache_key` in `AddUserOnlyTools` with only one
string `cacheKey` property and exactly six response fields:

```cpp
cJSON_AddStringToObject(json, "cacheKey", result.cache_key.c_str());
cJSON_AddStringToObject(json, "status", LessonAssetCacheEvictCodeName(result.code));
cJSON_AddBoolToObject(json, "evicted", result.evicted);
cJSON_AddBoolToObject(json, "notFound", result.not_found);
cJSON_AddNumberToObject(json, "fileCount", result.file_count);
cJSON_AddStringToObject(json, "reason", LessonAssetCacheEvictCodeName(result.code));
```

Only this exact tool bypasses the generic runtime MCP guard so its callback can
return typed `lesson_session_active`; the storage coordinator remains the
authoritative refusal.

- [ ] **Step 5: Run GREEN and commit Task 4**

Run:

```bash
scripts/run_host_native_lesson_asset_storage_coordinator_test.sh
scripts/run_host_native_lesson_asset_cache_evict_test.sh
scripts/run_host_native_lesson_asset_sync_path_test.sh
python3 -m pytest \
  tests/test_lesson_asset_cache_evict_contract.py \
  tests/test_lesson_sd_sync_attestation_contract.py \
  tests/test_mcp_tools_pagination_contract.py \
  tests/test_realtime_voice_state.py -q
git diff --check
```

Commit:

```bash
git add main/lesson_asset_cache_evict.h main/lesson_asset_cache_evict.cc \
  main/mcp_server.cc tests/native/lesson_asset_cache_evict_host_test.cc \
  tests/test_lesson_asset_cache_evict_contract.py \
  scripts/run_host_native_lesson_asset_cache_evict_test.sh
git commit -m "feat(firmware): evict exact lesson cache under storage lease"
```

### Task 5: Teach the ESP Server About Partial Eviction

**Files (ESP server repository):**
- Modify: `main/tbot-server/core/lesson/sd_pack_evict.py`
- Modify: `main/tbot-server/core/api/lesson_sd_evict_handler.py`
- Modify: `main/tbot-server/tests/test_lesson_sd_pack_evict.py`
- Modify: `main/tbot-server/tests/test_lesson_sd_evict_handler.py`
- Modify: `main/tbot-server/scripts/lesson_studio_task14_fault_driver.py`
- Modify: `main/tbot-server/tests/test_lesson_studio_task14_evidence.py`
- Modify: `main/tbot-server/docs/lesson-studio-task14-live-matrix.md`
- Modify: `main/tbot-server/docs/TEST_MATRIX_TASK14.md`

- [ ] **Step 1: Write RED protocol tests**

Add cases for the exact firmware result:

```json
{
  "cacheKey": "pip-farm-3m/v1-<sha256>",
  "status": "partial_evict_recovery_required",
  "evicted": false,
  "notFound": false,
  "fileCount": 2,
  "reason": "partial_evict_recovery_required"
}
```

Prove the server:

- returns a stable non-2xx maintenance failure;
- preserves the validated canonical key/code/deleted count;
- never treats partial as `evicted` or `not_found`;
- never accepts it in cold evidence;
- tells the attended operator to retry/repair the exact key;
- still rejects contradictory booleans, negative/boolean counts, foreign keys,
  unknown codes, and arbitrary remote text.

- [ ] **Step 2: Run RED server tests**

Run from `main/tbot-server`:

```bash
python3 -m pytest \
  tests/test_lesson_sd_pack_evict.py \
  tests/test_lesson_sd_evict_handler.py \
  tests/test_lesson_studio_task14_evidence.py -q
```

Expected: partial result is currently rejected generically or loses its count.

- [ ] **Step 3: Implement typed partial handling**

Extend the exact response validator with a coherent partial branch. Keep all
pre-mutation refusals at count zero. Map partial to a sanitized stable HTTP
status and response; do not include exception text or local paths. Update the
Task 14 validator/runbook so only `evicted` or `not_found` can proceed to fresh
assignment creation.

- [ ] **Step 4: Run GREEN and commit Task 5**

Run the command from Step 2 plus the three Task 14 self-tests, Ruff, compile,
and `git diff --check`.

Commit only the listed ESP server files:

```bash
git commit -m "fix(server): surface partial lesson cache eviction safely"
```

### Task 6: Build the ESP32-S3 Target and Audit Unsupported Symbols

**Files:**
- Modify only if the target build exposes a real contract defect in Tasks 1-4.

- [ ] **Step 1: Run all firmware software gates**

```bash
scripts/run_host_native_lesson_asset_storage_coordinator_test.sh
scripts/run_host_native_lesson_asset_cache_evict_test.sh
scripts/run_host_native_lesson_asset_sync_path_test.sh
scripts/run_host_native_lesson_handler_test.sh
python3 -m pytest tests -q
```

Expected: all existing and new tests pass with no orphan host processes.

- [ ] **Step 2: Build with the pinned ESP-IDF toolchain**

```bash
source /Users/manhhodinh/esp/esp-idf-v5.5.2/export.sh
idf.py -B build-lcdwiki-es3c35p -D SDKCONFIG_DEFAULTS=sdkconfig.defaults build
```

Use the repository's established `lcdwiki-es3c35p` board selection command if
the build directory already encodes the target. Record the exact command,
binary size, free partition percentage, and SHA-256.

- [ ] **Step 3: Audit the linked image**

Run `nm`/`rg` against the build map and archives. Expected: no unresolved or
referenced `lstat`, `openat`, `fstatat`, `fdopendir`, or `unlinkat`; coordinator
and eviction symbols are linked exactly once.

- [ ] **Step 4: Review and commit build-only fixes**

If fixes were required, repeat Tasks 1-4 focused tests and commit only those
fixes. Do not commit `build*/`, binaries, logs, or generated SDK files.

### Task 7: Flash and Run Attended Storage Concurrency HIL

**Files:**
- Modify: `docs/qa/` evidence index or the existing Task 14 test matrix only
  after real evidence exists.

- [ ] **Step 1: Flash the verified binary**

Use `/dev/cu.usbmodem1101`, verify the connected MAC/UUID before flashing, and
record flash exit status plus binary SHA-256. Do not reuse the previous 2.2.85
hardware proof for this new storage implementation.

- [ ] **Step 2: Run non-destructive refusal cases**

Prove invalid key, prepared lesson, running lesson, concurrent sync, nested
directory, unexpected node, and foreign key/path all refuse without mutation.

- [ ] **Step 3: Run disposable exact-key cases**

On a disposable cache leaf prove absent idempotence, exact deletion, sibling
version/slug/current/PVG/shared preservation, partial retry repair, and final
absence. Never use raw `rm` or generic filesystem deletion.

- [ ] **Step 4: Run interruption cases**

Exercise SD removal and controlled power loss at pre-unlink, mid-unlink, and
pre-rmdir checkpoints. Expected: no false success; after reboot the exact pack
is not ready until retry or sync repair completes.

- [ ] **Step 5: Run 100+ transition soak and log audit**

Alternate prepare/stop, sync, eviction refusal, exact eviction, and repair.
Require no reservation leak, deadlock, watchdog, panic, heap corruption, false
success, or mutation outside the exact key.

- [ ] **Step 6: Hand off to Task 14 cold proof**

Only after a coherent exact `evicted`/`not_found` response and verified healthy
robot state, execute the existing cold sequence. Task 14 remains `NOT PASS`
until `downloadedCount > 0`, all seven heap markers, checksum evidence, strict
timestamps, soak, rollback, and preview parity are captured.
