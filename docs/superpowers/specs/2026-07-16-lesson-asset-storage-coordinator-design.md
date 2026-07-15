# Lesson Asset Storage Coordinator Design

**Status:** Approved direction A for production implementation

**Goal:** Make exact lesson-cache eviction, lesson asset synchronization, and
lesson prepare/runtime mutually safe on ESP-IDF 5.5.2 FATFS without relying on
unsupported POSIX directory-relative APIs or meaningless FAT inode metadata.

## Context

Task 14 needs an attended operation that removes one exact inactive lesson
cache key so a cold assignment can prove a real download. The first firmware
implementation validated paths with `lstat`, rescanned directories, compared
`st_dev`/`st_ino`, and then unlinked by pathname. That design is not valid on
the target:

- ESP-IDF 5.5.2 does not provide linkable `openat`, `fstatat`, `fdopendir`, or
  `unlinkat` implementations for the ESP32-S3 target.
- FAT VFS `stat` does not provide useful device or inode identities.
- FAT has no native symlink type, so host symlink races do not model the target
  filesystem.
- MCP writers execute serially on the application task, but `lesson_worker`
  prepares and reads asset packs on a separate task.
- FAT/VFS locks individual calls, not a multi-call scan/delete transaction.

The production boundary must therefore be an application-owned reservation
covering every lesson-asset reader lifecycle and every writer transaction.

## Threat Model

### In scope

- Concurrent authenticated MCP sync and eviction requests.
- Lesson prepare/start/read racing with sync or eviction.
- Malformed cache keys and asset paths.
- Nested directories, unexpected node types, missing files, and FAT I/O errors.
- Power loss or SD removal during a mutation. The operation must never claim a
  complete eviction unless the exact leaf is verified absent.
- Partial deletion recovery after reboot or retry.

### Outside this change

- An attacker physically modifying or replacing the FAT namespace while the
  firmware is executing a transaction.
- A malicious block device that changes directory entries independently of
  firmware calls.
- Crash-atomic FAT metadata transactions.

The board mounts the SD card once at boot and has no runtime unmount/card
generation service. Supporting hostile media replacement requires a separate
hardware/storage project with card detection, mount generations, and lease
invalidation. Accidental removal remains in scope as an I/O failure and must
not produce a success response.

## Architecture

### Central coordinator

Create one process-wide `LessonAssetStorageCoordinator` for
`/sdcard/tbot/lesson-assets`. It owns a mutex and exactly two mutually exclusive
reservation states:

- one move-only mutation lease; or
- one prepared/running lesson session identified by `assignmentId` and
  `sessionId`.

Acquisition never waits. A conflicting caller gets a stable refusal so network
and lesson tasks cannot deadlock each other.

```cpp
enum class LessonAssetReservationCode {
    kAcquired,
    kMutationActive,
    kLessonSessionActive,
    kLessonSessionMismatch,
};

class LessonAssetMutationLease {
public:
    LessonAssetMutationLease(LessonAssetMutationLease&&) noexcept;
    LessonAssetMutationLease& operator=(LessonAssetMutationLease&&) noexcept;
    ~LessonAssetMutationLease();
    explicit operator bool() const;
    LessonAssetReservationCode code() const;
};

struct LessonAssetSessionResult {
    LessonAssetReservationCode code;
    bool acquired;
    bool idempotent;
};

class LessonAssetStorageCoordinator {
public:
    static LessonAssetStorageCoordinator& GetInstance();

    LessonAssetMutationLease TryBeginMutation(const char* operation);
    LessonAssetSessionResult TryBeginLessonSession(
        const std::string& assignment_id,
        const std::string& session_id
    );
    bool EndLessonSession(
        const std::string& assignment_id,
        const std::string& session_id
    );
    void ForceEndLessonSession();
    bool HasMutation() const;
    bool HasLessonSession() const;
};
```

The lease is RAII and releases exactly once. Duplicate prepare for the same
assignment/session is idempotent. A different assignment/session cannot steal
or release an active reservation. Forced release is limited to device-level
reset/disconnect teardown where the current lesson can no longer continue.

### Lesson lifecycle

`lesson_prepare` performs envelope-only validation first, then reserves the
lesson session before `BuildAssetPackAck` or any asset file read. If reservation
fails, prepare returns a retryable stable error and performs no asset I/O.

The reservation remains active across prepare, start, steps, pause, resume, and
interactive turns. It is released on every terminal or abandoned lifecycle:

- normal lesson stop/complete;
- prepare rejection after reservation;
- explicit lesson failure/cancel;
- connection/reset teardown that invalidates the lesson session.

`IsLessonRuntimeActive()` remains a UI/runtime signal but is not used as the
storage synchronization primitive.

### Mutation lifecycle

