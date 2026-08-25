# Idle Microphone And WakeNet Telemetry Design

## Goal

Determine why spoken "Hi ESP" is not detected after the AFE feed/fetch pipeline has been proven healthy, without recording or retaining speech audio.

## Confirmed Context

The production robot now enables WakeNet before passive WebSocket success and survives repeated lesson asset-sync quiet Stop/Start cycles. Hardware logs show `wake_running=1`, stable run generation, and continuously increasing AFE feed and fetch counters after the final quiet interval. Two firmware versions and multiple spoken-only windows still produced no wake detection.

The remaining boundary is signal quality and WakeNet decision output: codec reads and AFE processing are active, but current metrics do not show whether the idle microphone frames contain human-level energy or whether WakeNet reports intermediate states without reaching detection.

## Privacy-Safe Signal Metrics

Compute aggregate microphone statistics only on frames already submitted to the idle wake-word path:

- RMS amplitude for the completed feed chunk;
- absolute peak amplitude;
- count of chunks above a conservative human-speech diagnostic floor;
- monotonic chunk count and timestamp of the most recent above-floor chunk.

Publish rolling or interval aggregates through the existing periodic audio metrics log. Do not log, persist, transmit, hash, encode, or expose PCM samples. Do not add transcripts, spectral fingerprints, wake-word audio buffers, or per-sample values. Reset interval min/max/accumulators after each metrics publication while retaining monotonic counters needed for progress comparison.

## WakeNet Decision Metrics

Count successful AFE fetch results by `wakeup_state` category: no detection, detected, and any supported transitional/verification state. Log numeric/category counts rather than audio content. Record the latest returned model index only when WakeNet reports a non-idle decision; do not infer or log spoken content.

The implementation must validate array/model indices before use. Unknown future state values are counted under `other` and cannot trigger a wake callback.

## Diagnostic Interpretation

One spoken-only hardware window must distinguish these outcomes:

- RMS and peak remain near silence while codec input/feed/fetch advance: investigate microphone channel selection, gain, wiring, or idle codec configuration.
- Human-level RMS/peak and above-floor chunks advance, but WakeNet remains in no-detection: investigate model asset compatibility, selected model/index, or pronunciation/model mismatch.
- WakeNet reaches a transitional state but never detection: inspect verification thresholds/model configuration using measured evidence.
- WakeNet reports detection: retain the existing generation and callback flow and verify end-to-end voice behavior.

Threshold or model changes are explicitly out of scope until telemetry identifies the failing boundary.

## Runtime And Safety

Use integer accumulation with overflow-safe widths and bounded per-chunk work. Metrics must not allocate in the audio hot path, acquire application/network locks, or change microphone ownership. WakeNet Start/Stop synchronization, provisioning, lesson quiet, reset, and no-idle-uplink behavior remain unchanged.

Logging remains rate-limited by the existing system metrics interval. Production logs contain aggregate numbers only and are suitable for adult-operated diagnostic evidence.

## Verification

Use TDD to lock privacy exclusions, counter placement, overflow-safe RMS/peak calculation, state categorization, and metrics reset semantics. Run focused native calculation tests, firmware contracts, the full Python suite, and a clean production build.

Flash generated regions while preserving NVS semantically. During an adult-operated spoken-only window, require microphone aggregate metrics to react when speech occurs and use the resulting boundary evidence to choose the next fix. Do not press the robot button or retain raw audio.
