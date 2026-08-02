# Lesson Non-Hardware Completion Design

## Objective

Complete and verify the planned lesson workflow across the admin website, backend,
ESP server, and firmware host runtime while the physical robot is unavailable.
All software gates must pass except checks that inherently require the robot,
real display/audio, BLE/Wi-Fi provisioning, a physical SD card, or HIL equipment.

## Current State

- Backend tests, lint, typecheck, and build pass after correcting stale test mocks.
- Admin lesson contract suites and production build pass.
- ESP server pytest passes under the project Python environment with the canonical
  backend manifest configured.
- Firmware pytest and both ESP-IDF builds pass.
- Firmware host lesson coverage remains the only software blocker: `93.6%` line
  coverage while the existing gate requires `100%`.

The uncovered firmware lines are a mixture of reachable scenarios, deterministic
failure paths without host controls, exhaustive defensive fallbacks, and duplicated
or unreachable session-reservation cleanup.

## Selected Approach

Use a narrow hybrid approach that preserves the `100%` coverage contract:

1. Add characterization tests for reachable lesson paths using the existing host
   display, scheduler, renderer, storage coordinator, and callback fakes.
2. Add narrowly scoped controls compiled only with `TBOT_HOST_NATIVE_COVERAGE` for
   failures that cannot otherwise be driven deterministically, including cJSON
   allocation/serialization failures and visual-completion nonce wraparound.
3. Remove only duplicated or unreachable branches after a failing characterization
   test captures the required externally observable behavior.
4. Keep every extracted helper or new source file inside the coverage filters so
   refactoring cannot reduce the measured denominator.
5. Do not lower thresholds, add coverage exclusions, or weaken protocol assertions.

This approach is preferred over a broad dependency-injection rewrite because
`lesson_handler.cc` is a large, high-risk orchestration unit. Host-only compile
controls must produce no production `.text`, `.data`, or `.bss` increase.

## Coverage Work Groups

### Existing Host Controls

Add tests for:

- invalid visual generations and alternate opening layouts;
- rejected and timed-out visual callbacks;
- optional identity fields and ACK replay-window eviction;
- abandoned storage sessions and reservation conflicts;
- cinematic phase/error mappings and rejected prepare cleanup;
- duplicate frames while a visual ACK is pending;
- renderer-v2 opening assets, identity mismatch, resume, and repeated start;
- stale scheduled callbacks and non-LVGL degraded completion.

### Narrow Host-Only Controls

Add deterministic host coverage controls for:

- cJSON object creation, item insertion, and serialization failure;
- visual-completion nonce wraparound;
- cached-ACK fallback state only if it cannot be reached through public frame flow.

These controls must be absent when `TBOT_HOST_NATIVE_COVERAGE` is not defined.

### Redundant Or Unreachable Paths

The repeated normal `lesson_prepare` reservation block and cross-renderer cleanup
paths must be traced against their callers. Remove them only when tests demonstrate
that current valid, conflict, and failure responses remain unchanged. Exhaustive
enum fallbacks remain fail-closed unless a total helper makes the invariant explicit.

## Cross-Repository Regression Scope

After firmware coverage reaches `100%`, rerun the complete non-hardware lesson path:

- Backend: full Vitest, lint, typecheck, and production build.
- Admin: lesson studio, TVideo template/journey, cinematic derivative, SD-sync,
  visual selection, cache policy, and production build suites.
- ESP server: full pytest with the supported Python environment and canonical
  backend manifest.
- Firmware: full `tests/` pytest, all applicable host-native lesson runners,
  host coverage, and ESP-IDF build.
- Servant firmware: ESP-IDF build.

## Verification Gates

The work is complete only when:

- host lesson line coverage is exactly `100%` with the existing threshold;
- no coverage ignore or denominator reduction is introduced;
- firmware pytest reports zero failures;
- backend, admin, and ESP server lesson suites report zero failures;
- firmware and Servant builds exit successfully;
- production firmware size comparison shows no host-seam `.text`, `.data`, or
  `.bss` regression;
- independent review finds no weakened assertions or unreachable-code mistakes;
- `git diff --check` passes in every changed repository.

## Deferred Hardware Verification

The following checks are explicitly deferred until the robot is connected:

- flashing and boot verification;
- real TFT layer playback, frame pacing, and RAM/PSRAM observation;
- speaker, microphone, and Google Live interaction;
- BLE/Wi-Fi provisioning and claim flow on-device;
- physical SD-card synchronization and corruption recovery;
- full lesson playback and HIL timing on the robot.

No software test may claim these hardware results by simulation.

## Risk Controls

- Preserve all unrelated user changes and untracked package-manager files.
- Use RED/GREEN TDD for every production change.
- Make one behavioral change at a time and rerun focused coverage after each group.
- Stop and revisit the design if three attempted fixes expose new architectural
  coupling instead of closing a known coverage cluster.
