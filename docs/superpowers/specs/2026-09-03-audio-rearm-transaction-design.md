# Audio Rearm Transaction Design

## Problem

Physical BluFi provisioning now joins the selected Wi-Fi network and reaches the
backend, but restoring the claimed robot runtime can leave the audio service
partially started.

The captured run requested the Opus worker's 28 KiB internal task stack while
the largest contiguous internal block was only 27 KiB. FreeRTOS rejected that
allocation, but `AudioService::Start()` ignored every task-creation return value
and had already marked the service as running. The provisioning completion then
reported `rearmed=1` even though the Opus handle remained null indefinitely.

The rearm callback also runs from the Application task immediately after the
high-priority BluFi connection worker schedules it. That worker self-deletes
after scheduling the callback, so the idle task may not yet have reclaimed and
coalesced its stack when audio allocations begin.

## Constraints

- Preserve the measured 28 KiB Opus stack budget and its internal-RAM latency
  characteristics.
- Continue releasing all three audio worker stacks before BluFi initialization.
- Preserve provisioning generation and exactly-once restart ownership.
- Do not create duplicate workers after stale, duplicate, or failed rearm.
- Never report a successful rearm unless all required workers exist.
- Keep retries bounded and leave the service safely stopped after exhaustion.
- Do not change Wi-Fi credentials, BluFi protocol, NVS, or claim semantics.

## Considered Approaches

### 1. Reclaim, allocate largest-first, and make startup transactional (selected)

Before rearming, delay for 10 ms so the idle task can reclaim the completed
BluFi worker. Create the 28 KiB Opus task before the smaller input/output tasks,
check every creation result, and commit the running state only after the full
worker set exists. If any creation fails, stop and join the partial set, allow
idle reclamation, and make one bounded retry. Return failure if the second
attempt cannot create the complete set.

This retains internal-stack performance, addresses the observed scheduling and
fragmentation boundary, and closes the false-success contract independently of
whether memory pressure recurs.

### 2. Allocate the Opus stack in PSRAM

External RAM avoids the internal contiguous-block requirement, but changes the
latency and cache behavior of a real-time codec task. This needs broader audio
performance qualification and is unnecessary while internal memory can be
reclaimed predictably.

### 3. Reduce the Opus stack budget

The 28 KiB value was selected from prior live-encoding measurements. Reducing it
without a new worst-case stack study risks stack overflow and is not an
acceptable provisioning fix.

## Design

Change audio worker startup from a fire-and-forget method into a checked
transaction.

`AudioService::Start()` returns a boolean result. It keeps the existing atomic
duplicate-start guard, but does not publish a healthy running service merely
because the guard was acquired. A private checked worker-start path creates the
Opus task first, followed by the required input and output tasks, using the same
cores, priorities, entry functions, and stack sizes as today. Each FreeRTOS
return code and resulting handle is validated.

If all required tasks are created, the periodic audio power timer remains
active and `Start()` returns true. If any task cannot be created, startup sets
the stop signal, wakes queues/event waits, stops the power timer, waits for any
created workers to clear their protected handles and self-delete, and leaves
`service_stopped_` true. A partial service is never observable as running.

`EndWifiProvisioningAndRearm()` still consumes the exact provisioning token once.
When the token says the prior service was running, it first calls
`vTaskDelay(pdMS_TO_TICKS(10))` to cross an idle-task reclamation boundary, then
calls checked startup. On failure it waits until the partial worker handles are
null, delays another 10 ms for reclamation, and retries once. It returns true
only if the full required worker set is alive after startup. A stale or
duplicate token still cannot trigger either attempt.

Callers that already inspect the boolean result keep treating false as a failed
runtime restoration. BluFi success teardown must log `rearmed=0` when worker
restoration fails; it may keep the proven Wi-Fi credentials and connected
station, but must not claim that the claimed runtime is ready.

## State And Concurrency Invariants

- `service_stopped_ == false` implies every required base audio worker handle is
  non-null after `Start()` returns.
- A failed startup owns cleanup of only the workers it created during that
  attempt.
- Worker handles remain protected by `task_handle_mutex_`; task exit clears its
  own handle before self-delete as today.
- Retry begins only after partial-worker handles are null and an idle reclaim
  delay has elapsed.
- The provisioning worker ledger is consumed once before restart. Retry is
  internal to that one accepted completion and cannot be triggered by a stale
  external token.
- An audio service that was stopped before provisioning remains stopped and
  performs no allocation.

## Error Handling And Observability

Log task-creation failure by worker name, FreeRTOS result, internal free bytes,
and largest internal block. Do not log audio payloads, Wi-Fi data, or secrets.

Log the start attempt number and final complete-worker invariant. The final
provisioning teardown result uses the checked boolean, eliminating the observed
`rearmed=1` false positive.

If both attempts fail, retain the stopped state and return false. Existing
caller error handling remains responsible for surfacing or scheduling broader
runtime recovery; this design does not introduce an unbounded background retry.

## Test Strategy

Use test-driven development:

1. Add a host-testable worker-start transaction seam that can inject success or
   failure for each task creation and record cleanup/retry ordering.
2. Prove Opus is requested before the smaller input/output workers.
3. Prove a failure at each creation point rolls back every created worker,
   restores the stopped invariant, and never reports success.
4. Prove one failed attempt may retry once after cleanup/reclaim and that a
   second failure remains fail-closed.
5. Prove a successful retry reports rearmed only when all handles exist.
6. Preserve stale-token, duplicate-token, previously-stopped-service, stack
   budget, Wi-Fi provisioning, and BluFi ownership contracts.
7. Run focused native/sanitizer tests, the full firmware suite, and the LCDWiki
   ESP32-S3 production build.
8. Flash and repeat at least three provisioning/rearm cycles while recording
   largest internal block and all three worker stack metrics.

## Success Criteria

- No 28 KiB allocation failure occurs during three consecutive physical rearms.
- Input, output, and Opus worker metrics are all present after every successful
  provisioning teardown.
- `rearmed=1` is emitted only when the complete worker set is running.
- An injected or physical allocation failure rolls back cleanly, returns false,
  and does not block a later controlled recovery.
- Automatic BluFi entry and exact-credential Wi-Fi connection remain unchanged.
- Focused, native sanitizer, full firmware, production build, and physical E2E
  gates pass without credential leakage.
