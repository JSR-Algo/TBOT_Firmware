# Lesson Storage HIL Blank-Card Parent Creation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make HIL fixture staging work on a mounted blank microSD by creating the missing TBOT namespace hierarchy one component at a time with exact rollback evidence.

**Architecture:** Extend the existing non-recursive fixture parent state from two components to three: TBOT namespace, lesson-assets root, and fixture slug. The mount point remains externally owned. Host-native tests use a device-shaped temporary path so a missing namespace is exercised before any test can accidentally create it.

**Tech Stack:** C++17, POSIX filesystem APIs used by ESP-IDF FAT VFS, host-native clang++ tests, pytest contract tests, ESP-IDF 5.5.2 HIL build and artifact auditor.

---

## File Map

- Modify `scripts/run_host_native_lesson_storage_hil_fixture_test.sh`: compile the native fixture against a device-shaped root beneath an explicitly created temporary mount point.
- Modify `tests/native/lesson_storage_hil_fixture_host_test.cc`: add blank-card success and namespace rollback regression coverage before parent-directory test pollution occurs.
- Modify `main/lesson_storage_hil_fixture.cc`: derive and validate the namespace parent, create each missing component in order, and include the namespace in rollback evidence.
- Modify `tests/test_lesson_storage_hil_contract.py`: freeze the non-recursive namespace-parent behavior and mount-point ownership boundary.
- Create `docs/evidence/lesson-storage-hil-blank-card-parent.md`: record RED/GREEN commands, hashes, HIL candidate identity, and live disposition without changing the parent Goal 2 files.

### Task 1: Reproduce A Mounted Blank Card In The Native Harness

**Files:**
- Modify: `scripts/run_host_native_lesson_storage_hil_fixture_test.sh`
- Modify: `tests/native/lesson_storage_hil_fixture_host_test.cc`

- [ ] **Step 1: Change the native root to include the missing namespace**

In `scripts/run_host_native_lesson_storage_hil_fixture_test.sh`, replace the single-level root with:

```bash
MOUNT_POINT="${BUILD_DIR}/sdcard"
STORAGE_ROOT="${MOUNT_POINT}/tbot/lesson-assets"
mkdir -p "${MOUNT_POINT}"
```

This creates only the simulated mounted card. It must not pre-create `tbot` or `lesson-assets`.

- [ ] **Step 2: Add a failing blank-card regression first in `main()`**

Add these helpers beside `Root()`:

```cpp
fs::path NamespaceRoot() { return Root().parent_path(); }
fs::path MountPoint() { return NamespaceRoot().parent_path(); }

void ResetBlankMountedCard() {
    std::error_code error;
    fs::remove_all(NamespaceRoot(), error);
    Expect(!error, "blank-card namespace reset failed");
    fs::create_directories(MountPoint(), error);
    Expect(!error, "blank-card mount point creation failed");
}
```

Add the regression:

```cpp
void TestMountedBlankCardCreatesNamespaceHierarchy() {
    ResetBlankMountedCard();
    ResetMutationInjection();
    const std::string key = Key("hil-blank", 1, 'a');
    const std::string sibling = Key("hil-blank", 2, 'b');
    auto lease = Lease();
    const auto staged = StageLessonStorageHilFixture(
        lease, key, LessonStorageHilFixture::kPreservationSet, sibling
    );
    Expect(staged.code == LessonStorageHilFixtureCode::kStaged && staged.changed,
           "mounted blank card did not stage preservation fixture");
    Expect(fs::is_directory(NamespaceRoot()), "TBOT namespace was not created");
    Expect(fs::is_directory(Root()), "lesson-assets root was not created");
    Expect(fs::is_regular_file(Leaf(key) / ".tbot-hil-sentinel"),
           "primary preservation sentinel missing");
    Expect(fs::is_regular_file(Leaf(sibling) / ".tbot-hil-sentinel"),
           "sibling preservation sentinel missing");
}
```

Call `TestMountedBlankCardCreatesNamespaceHierarchy()` before `TestValidationAndLeaseRefusalPrecedeFilesystemAccess()` in `main()`.

- [ ] **Step 3: Run the native test and verify RED**

