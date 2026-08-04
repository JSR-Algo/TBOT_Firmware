# Production Cinematic Evidence Design

## Purpose

The exact production firmware used for release HIL must emit enough structured evidence to prove five consecutive 19-cue cinematic journeys. The current collector is compiled out unless `CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=y`, while production correctly forbids that HIL configuration. This leaves the production artifact unverifiable by the release evidence gate.

## Configuration And Identity

Add `CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE`, enabled for the production LCDWiki configuration. Keep `CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=n`, `CONFIG_TBOT_HIL_STORAGE_FAULTS=n`, and all HIL MCP tools, banners, and fault-injection symbols absent.

The release channel emits lines beginning with `CINE_EVIDENCE`. Public firmware APIs and compiled symbols use `LessonCinematicEvidence` naming rather than `LessonCinematicHilTelemetry`. The output remains serial-only: it adds no MCP, HTTP, WebSocket, or other network surface.

## Evidence Data Flow

At boot, the collector records a random nonzero boot nonce, reset reason, lifetime internal-heap minimum, and PSRAM minimum, then emits one `CINE_EVIDENCE event=boot` line.

The flattened cinematic renderer starts a bounded cue record with cue identity, sequence, and monotonic start time. Existing renderer and panel boundaries record SD read latency, queue completion, parser/CRC/I/O/DMA/timeout faults, late ticks, and missed periods. The collector samples current internal heap and PSRAM at cue begin, every existing `Record*` boundary, and cue end, retaining the lowest sampled value for each cue in fixed state. Each terminal cue transition emits one `CINE_EVIDENCE event=cue_end` line containing the existing fixed-width counters, these cue-scoped boundary-sampled heap minima, the allocator's separate lifetime internal-heap minimum, boot nonce, and reset reason.

The collector retains one boot record and one cue record only. It performs no dynamic allocation and uses a fixed 1,024-byte stack buffer for terminal formatting. Native maximum-width formatting tests require at least 256 bytes of unused margin after the longest canonical cue and maximum-width numeric fields are formatted.

## Verification Contracts

The log verifier requires the exact `CINE_EVIDENCE` prefix, one boot record, the canonical 19 cue-end records in order, per-cue internal heap of at least 20,480 bytes, nonzero PSRAM, zero fault/late/reset counters, and valid terminal reasons. It does not accept legacy `HIL_CINE` lines for production release evidence.

The five-pass verifier continues to require distinct boot nonces, unique serial/server logs, exact vi-VN/Kore identity, gentle wrong-answer and silence retry evidence, no post-boot reset signature, no OOM/allocation failure, and no fallback/degraded marker.

The production artifact auditor requires release cinematic evidence enabled while both HIL configurations remain disabled. It records this state in the build manifest and continues to reject HIL tools, storage-fault symbols, banners, banned APIs, or a non-production embedded profile.

## Resource And Safety Gates

Native tests prove the release-enabled ESP path uses bounded state, updates cue minima across allocation-free boundary samples, emits the exact boot and cue release schemas, fits maximum-width output in the fixed buffer with the required margin, and leaves the disabled configuration as a no-op. Static contracts reject heap-monitor APIs, dynamic allocation, and network/tool registration in the evidence module.

ESP-IDF's local minimum heap monitor was explicitly rejected: its start path allocates a global snapshot array with `heap_caps_malloc`, asserts when that allocation fails, and owns process-global monitor state until stop. Production evidence instead calls the allocation-free current-size query at the defined cue boundaries and keeps the minimum in the collector's existing fixed cue record. These fields are sampled boundary minima, not a claim of continuous per-cue allocator monitoring; the global allocator lifetime minimum remains a separate diagnostic.

The ESP evidence lock is backed by one global `StaticSemaphore_t` initialized exactly once with `xSemaphoreCreateMutexStatic`. Guards use `xSemaphoreTake(..., portMAX_DELAY)` and `xSemaphoreGive`; they do not spin across heap queries or terminal formatting. `std::mutex` is permitted only in the non-ESP host-test branch because ESP-IDF's pthread-backed standard mutex can lazily allocate. The ESP branch must contain no standard/pthread mutex operation or dynamic FreeRTOS mutex creator.

The full ESP-IDF build must pass the existing internal-RAM guardrails and production artifact audit. The image must fit the app partition. Hardware flashing remains locked until the exact production artifact, backend/server/web deployment, and assignment validator gates are all released by the root task.

## Rollback

Disabling `CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE` restores the bounded collector to no-op behavior without changing cinematic playback. Rollback does not enable HIL configurations or alter stored assets, NVS, or SD content.