Both MCP sync tools and exact eviction acquire a mutation lease before the
first `mkdir`, temporary-file cleanup, download write, `remove`, `rename`,
directory scan, `unlink`, or `rmdir`. The lease spans the complete operation.
If a lesson session exists, the operation refuses before filesystem mutation.

All current writers for the lesson-assets subtree are in `mcp_server.cc` and
must be covered. Contract tests scan production sources so a future unguarded
writer fails CI.

### Exact path policy

The cache key grammar remains byte-exact:

```text
[a-z0-9]+(?:-[a-z0-9]+)*/v[1-9][0-9]*-[0-9a-f]{64}
```

The parser is character-by-character, reconstructs the key, and requires byte
equality. It never trims, normalizes, URL-decodes, or accepts a URI. Invalid
input is not echoed in logs or responses.

On FAT the helper uses `stat`, `opendir`, and `readdir`; it must not use
unsupported `lstat`/`openat` APIs or claim inode identity. Root, slug parent,
and exact leaf must be directories. Every direct child must be a regular file.
Nested directories and unexpected node types refuse before deletion. Only the
exact flat leaf is eligible; root, slug parent, sibling versions, `current.json`,
PVG metadata, and shared stores are never targets.

### Sync path binding

The generic `sync_to_sd` tool currently accepts any path with a broad root
prefix. The revised contract binds every asset to the request's canonical
`cacheKey`:

```text
/sdcard/tbot/lesson-assets/<cacheKey>/<direct-file-basename>
```

Nested asset paths, metadata names, alternate cache keys, root files, slug
parent files, and directory destinations are rejected. Temporary `.tmp` and
`.download` names are internal only and stay in the same exact leaf.

### Partial deletion truth

FAT cannot make multi-file deletion atomic. The result must distinguish a
pre-mutation refusal from a failure after one or more files were removed.

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

struct LessonAssetCacheEvictResult {
    LessonAssetCacheEvictCode code;
    std::string cache_key;
    int file_count;
    bool evicted;
    bool not_found;
};
```

`file_count` is zero for all failures before mutation, the deleted count for a
partial result, and the total deleted count for success. If `rmdir` fails after
files were deleted, the result is partial rather than a clean refusal. A retry
may finish the exact leaf. Success is returned only after a final `stat` proves
the exact leaf is absent with `ENOENT`.

The ESP server treats partial eviction as a definitive failed maintenance
result, never as cold-proof evidence. Task 14 proceeds only after a coherent
`evicted` or idempotent `not_found` result and still requires
`downloadedCount > 0`.

## Error and Logging Contract

- Reservation conflicts use stable public codes and contain no internal paths.
- Invalid cache keys return an empty `cacheKey` and logs omit the input.
- Validated keys may be logged with stable code and deleted count.
- No bearer token, JWT, local absolute artifact path, or arbitrary exception is
  returned to the operator evidence flow.
- SD removal/read/write errors never map to `evicted` or `not_found` unless the
  final exact-leaf absence check is authoritative.

## Verification

### Native coordinator tests

- mutation versus mutation: one winner, one immediate refusal;
- mutation versus lesson prepare: one winner, no interleaving;
- same-session duplicate prepare is idempotent;
- foreign session cannot replace or release the owner;
- move-only lease releases once, including early return and exception paths;
- terminal and forced teardown do not leak reservations.

### Native eviction and sync tests

- full backend parser invalid matrix, including embedded NUL;
- active/prepared lesson refusal before mutation;
- absent idempotence;
- exact flat-leaf deletion and deleted count;
- root/slug/sibling/current/PVG/shared preservation;
- nested and unexpected node refusal with zero mutation;
- deterministic scan, unlink, rmdir, and partial-delete failures;
- final absence verification;
- retry repairs a partial leaf;
- sync rejects paths outside the exact cache leaf and metadata targets.

### Contract tests

- every production lesson-assets writer is inside a mutation-lease scope;
- lesson prepare reserves before `BuildAssetPackAck` and all asset reads;
- every terminal/failure path releases the exact session;
- only the exact user-only eviction tool bypasses the generic runtime MCP guard;
- result JSON contains the stable six-field envelope;
- unsupported POSIX symbols and FAT inode claims are absent.

### Target and hardware gates

- ESP-IDF 5.5.2 build for `lcdwiki-es3c35p` links without unsupported symbols;
- invalid, prepared/running, absent, exact-leaf, sibling/current/PVG preservation;
- concurrent prepare versus eviction and sync versus eviction;
- SD removal and controlled power loss never produce false success;
- retry/sync repairs partial state;
- 100+ transition soak with log audit and no reservation leak.

## Rollout

Exact eviction remains an authenticated, attended maintenance operation used by
Task 14 and recovery tooling. Immutable versioned cache identities remain the
normal production delivery path. No unattended background GC is introduced by
this change.