Run:

```bash
./scripts/run_host_native_lesson_storage_hil_fixture_test.sh
```

Expected: exit non-zero with `mounted blank card did not stage preservation fixture`; the current direct `mkdir(.../tbot/lesson-assets)` fails because `.../tbot` is absent.

- [ ] **Step 4: Commit the RED test checkpoint**

```bash
git add scripts/run_host_native_lesson_storage_hil_fixture_test.sh \
  tests/native/lesson_storage_hil_fixture_host_test.cc
git commit -m "test(firmware): reproduce blank-card HIL parent failure"
```

### Task 2: Create And Validate The Namespace Parent

**Files:**
- Modify: `main/lesson_storage_hil_fixture.cc`

- [ ] **Step 1: Add explicit three-component parent state**

Replace `ParentCreation` and the boolean parent outputs with:

```cpp
struct ParentState {
    bool namespace_missing = false;
    bool root_missing = false;
    bool slug_missing = false;
};

struct ParentCreation {
    bool namespace_created = false;
    bool root_created = false;
    bool slug_created = false;
};
```

Add a derived namespace path helper beside `RootPath()`:

```cpp
std::string ParentPath(const std::string& path) {
    const std::size_t separator = path.rfind('/');
    return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

std::string NamespacePath() {
    return ParentPath(RootPath());
}
```

- [ ] **Step 2: Validate namespace, root, and slug in order**

Change `ValidateParents` to accept `ParentState* state`. Require a non-empty derived namespace path. Read `NamespacePath()` first and return `kIoFailed` or `kUnexpectedExistingNode` for unreadable or non-directory nodes. Then retain the existing exact checks for `RootPath()` and `SlugPath(slug)`, setting the three `*_missing` fields.

Use this exact state transition:

```cpp
const NodeKind namespace_kind = ReadNodeKind(NamespacePath());
if (namespace_kind == NodeKind::kIoFailed) {
    return LessonStorageHilFixtureCode::kIoFailed;
}
if (namespace_kind != NodeKind::kMissing &&
    namespace_kind != NodeKind::kDirectory) {
    return LessonStorageHilFixtureCode::kUnexpectedExistingNode;
}
state->namespace_missing = namespace_kind == NodeKind::kMissing;
```

- [ ] **Step 3: Create parents one component at a time**

Change `EnsureParents` to accept `const ParentState& state` and perform:

```cpp
if (state.namespace_missing) {
    if (CreateFixtureDirectory(NamespacePath()) != 0) {
        return false;
    }
    creation->namespace_created = true;
}
if (state.root_missing) {
    if (CreateFixtureDirectory(RootPath()) != 0) {
        return false;
    }
    creation->root_created = true;
}
if (state.slug_missing) {
    if (CreateFixtureDirectory(SlugPath(slug)) != 0) {
        return false;
    }
    creation->slug_created = true;
}
```

- [ ] **Step 4: Update all stage and cleanup call sites**

In `StageNested`, `StageLeafFile`, `StagePreservation`, `CleanupNested`, `CleanupLeafFile`, and `CleanupPreservation`, replace the two booleans with one `ParentState state`. Existing “parents missing means leaf missing/clean” checks become `state.root_missing || state.slug_missing`; namespace missing necessarily implies root missing after validation.

- [ ] **Step 5: Include namespace creation in every stage rollback**

Declare:

```cpp
const std::string namespace_path = NamespacePath();
```

For every `RollBackCreatedNodes` list in all three stage functions, append the namespace entry last so leaf-to-root deletion order is preserved:

```cpp
{&namespace_path,
 NodeKind::kDirectory,
 creation.namespace_created,
 state.namespace_missing && !creation.namespace_created},
```

For an `EnsureParents` failure, set `creation_attempt_failed` only when all earlier required parents were created, matching the existing root/slug residual-truth logic.

- [ ] **Step 6: Run the focused native test and verify GREEN**

Run:

```bash
./scripts/run_host_native_lesson_storage_hil_fixture_test.sh
```

Expected: exit `0` and a `lesson storage HIL fixture host checks:` line with a
strictly positive integer count.

- [ ] **Step 7: Commit namespace creation**

