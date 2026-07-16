# Lesson Storage HIL Fault Seams Design

**Status:** Approved approach 1; written specification awaiting user review

**Goal:** Add deterministic, attended hardware-in-the-loop controls that can
prove lesson-storage interruption and recovery behavior without exposing
arbitrary filesystem mutation or shipping test controls in production firmware.

## Context

The production lesson-storage implementation now coordinates lesson sessions,
sync writers, and exact cache eviction under one process-wide reservation. Host
tests can deterministically inject scan, allocation, unlink, `rmdir`, download,
and staging failures. Those seams intentionally compile out on ESP32.

Task 14 still requires live proof at named physical checkpoints:

- before the first unlink;
- after one or more successful unlinks;
- after all file removals but before leaf `rmdir`;
- interrupted sync, checksum failure, and bounded SD-full behavior;
- nested-directory and unexpected-node refusal;
- exact-leaf, sibling, PVG, current metadata, and shared-store preservation.

Random power cuts or SD removal cannot prove which checkpoint was reached.
Production APIs also cannot safely construct the invalid fixtures needed for
the refusal matrix. A separate HIL build is therefore required.

## Safety Boundary

Add one Kconfig switch:

```text
CONFIG_TBOT_HIL_STORAGE_FAULTS
```

It is a boolean with default `n`. No committed production/default sdkconfig may
enable it. Kconfig restricts it to the ESP32-S3 `lcdwiki-es3c35p` attended lab
target so it cannot be enabled accidentally for unrelated boards. When
disabled:

- HIL source bodies and MCP registrations are not compiled;
- HIL tool-name strings, checkpoint symbols, fixture sentinels, and boot banner
  are absent from the linked image;
- production lesson sync and eviction behavior remains byte-for-byte governed
  by the existing contracts.

The HIL profile uses a separate sdkconfig defaults file and build directory. A
boot warning and system-info capability identify the image as non-production.
After fault testing, the robot must be reflashed with a clean production build
from the same reviewed source commit before soak, rollback, or release evidence
can be accepted.

## Disposable-Key Policy

Every HIL operation requires a canonical lesson cache key whose slug starts
with `hil-`. The existing cache-key parser remains authoritative. The HIL layer
does not normalize, truncate, URL-decode, or accept raw paths.

The allowed root is fixed at:

```text
/sdcard/tbot/lesson-assets/<canonical-hil-cache-key>
```

The API never accepts a filesystem path, basename, parent directory, arbitrary
delete target, or recursive option. A non-`hil-` key is rejected before lease
acquisition or filesystem access and is not echoed if it is non-canonical.

All fixture staging and cleanup acquires the existing non-waiting mutation
lease. It refuses while a lesson session or another writer is active.

## HIL Controller

Create a small process-wide `LessonStorageHilController`, compiled only when
the Kconfig switch is enabled. It stores one volatile, one-shot arm record:

```cpp
enum class LessonStorageHilOperation {
    kEvict,
    kSync,
};

enum class LessonStorageHilCheckpoint {
    kBeforeFirstUnlink,
    kAfterUnlinks,
    kBeforeRmdir,
    kBeforeDownloadWrite,
    kAfterDownloadBytes,
    kBeforeChecksumVerify,
    kBeforeCommitRename,
};

enum class LessonStorageHilAction {
    kFail,
    kPause,
    kNoSpace,
    kCorruptStaging,
};
```

The arm record binds exactly one canonical HIL cache key, operation,
checkpoint, action, optional positive count/byte threshold, and a bounded pause
duration. Configuration is fully validated and allocated before it becomes
active. Arming never touches the SD card.

The controller consumes the record at most once. A checkpoint for a foreign
cache key or operation cannot consume or affect it. Reset/reboot clears the
volatile arm; no NVS state or secret is written.

Checkpoint observation must not allocate inside the destructive eviction
phase. The eviction integration reads prevalidated fixed-size state and updates
only scalar reached/consumed flags. Existing truthful partial-count behavior
remains authoritative.

