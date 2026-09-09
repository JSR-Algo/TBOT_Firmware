# Chat Runtime Timing Diagnosis

## Scope And Evidence

The operator requests repair and real-robot verification of Hi ESP, speech,
and relistening. App7609e9cb fails that acceptance: wake337736ms,
listening337986ms, idle339516ms, JSON Full339916ms after253880us.
Provider first audio latency is687.1ms. Initial idle precedes the visible fault.
Synchronous GIF reconstruction on Application overlaps this interval. Existing
native display stubs do not measure target rendering cost.

## Selected Approach

First instrument the unchanged runtime, then use an attended real wake to
identify the earliest recovery branch and work blocking admission. This is
preferred to increasing queue capacity/deadlines (masks scheduling pressure)
or moving all display work immediately (larger unproven architecture change).

Use fixed reason codes for START expiry/recovery and playout/rearm recovery.
Capture elapsed microseconds for inbound dispatch, state rendering, and emotion
rendering, warning only for slow work (at least50ms). Numeric counters distinguish
queued and outstanding permits when admission fails; a contended snapshot must
report unavailable, never block admission. Do not log JSON, transcripts, session
identifiers, keys, URLs, or dynamic server error text in new diagnostics.

No queue size, deadline, ownership, event ordering, audio processing, wake model,
Neweye assets, volume, credentials, gestures, or server behavior changes in this
diagnostic slice. Any correction follows measured evidence and a scoped design.

## Test And Hardware Gates

- Test-first checks verify diagnostic coverage/privacy and nonblocking snapshot
  semantics; native application tests prove instrumentation preserves behavior.
- Build exact ESP32-S3 target and verify frozen binary identity.
- Confirm sole connected target MAC14:c1:9f:d1:ac:20, active partition and backup.
- After fresh operator presence/clearance, write only app at0x20000; separately
  verify the write. Preserve NVS, OTA metadata, Neweye and rollback image.
- Capture bounded private serial plus matching read-only server logs. Ask for
  one Hi ESP trial, not repeated blind retries. No injected motion commands.
- Report the earliest measured failure. A diagnostic build is not a fix or
  production-ready acceptance. Final acceptance requires repeated successful
  wake/reply/relisten and audible stutter/echo checks on real hardware.

## Workflow Checklist

- [x] Explore context and compare frozen source/binary with hardware evidence.
- [x] Clarify scope from operator: communication first, preserve existing flow.
- [x] Compare diagnosis, capacity tuning and display separation approaches.
- [x] Present diagnostic design; operator requests proceeding with robot repair.
- [x] Write and self-review this diagnostic specification.
- [ ] Operator review of this written specification.
- [ ] Write implementation plan and execute test-first diagnostic slice.

No visual companion is needed: this is runtime diagnosis, not visual design.
