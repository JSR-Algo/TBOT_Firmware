# Renderer V2 Memory Stability Evidence

## Disposition

- Host gate: `PASS` using deterministic simulated allocator values and the production lifecycle counters.
- Hardware-in-the-loop gate: `PENDING`; no robot was flashed or measured for this task.
- The host byte counts below are test fixtures, not ESP32-S3 measurements. They prove threshold evaluation, lifecycle accounting, and zero C++ allocations inside the animation-frame callback boundary.

## Source And Toolchain

- Measured base commit: `83420c6bd4bf0354172120cd484977e4cc0646d5`.
- Candidate commit: the commit containing this document (`git rev-parse HEAD` after checkout); the code, test, script, CMake linkage, and evidence are committed atomically.
- Host compiler: `Apple clang version 21.0.0 (clang-2100.1.1.101)`, target `arm64-apple-darwin25.2.0`.
- Production-object cross compiler: `xtensa-esp-elf-g++ (crosstool-NG esp-14.2.0_20251107) 14.2.0`.
- ESP-IDF checkout available for production compilation: `v5.5.2-dirty`. The `dirty` suffix describes the local toolchain checkout and is not presented as an upstream release identity.

## Machine-Readable Host Report

```json
{"schemaVersion":2,"measurementKind":"simulated-host","passed":true,"cycles":{"cancel":100,"complete":100},"actual":{"frameAllocationsMeasured":true,"frameAllocations":0,"maxLiveDecodedLayers":3,"maxSettledAnimations":0,"maxSettledContexts":0,"internalHeapLossBytes":1024,"psramLossBytes":2048,"largestInternalBlockBytes":90112,"minInternalFreeBytes":126976,"settledObservationsAt500ms":200,"forbiddenMarkersFound":false},"thresholds":{"maxFrameAllocations":0,"maxLiveDecodedLayers":3,"maxSettledAnimationsAndContexts":0,"maxInternalHeapLossBytes":4096,"maxPsramLossBytes":8192,"minLargestInternalBlockBytes":65536,"minInternalFreeBytes":49152}}
```

The report comes directly from `./scripts/run_host_native_lesson_memory_test.sh`. Only allocator byte values are deterministic fixtures. Layer, animation, and context values come from the same production atomic hooks used by `LcdDisplay`. Every frame executes `AdvanceLessonRendererAnimationFrame`, the bounds-writing callback body shared with the real LVGL timer. The host enables the same diagnostic measurement window used by HIL and verifies that multiple allocations saturate one frame's allocation-observed flag. Therefore zero proves no observed frame allocation; a positive value counts affected frames rather than exact allocation calls.

## Verification

Commands run from the firmware worktree:

```text
./scripts/run_host_native_lesson_memory_test.sh
PASS: JSON report above

SANITIZE=1 ./scripts/run_host_native_lesson_memory_test.sh
PASS: AddressSanitizer and UndefinedBehaviorSanitizer, same JSON report

CXX=g++ ./scripts/run_host_native_lesson_memory_test.sh
PASS: compiler-driver cross-check, same JSON report

ninja -C /tmp/tbot-renderer-memory-build esp-idf/main/CMakeFiles/__idf_main.dir/display/lcd_display.cc.obj esp-idf/main/CMakeFiles/__idf_main.dir/lesson_renderer_memory_heap_hook.cc.obj esp-idf/main/CMakeFiles/__idf_main.dir/lesson_renderer_memory_probe.cc.obj
PASS: production display wiring and probe cross-compiled as Xtensa objects

idf.py -B /tmp/tbot-renderer-memory-diag -D IDF_TARGET=esp32s3 -D SDKCONFIG=/tmp/tbot-renderer-memory-diag/sdkconfig -D TBOT_RENDERER_MEMORY_DIAGNOSTICS=ON -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local;sdkconfig.defaults.renderer-memory-diagnostics' reconfigure
ninja -C /tmp/tbot-renderer-memory-diag esp-idf/main/CMakeFiles/__idf_main.dir/display/lcd_display.cc.obj esp-idf/main/CMakeFiles/__idf_main.dir/lesson_renderer_memory_heap_hook.cc.obj esp-idf/main/CMakeFiles/__idf_main.dir/lesson_renderer_memory_probe.cc.obj
PASS: diagnostic heap-hook wiring cross-compiled as Xtensa objects; `esp_heap_trace_alloc_hook` is a strong symbol in IRAM, uses aligned DRAM flags, and has no undefined helper calls

./scripts/run_host_native_lesson_visual_animation_test.sh
PASS: lesson visual animation host test passed: 106 checks

./scripts/run_host_native_lesson_coverage.sh
PRE-EXISTING GATE FAILURE: 94.6% line coverage (2096/2216), below the script's hardcoded 100% threshold
```

The coverage failure is in existing `lesson_handler.cc` branches and is outside the Task 9 file ownership. This task does not lower the threshold or alter unrelated coverage tests.

## Production Instrumentation Contract

- `lesson_renderer_memory_probe.cc` is linked by `main/CMakeLists.txt`.
- Default production sampling reads internal free heap, largest internal block, PSRAM free bytes, and atomic live decoded-layer/LVGL-animation/context counters.
- `LcdDisplay` balances decoded-layer hooks on layer install, clear, replacement, and destruction; animation/context hooks balance on start, completion, timer-create failure, cancellation, and display destruction.
- `LcdDisplay::StartLessonRobotEntrance` captures start and peak; immediate fallback and timer completion capture complete; active cancellation captures cancel.
- Diagnostic builds separately arm a real 500 ms LVGL settled-observation timer after complete/cancel. Timer identity and generation checks invalidate superseded starts, cancellation, and destruction; a missed or cancelled observation cannot pass the 200-observation HIL gate.
- The real LVGL timer calls the allocation-free `AdvanceLessonRendererAnimationFrame` bounds-writer path exercised by the host soak.
- Phase capture emits one compact `renderer_mem` record for `start`, `peak`, `complete`, or `cancel`.
- Normal release builds do not execute frame-accounting code and log `frame_alloc=na`; an unmeasured value is never emitted as numeric zero.
- HIL diagnostics enable `TBOT_RENDERER_MEMORY_DIAGNOSTICS=ON` with `CONFIG_HEAP_USE_HOOKS=y`. The strong IRAM ESP allocation hook performs only aligned volatile DRAM loads/stores while the exact LVGL frame window is active. A frame's own allocator hook completes synchronously before the allocation returns, and concurrent hooks only store `1`, so they can cause a conservative positive but cannot erase an observed frame allocation.
- `LessonRendererMemoryReport::passed` requires `frameAllocationsMeasured=true`; an N/A frame count cannot pass the zero-allocation gate.
- The host log audit rejects queue-full, allocation-failed, watchdog, decoder-leak, and OOM marker variants.

Diagnostic HIL builds must include the checked-in overlay and CMake flag:

```text
idf.py -B build-renderer-memory-hil \
  -D TBOT_RENDERER_MEMORY_DIAGNOSTICS=ON \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local;sdkconfig.defaults.renderer-memory-diagnostics' \
  build
```

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