The compatibility matrix is closed rather than permissive:

| Operation | Checkpoint | Allowed actions |
|---|---|---|
| Evict | before first unlink | fail, pause |
| Evict | after N unlinks | fail, pause; N is 1-64 |
| Evict | before `rmdir` | fail, pause |
| Sync | before download write | fail, pause, no-space |
| Sync | after N downloaded bytes | fail, pause, no-space; N is positive and bounded by the declared asset size |
| Sync | before checksum verification | fail, pause, corrupt-staging |
| Sync | before commit rename | fail, pause, no-space |

Every other combination is rejected while arming. Zero, negative, boolean, or
out-of-range numeric inputs are invalid.

## Checkpoint Actions

### Fail

`kFail` returns a deterministic local I/O failure at the selected checkpoint.
Before mutation it reports a zero-count refusal. After successful unlinks it
must flow through the existing partial-result path and preserve the exact
deleted count. It may not invent a new success or `not_found` state.

### Pause

`kPause` emits a stable serial marker containing only operation, checkpoint,
canonical HIL key, and completed count, then waits for a bounded 5-60 second
window. It is intended for attended SD removal or power removal:

```text
HIL_STORAGE_CHECKPOINT_REACHED operation=... checkpoint=... count=...
```

No MCP resume call is required because the application task may be blocked at
the checkpoint. If power remains on, the operation continues after the bounded
window and normal FAT errors determine the result. If power is removed, the
missing response is not success evidence; reboot/retry must attest the exact
leaf.

The pause uses a yielding RTOS delay, never a busy loop. It emits the reached
marker before sleeping and a separate continued marker after the window. The
watchdog must remain serviced through existing task/runtime behavior.

### No Space

`kNoSpace` is valid only for sync write checkpoints. It produces the same
sanitized local failure path as an SD `ENOSPC` condition without filling the
physical card. It is one-shot and exact-key-bound. No global free-space value
or quota is changed.

### Corrupt Staging

`kCorruptStaging` is valid only immediately before checksum verification. It
changes one byte in the temporary HIL staging file, flushes the change, then
allows the normal SHA-256 verifier to reject it. The verified destination
must not be replaced and staging/backup recovery remains responsible for
cleanup. It cannot target an existing non-HIL asset.

## User-Only HIL Tools

When enabled, register a separate namespace in `AddUserOnlyTools`:

```text
self.lesson_assets.hil.arm_fault
self.lesson_assets.hil.status
self.lesson_assets.hil.stage_fixture
self.lesson_assets.hil.cleanup_fixture
self.lesson_assets.hil.inspect
```

`arm_fault` accepts only structured enum-like strings, a canonical HIL cache
key, and bounded integer values. `status` is read-only and returns stable
fields describing armed/reached/consumed state without paths or exception text.

`stage_fixture` supports only these named fixtures:

- `nested_directory`: create one fixed sentinel directory directly inside the
  exact HIL leaf;
- `leaf_regular_file`: create the exact HIL leaf as a fixed-content regular
  file so eviction returns `unexpected_node_type`;
- `preservation_set`: create two canonical HIL sibling leaves containing only
  fixed sentinel regular files.

For `preservation_set`, the request supplies a second canonical HIL cache key.
Both keys must have the same slug, different versions, and the `hil-` prefix;
neither may already contain unknown entries. Other fixture modes accept only
one key.

`cleanup_fixture` accepts the cache key and fixture name, verifies the exact
sentinel name and fixed magic content, and removes only nodes created by that
fixture. It is not recursive and cannot delete normal lesson assets. An exact
leaf containing any unknown entry fails closed and requires attended recovery.

