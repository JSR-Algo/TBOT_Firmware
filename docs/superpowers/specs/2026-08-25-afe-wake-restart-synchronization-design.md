# AFE Wake Restart Synchronization Design

## Goal

Keep the local "Hi ESP" detector processing microphone frames after lesson asset synchronization temporarily pauses wake-word ownership.

## Confirmed Context

The production robot now enters claimed `Idle` with WakeNet enabled before passive WebSocket success. During two spoken-only hardware windows, `wake_running=1` and codec `input_count` advanced normally, but no wake detection occurred. Historical successful captures on the same robot used the same `wn9s_hiesp` model, 16 kHz mono AFE pipeline, 0.55 threshold, and comparable input RMS. Model choice, threshold, and basic microphone input are therefore not the leading causes.

Startup performs repeated lesson asset-sync quiet sections. Each section disables WakeNet, calls `AfeWakeWord::Stop()`, resets the AFE buffer, then starts the existing detection task again. That task can be blocked indefinitely in `fetch_with_delay(..., portMAX_DELAY)`. The public lifecycle state can consequently report running while no successful AFE fetch returns.

## AFE Progress Evidence

Add monotonic, non-audio diagnostic counters for complete `Feed()` chunks submitted to `afe_iface_->feed()` and successful `fetch_with_delay()` returns. Record the latest progress timestamps and expose them through the existing periodic audio metrics log. Do not retain PCM, transcripts, wake phrases, or per-frame content.

The counters establish the component boundary: codec input, WakeNet feed, and WakeNet fetch. Hardware verification must show all three advancing before quiet, after quiet, and during a spoken test.

## Stop And Start Synchronization

Replace the indefinitely blocking fetch with a bounded fetch delay so the detection task regularly observes running and shutdown state. `Stop()` first clears the running bit, then waits for the detection task to acknowledge that it is outside an active fetch before resetting AFE buffers. The wait is bounded; timeout logs a diagnostic and skips unsafe buffer reset rather than racing the fetch task.

`Start()` sets the running bit only after any prior stop acknowledgement is complete. Repeated Start and Stop calls remain idempotent. Shutdown continues to use its existing exit acknowledgement and must take precedence over restart.

No callback may be emitted after Stop has cleared the running state. A fetch returning during the transition discards its result unless the same run generation remains active.

## Asset Sync Quiet Ownership

Preserve the existing safety gates for lesson runtime, active voice, connection, and reset. Coalesce adjacent startup asset operations under one quiet ownership interval where the caller already has a batch boundary, instead of stopping and restarting WakeNet for every asset. Nested or duplicate quiet requests retain the current fail-closed behavior; this change must not broaden microphone ownership or allow audio uplink while idle.

AFE synchronization is the correctness boundary. Quiet coalescing reduces churn but is not a substitute for a safe Stop/Start implementation.

## Failure Handling

If stop acknowledgement times out, keep WakeNet logically stopped, leave its feed target disabled, avoid resetting the shared AFE buffer, and allow the next controlled Start or full wake-word recreation to recover. Diagnostic logs include generation and feed/fetch counters without audio data.

Foreground voice connection, passive WebSocket retry, provisioning, reset, lesson playback/listening, and quiet-sync ownership retain their existing state and generation guards.

## Verification

Use TDD source contracts and host-testable lifecycle seams to prove bounded fetch, stop acknowledgement before reset, generation-gated fetch results, and idempotent restart. Retain existing lesson quiet and wake lifecycle suites. Run the full Python firmware tests and a clean ESP-IDF production build.

Flash only generated regions and preserve NVS. On hardware, require WakeNet feed and fetch counters to advance after startup quiet cycles, then require a spoken-only "Hi ESP" to produce wake detection, enter the foreground listening flow, send audio successfully, and reach speaking without any button input.
