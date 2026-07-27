# Renderer V2 Memory Stability Evidence

## Disposition

- Host gate: `PASS` using deterministic simulated allocator and LVGL counters.
- Hardware-in-the-loop gate: `PENDING`; no robot was flashed or measured for this task.
- The host byte counts below are test fixtures, not ESP32-S3 measurements. They prove threshold evaluation, lifecycle accounting, and zero C++ allocations inside the animation-frame callback boundary.

## Source And Toolchain

- Measured base commit: `7baa665f58445e3c678c034df12e853114d7f6b5`.
- Candidate commit: the commit containing this document (`git rev-parse HEAD` after checkout); the code, test, script, CMake linkage, and evidence are committed atomically.
- Host compiler: `Apple clang version 21.0.0 (clang-2100.1.1.101)`, target `arm64-apple-darwin25.2.0`.
- Production-object cross compiler: `xtensa-esp-elf-g++ (crosstool-NG esp-14.2.0_20251107) 14.2.0`.
- ESP-IDF checkout available for production compilation: `v5.5.2-dirty`. The `dirty` suffix describes the local toolchain checkout and is not presented as an upstream release identity.

## Machine-Readable Host Report

```json
{"schemaVersion":1,"measurementKind":"simulated-host","passed":true,"cycles":{"cancel":100,"complete":100},"actual":{"frameAllocations":0,"maxLiveDecodedLayers":3,"maxSettledAnimations":0,"maxSettledContexts":0,"internalHeapLossBytes":1024,"psramLossBytes":2048,"largestInternalBlockBytes":90112,"minInternalFreeBytes":126976,"settledObservationsAt500ms":200,"forbiddenMarkersFound":false},"thresholds":{"maxFrameAllocations":0,"maxLiveDecodedLayers":3,"maxSettledAnimationsAndContexts":0,"maxInternalHeapLossBytes":4096,"maxPsramLossBytes":8192,"minLargestInternalBlockBytes":65536,"minInternalFreeBytes":49152}}
```

The report comes directly from `./scripts/run_host_native_lesson_memory_test.sh`. The deterministic host model holds three decoded layers during an entrance, one animation and one context until termination, and samples cleanup at 500 ms. Every frame advances the real `lesson_tvideo::StateMachine`; test-only global `new` instrumentation feeds the probe's per-frame allocation counter.

## Verification

Commands run from the firmware worktree:

```text
./scripts/run_host_native_lesson_memory_test.sh
PASS: JSON report above

SANITIZE=1 ./scripts/run_host_native_lesson_memory_test.sh
PASS: AddressSanitizer and UndefinedBehaviorSanitizer, same JSON report

CXX=g++ ./scripts/run_host_native_lesson_memory_test.sh
PASS: compiler-driver cross-check, same JSON report

ninja -C /tmp/tbot-renderer-memory-build esp-idf/main/CMakeFiles/__idf_main.dir/lesson_renderer_memory_probe.cc.obj
PASS: production branch cross-compiled to a 59,440-byte Xtensa object

./scripts/run_host_native_lesson_visual_animation_test.sh
PASS: lesson visual animation host test passed: 106 checks

./scripts/run_host_native_lesson_coverage.sh
PRE-EXISTING GATE FAILURE: 94.6% line coverage (2096/2216), below the script's hardcoded 100% threshold
```

The coverage failure is in existing `lesson_handler.cc` branches and is outside the Task 9 file ownership. This task does not lower the threshold or alter unrelated coverage tests.

## Production Instrumentation Contract

- `lesson_renderer_memory_probe.cc` is linked by `main/CMakeLists.txt`.
- Default production sampling reads internal free heap, largest internal block, PSRAM free bytes, and atomic live decoded-layer/LVGL-animation/context counters.
- Phase capture emits one compact `renderer_mem` record for `start`, `peak`, `complete`, or `cancel`.
- Renderer lifecycle hooks only update atomics; `RecordFrameAllocations` only increments a scalar and does not allocate.
- The host log audit rejects queue-full, allocation-failed, watchdog, decoder-leak, and OOM marker variants.

## Pending HIL Measurements

| Measurement | Status | Hardware value |
| --- | --- | --- |
| 100 cancel cycles | `PENDING` | Not measured |
| 100 completed entrances | `PENDING` | Not measured |
| Per-frame dynamic allocations | `PENDING` | Not measured |
| Maximum decoded layers | `PENDING` | Not measured |
| Animations/contexts at 500 ms | `PENDING` | Not measured |
| Internal heap loss | `PENDING` | Not measured |
| PSRAM loss | `PENDING` | Not measured |
| Largest internal block | `PENDING` | Not measured |
| Minimum internal free heap | `PENDING` | Not measured |
| Serial forbidden-marker audit | `PENDING` | No hardware log captured |

A live HIL `PASS` requires a production image, device identity, serial capture, and the same numeric thresholds. This document does not substitute simulated host values for those measurements.