`inspect` is read-only and accepts no path. It returns bounded fingerprints for
the requested exact HIL leaf, its canonical HIL sibling fixture, and a fixed
compiled list of protected lesson-storage locations such as `current.json`,
PVG metadata, and the shared store. It reports only stable relative labels,
node type, byte count, and SHA-256 for regular files; it never returns an
absolute path or recursively walks unknown directories. Before/after
fingerprints provide preservation evidence without creating or mutating fake
protected metadata.

The authenticated generic internal MCP proxy may invoke these user-only tools
only for the configured lab MAC allowlist. No public or parent-facing HTTP
endpoint is added.

## Production Exclusion Contract

CI and release verification must prove all of the following for the production
build:

- `CONFIG_TBOT_HIL_STORAGE_FAULTS` is unset;
- the five HIL MCP tool strings are absent from `xiaozhi.bin`, ELF, map, and
  main archive;
- controller/checkpoint/sentinel symbols are absent from `nm` output;
- no HIL defaults file participates in the production build command;
- normal eviction, sync, cJSON OOM, coordinator, and lesson-handler suites pass.

The HIL build must prove the inverse: the config is enabled, the boot warning
is present, and all five tools register exactly once. HIL and production binary
hashes are recorded separately and never substituted for one another.

## Error and Evidence Contract

- HIL validation errors are stable and sanitized; arbitrary exception strings,
  absolute local paths, credentials, and bearer values are never returned.
- Only canonical HIL keys may be echoed.
- Every arm, reach, consume, refusal, and cleanup event has a stable serial log
  marker and boot-scoped monotonic sequence number.
- A reached pause followed by lost power is recorded as interrupted, never as a
  successful eviction or sync.
- After reboot, only coherent production `evicted` or `not_found` followed by a
  verified resync may restore cold-proof eligibility.
- Each evidence bundle records source commit, sdkconfig hash, HIL/production
  binary hash, MAC, UUID, timestamps, command exit status, raw sanitized result,
  serial/server logs, and artifact SHA-256 values.

## Test Strategy

### Host-native RED/GREEN tests

- default controller has no arm and cannot affect an operation;
- invalid/non-HIL/foreign keys cannot arm or consume;
- every operation/checkpoint/action compatibility rule is enforced;
- arm is one-shot and reset clears it;
- before-first-unlink failure mutates zero files;
- after-N-unlinks failure returns exact partial count;
- pre-`rmdir` failure returns partial after all file removals;
- pause emits one reached marker and honors bounds;
- sync no-space and staging corruption preserve the verified destination;
- mutation lease and directory/file resources release on every failure;
- fixture cleanup refuses unknown entries and never recurses.

### Static and target contracts

- Kconfig defaults OFF;
- HIL registrations and source hooks are fully compile-guarded;
- production build contains no HIL tool string or symbol;
- HIL build contains the warning/capability and exactly one registration per
  tool;
- unsupported FAT/POSIX APIs remain absent;
- production image size still satisfies the app-partition gate.

### Attended HIL sequence

1. Flash and attest the HIL binary on the expected MAC/UUID.
2. Run invalid, prepared/running, concurrent-sync, nested, unexpected-node,
   foreign-key, exact deletion, absence, sibling, PVG/current/shared
   preservation, partial retry, no-space, checksum, SD-removal, and power-loss
   scenarios using disposable HIL keys only.
3. Validate every evidence directory with the Task 14 fault driver.
4. Flash and attest the production binary built from the same source commit
   with HIL disabled.
5. Run cold/warm/offline/rollback, preview parity, and at least 104 lesson-step
   transitions on the production image.
6. Reject release on any reset, watchdog, heap corruption, false success,
   reservation leak, monotonic PSRAM loss above the agreed bound, or internal
   SRAM minimum below the provisional 20 KiB gate.

## Non-Goals

- General-purpose filesystem shell, raw `rm`, recursive cleanup, arbitrary
  path creation, or production maintenance API.
- Persisting fault arms across reboot.
- Shipping HIL controls in production builds.
- Treating a HIL binary soak as production release evidence.
- Editing or depending on `manager-mobile`.