```bash
git add main/lesson_storage_hil_fixture.cc \
  scripts/run_host_native_lesson_storage_hil_fixture_test.sh \
  tests/native/lesson_storage_hil_fixture_host_test.cc
git commit -m "fix(firmware): stage HIL fixtures on blank mounted cards"
```

### Task 3: Prove Namespace Rollback Truth

**Files:**
- Modify: `tests/native/lesson_storage_hil_fixture_host_test.cc`

- [ ] **Step 1: Add injected namespace-creation failure coverage**

Add a test that resets to a blank mounted card, sets `g_fail_mkdir_call = 1`, stages a preservation fixture, and requires `kIoFailed`, `changed=false`, no namespace, no root, and exactly one mkdir call.

```cpp
void TestBlankCardNamespaceCreationFailureIsNonMutating() {
    ResetBlankMountedCard();
    ResetMutationInjection();
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    g_fail_mkdir_call = 1;
    auto lease = Lease();
    const auto result = StageLessonStorageHilFixture(
        lease, Key("hil-blank-fail", 1, 'a'),
        LessonStorageHilFixture::kPreservationSet,
        Key("hil-blank-fail", 2, 'b')
    );
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && !result.changed,
           "namespace creation failure reported mutation");
    Expect(!fs::exists(NamespaceRoot()),
           "namespace creation failure left a residual");
    Expect(g_mkdir_calls == 1, "namespace creation failure continued mutation");
}
```

- [ ] **Step 2: Add failure-after-namespace rollback coverage**

Set `g_fail_mkdir_call = 2` so namespace creation succeeds and lesson-assets root creation fails. Require `kIoFailed`, `changed=false`, and namespace removal. Then set `g_create_then_fail_mkdir_call = 2`; require `kIoFailed`, `changed=true`, because an untracked root may remain and exact rollback cannot prove a clean state.

- [ ] **Step 3: Run the focused suite**

Run:

```bash
./scripts/run_host_native_lesson_storage_hil_fixture_test.sh
```

Expected: PASS with the increased check count.

- [ ] **Step 4: Commit rollback tests**

```bash
git add tests/native/lesson_storage_hil_fixture_host_test.cc
git commit -m "test(firmware): cover blank-card parent rollback truth"
```

### Task 4: Freeze The Contract And Run Software Verification

**Files:**
- Modify: `tests/test_lesson_storage_hil_contract.py`

- [ ] **Step 1: Add static contract assertions**

Add a test requiring `NamespacePath`, `ParentState`, `namespace_missing`, and `namespace_created` in `lesson_storage_hil_fixture.cc`. Require that the fixture still uses `CreateFixtureDirectory` and does not introduce `std::filesystem::create_directories`, `mkdir -p`, `system(`, or `format_if_mount_failed`.

- [ ] **Step 2: Run focused firmware verification**

Run:

```bash
./scripts/run_host_native_lesson_storage_hil_fixture_test.sh
python3 -m pytest -q \
  tests/test_lesson_storage_hil_contract.py \
  tests/test_lesson_storage_hil_local_config.py \
  tests/test_lesson_storage_hil_artifact_auditor.py
```

Expected: all commands exit `0`.

- [ ] **Step 3: Run diff and secret checks**

Run:

```bash
git diff --check HEAD~2..HEAD
rg -n '(Bearer [A-Za-z0-9._-]{20,}|eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,})' \
  main/lesson_storage_hil_fixture.cc \
  tests/native/lesson_storage_hil_fixture_host_test.cc \
  tests/test_lesson_storage_hil_contract.py
```

Expected: diff check exits `0`; credential scan has no matches.

- [ ] **Step 4: Commit contract coverage**

```bash
git add tests/test_lesson_storage_hil_contract.py
git commit -m "test(firmware): freeze blank-card HIL parent contract"
```

### Task 5: Build And Attest A New HIL Candidate

**Files:**
- Create (generated, uncommitted): `build-task14-hil-blank-parent/`
- Create: `docs/evidence/lesson-storage-hil-blank-card-parent.md`

- [ ] **Step 1: Generate the LAN-only HIL overlay**

