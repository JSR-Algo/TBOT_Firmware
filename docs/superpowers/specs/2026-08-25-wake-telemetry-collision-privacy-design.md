# Wake Telemetry Collision and Privacy Design

## Goal

Close the remaining wake telemetry publication-delay and raw-sample privacy gaps without changing the runtime log or introducing producer/consumer waiting.

## Publication Protocol

Keep the independent feed and state two-slot SPSC channels and their producer-active flags. Add consumer-owned collision state for each channel.

When a snapshot first requests a slot while its producer is active, it records the collision and returns an empty interval without waiting. On the first later non-busy drain, it reads the completed producer slot, requests the opposite slot, rechecks the producer-active flag, and drains the completed slot. If the original pending slot differs, it remains pending for a later drain. This makes the collision sequence report data on the next successful snapshot while retaining all older interval data exactly once.

The consumer always redirects future production away from the slot it will drain and rechecks the production flag before reading or clearing interval storage. The producer remains wait-free and the consumer never spins.

## Deterministic Runtime Tests

Add a native regression that stages the exact collision ordering: producer active, consumer changes the requested slot, producer observes the new request and fills it, then production ends. Assert the collided snapshot is empty and the next snapshot contains the produced interval.

Continue draining after the collision and assert every staged observation is reported exactly once, including data already present in the original pending slot. Retain the concurrent feed/state stress test and run it under ThreadSanitizer.

## Privacy Contract

Retain structural checks for the real include allowlist, allowed top-level and nested types, aggregate-only fields, no arbitrary nested members, no owning containers, no global/static raw-sample storage, and no logging, persistence, transport, callbacks, or allocation.

Remove exact source-line, declaration-order, atomic call-order, and complete call-name allowlists that reject harmless wrapping or formatting.

Add a normalized `ObserveFeedChunk` dataflow contract:

- Every `samples[...]` read appears only in the canonical local `sample` initialization.
- The local `sample` is used only to derive `magnitude`.
- Neither the borrowed sample expression nor the local raw sample is returned, passed to calls, or assigned into members, globals, interval storage, or channel storage.

Add a mutation that stores `samples[0]` in the upper bits of `feed_channel_.producer_slot`; the validator must reject it. Add acceptance mutations for harmless signature wrapping and `static_cast` formatting.

## Verification

Run focused pytest, the native lifecycle suite with the deterministic collision case, a ThreadSanitizer build/run of the telemetry native test, a production header compile, the relevant production build or compile check, `git diff --check`, and scope/status inspection.
