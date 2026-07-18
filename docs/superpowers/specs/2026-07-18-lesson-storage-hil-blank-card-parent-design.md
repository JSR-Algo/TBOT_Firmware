# Lesson Storage HIL Blank-Card Parent Creation Design

**Date:** 2026-07-18

## Problem

The HIL fixture root is `/sdcard/tbot/lesson-assets`. On an otherwise valid,
mounted, blank microSD card, `/sdcard` exists while `/sdcard/tbot` does not.
The current fixture implementation attempts to create
`/sdcard/tbot/lesson-assets` directly. POSIX `mkdir` is non-recursive, so the
call fails with `ENOENT` and the MCP response reports `status=io_failed` and
`changed=false`.

The host-native fixture suite misses this case because its first test creates
the fixture root's parent directory. Later tests remove only the fixture root,
leaving the parent in place for the rest of the process.

Live evidence from the replacement-card attempt matches this failure mode:
identity, idle status, and inspection passed, then the single authorized
`stage_fixture` call returned the exact response `io_failed/changed=false`.

## Selected Approach

Teach the HIL fixture to manage the missing application namespace parent as an
explicit filesystem component:

1. Treat the mount point (`/sdcard`) as externally owned and never create it.
2. Derive the namespace parent (`/sdcard/tbot`) from the configured fixture root
   rather than adding a second independently configurable absolute path.
3. Validate the namespace parent, fixture root, and fixture slug in order.
4. When missing, create them one component at a time:
   `/sdcard/tbot` -> `/sdcard/tbot/lesson-assets` -> fixture slug.
5. Track which components this stage invocation created.
6. If a later stage operation fails, roll back newly created components in
   leaf-to-root order and report `changed=true` if rollback cannot fully remove
   and re-verify them.

The successful cleanup path may leave the empty `/sdcard/tbot` application
namespace in place. It must not recursively delete the namespace because it is
not exclusively owned by one fixture lifecycle and may later contain other
TBOT data. Existing fixture-root cleanup semantics remain unchanged.

## Fail-Closed Rules

- If `/sdcard` is absent or inaccessible, namespace creation still fails with
  `io_failed`; the fixture must not fabricate a mount point.
- If `/sdcard/tbot` or `/sdcard/tbot/lesson-assets` exists as a non-directory,
  staging returns `unexpected_existing_node` without mutating descendants.
- Symlink protections in native builds remain intact.
- Validation and mutation-lease refusal still occur before filesystem writes.
- No recursive `create_directories`, `mkdir -p`, formatting, remounting, or
  arbitrary deletion is introduced.
- Production behavior outside HIL fixture staging/rollback is unchanged.

## Test Design

Add a host-native regression that uses a root shaped like the device path:

```text
<temporary mount>/sdcard/tbot/lesson-assets
```

The test creates only `<temporary mount>/sdcard`, proving the card is mounted
while `tbot/` is absent. It then requires:

- preservation fixture stage returns `staged`, `changed=true`;
- the namespace, fixture root, slug, both leaves, and both sentinels exist;
- cleanup restores the HIL fixture root to its clean state;
- no path outside the configured mounted-card tree is touched.

Add injected-failure coverage for namespace creation and for failures after the
namespace was created. Each case must prove exact residual truth and rollback
ordering. Run the full native fixture suite, HIL contract tests, local-config
tests, and artifact auditor tests.

## Build And Live Gate

After offline tests pass, build a new HIL profile candidate and validate its
manifest, binary/ELF hashes, HIL symbols, and `lessonStorageHilFaults=true`.
No production image is flashed by Goal 2.

Any HIL reflash requires atomic ownership of
`/tmp/tbot-task14-hardware.lock`. After HIL attestation, run exactly one recovery
preflight. Only a validated `staged/changed=true` followed by
`cleaned/changed=true` permits the cold matrix to start. A new failure is
recorded and releases the lock without retry.

## Alternatives Rejected

1. Pre-creating `/tbot/lesson-assets` manually on every card hides the firmware
   defect and makes blank-card recovery operator-dependent.
2. Recursive directory creation is broader than required and weakens the
   fixture's exact rollback/evidence guarantees.
3. Formatting the card is destructive, unnecessary for this diagnosed parent
   creation bug, and remains unauthorized without separate user approval.