```bash
mkdir -p build-task14-hil-blank-parent
python3 scripts/generate_lesson_storage_hil_local_config.py \
  --ota-url http://192.168.100.209:8003/tbot/ota/ \
  --websocket-url ws://192.168.100.209:8000/tbot/v1/ \
  --output build-task14-hil-blank-parent/sdkconfig.defaults.hil-local
```

- [ ] **Step 2: Build the HIL profile**

```bash
source /Users/manhhodinh/esp/esp-idf-v5.5.2/export.sh
idf.py -B build-task14-hil-blank-parent \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local;sdkconfig.defaults.hil-storage;build-task14-hil-blank-parent/sdkconfig.defaults.hil-local' \
  build
```

Expected: build exit `0`; no flash command is run.

- [ ] **Step 3: Generate and validate the HIL manifest**

Run the repository's artifact auditor against `build-task14-hil-blank-parent`, requiring profile `hil`, `configEnabled=true`, all HIL symbols/tool literals present, and a non-production banner. Record the resulting `lesson-storage-hil-build.json` and sidecar SHA-256.

```bash
python3 scripts/assert_lesson_storage_hil_artifacts.py \
  --build-dir build-task14-hil-blank-parent \
  --profile hil
```

Expected: JSON `status=PASS` and exit `0`.

- [ ] **Step 4: Record candidate evidence**

Write `docs/evidence/lesson-storage-hil-blank-card-parent.md` with the RED failure, GREEN test counts, source commit, build manifest path, binary/ELF SHA-256, and explicit statements that no robot, SD, lock, or production image was touched during implementation/build.

- [ ] **Step 5: Commit implementation evidence**

```bash
git add docs/evidence/lesson-storage-hil-blank-card-parent.md
git commit -m "docs(firmware): record blank-card HIL candidate evidence"
```

### Task 6: HIL Reflash And Single Recovery Gate

**Files:**
- Evidence: `/Users/manhhodinh/Documents/TBOT/robot/docs/evidence/artifacts/lesson-hardware-resilience/20260718T035911Z/blank-parent-fix-${RUN_ID}/`

- [ ] **Step 1: Acquire the hardware lock for HIL app reflash**

Atomically create `/tmp/tbot-task14-hardware.lock`, write a mode-`0600` owner record, and install a trap that releases only the owned lock inode. Stop if the lock exists. Refresh/source the authorized credential files without printing values.

- [ ] **Step 2: Revalidate the HIL candidate before mutation**

Re-run the artifact auditor, verify the manifest sidecar, confirm target `esp32s3`, profile `hil`, `lessonStorageHilFaults=true`, and record the candidate app offset `0x20000`. Verify `/dev/cu.usbmodem1101` is the expected attended device.

- [ ] **Step 3: Flash only the HIL application image**

Use ESP-IDF's pinned esptool from the current environment to write only the candidate `xiaozhi.bin` at `0x20000`. Preserve bootloader, partition table, NVS, and OTA data. Capture command, start/end UTC, exit code, and redacted esptool log. Release the flash lock on every terminal path.

- [ ] **Step 4: Attest the running HIL image**

After reboot, capture serial boot evidence and require the non-production HIL banner plus candidate ELF/build identity. Require the expected UUID/MAC live connection. If attestation fails, release the lock and stop; do not run recovery.

- [ ] **Step 5: Run exactly one recovery preflight under a fresh atomic lock**

Invoke `robot/scripts/lesson_task14_post_reseat_recovery.py` with the replacement-card identities and the cold scenario as its argv child. Require `stage_fixture: staged/changed=true`, `cleanup_fixture: cleaned/changed=true`, clean post-inspection, and atomic `READY_FOR_COLD` evidence before cold starts.

- [ ] **Step 6: Continue or stop fail-closed**

If recovery passes, continue the bounded HIL matrix from `T14-LIVE-02 cold` while retaining the lock for each active scenario. If recovery fails, record the exact `failureEvidence.fixtureResponse`, keep `T14-LIVE-02 FAIL` and `T14-LIVE-03..11 NOT RUN`, release the lock, and do not retry.

Production reflash and production soak remain owned by Goal 3 and are never performed by this plan.
