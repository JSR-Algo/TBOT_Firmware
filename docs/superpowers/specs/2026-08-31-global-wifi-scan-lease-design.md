# Global Wi-Fi Scan Lease Design

## Goal

Make every ESP-IDF Wi-Fi scan callback attributable to exactly one firmware
component, including callbacks delivered before `esp_wifi_scan_start()` returns,
without allowing a stale station/config scan to satisfy a BluFi phone request.

## Problem

`WIFI_EVENT_SCAN_DONE` is global to the Wi-Fi driver. It does not contain a
reliable caller token. The firmware currently has independent scan ownership in
BluFi, `WifiStation`, `WifiConfigurationAp`, and one board-specific blocking UI.

This creates an ownership ambiguity around BluFi submission:

- accepting `SCAN_DONE` while BluFi is `Starting` can consume a queued callback
  from another component;
- rejecting it can lose a legitimate BluFi callback that runs after driver
  acceptance but before `esp_wifi_scan_start()` returns;
- stopping a scanner and unregistering its handler does not prove that its
  already-posted event has left the default event loop.

Software request IDs inside BluFi cannot solve this because ESP-IDF does not
return those IDs in the callback.

## Chosen Architecture

Add one ESP-independent global scan lease coordinator, owned for process
lifetime by `WifiManager`. Every direct `esp_wifi_scan_start()` caller must hold
a lease before submission and retain it until one of these terminal proofs:

1. the matching callback was claimed and its AP-list ownership was finished;
2. a synchronous submission failure occurred before any callback was claimed;
3. the scan was stopped and a default-event-loop FIFO barrier completed;
4. lost-callback recovery reset the driver, drained the event loop, and advanced
   the driver incarnation.

The coordinator is separate from BluFi's logical controller:

- the global coordinator proves which firmware component owns the physical
  driver scan and callback;
- `BlufiWifiScanController` continues to bind a BluFi request to setup
  generation, BLE session, connection epoch, pending work, and delivery rules.

Neither layer treats ESP-IDF's scan ID as an ownership token.

## Lease Model

### Identity

Each lease contains:

- `WifiScanOwner`: Station, Config AP, BluFi, or board blocking UI;
- a monotonically increasing `lease_id`;
- the current Wi-Fi `driver_incarnation`.

All three fields must match before a state transition or callback claim is
accepted. A stale lease is always rejected.

### Physical States

- `Free`: no component may have an outstanding scan callback.
- `Starting`: one owner has the exclusive right to submit a scan.
- `Running`: ESP-IDF accepted the scan; one callback is owed.
- `Completing`: the matching callback was claimed; no other component may
  access or clear driver AP records.
- `Draining`: submission or ownership was cancelled; the lease remains held
  until callback consumption or FIFO-barrier proof.
- `Recovering`: the driver is being reset for a lost callback; ownership is not
  released until reset and barrier completion.

Only `Free` can grant a new lease.

## Submission And Early Callback Handshake

The scanner acquires a lease before calling `esp_wifi_scan_start()`.

If `SCAN_DONE` arrives while the lease is `Starting`, the coordinator knows it
belongs to that scanner because every other physical scanner is excluded. The
callback is latched, but driver AP records are not touched yet. When submission
returns:

- `ESP_OK`: commit the lease and consume the latched callback exactly once;
- synchronous error with no callback: release the lease and report failure;
- synchronous error with a latched callback: treat the callback as authoritative,
  suppress duplicate start failure, consume it once, and log an invariant
  diagnostic without secrets.

For a normal `Running` or `Draining` callback, the handler moves the lease to
`Completing` and processes or discards results according to its logical owner.
Finishing callback ownership returns the lease to `Free` only after driver AP
records have been collected or cleared.

## Scanner Integration

### BluFi

BluFi first reserves its logical request, then attempts to acquire the global
lease on the Application task. If the lease is busy, it retains one coalesced
current-session request and retries; it does not send `WIFI_SCAN_FAIL` merely
because another component is draining.

Immediately before the ESP-IDF call, BluFi revalidates and claims its logical
request. The global lease is stored with that request. The event handler must
claim the global lease before calling the logical controller or touching AP
records. Early callbacks are resumed after submission commit through the same
completion helper used by ordinary callbacks.

Owner-bound list and failure delivery remains deferred and revalidates setup
generation, BLE session state, and connection epoch.

### WifiStation And Config AP

Their local `scan_in_progress_` flags are not ownership proof. Each scan timer or
start event must acquire a lease before submission, and each event handler must
claim that exact lease before reading AP records.

Stop cancels future timers first, stops the current scan, marks the lease
`Draining`, posts a FIFO barrier, and releases ownership only after the barrier.
If the barrier times out, the lease remains `Draining` and other scanners stay
blocked until recovery.

### Blocking Board UI

The board-specific blocking scan also acquires a lease. After the blocking call
and AP-list handling, it drains any queued `SCAN_DONE` through the shared FIFO
barrier before releasing the lease. It cannot overlap Station, Config AP, or
BluFi scans.

## Default-Event-Loop Barrier

Move the FIFO barrier into a reusable Wi-Fi component helper so Station, Config
AP, board UI, and BluFi recovery use identical semantics. The helper:

1. registers a private event handler on the default loop;
2. posts a private barrier event;
3. waits at most 1000 ms for a binary semaphore;
4. unregisters the handler and destroys the semaphore;
5. returns false on allocation, registration, post, wait, or unregister failure.

Because the default loop is FIFO, successful completion proves all events
posted before the barrier have run. The barrier never runs while holding the
global coordinator, `WifiManager`, BluFi lifecycle, or BluFi finalization mutex.

## Driver Recovery Compatibility

Task 4 driver reconciliation requires a valid BluFi lease and inactive
Station/Config AP ownership. Task 5 lost-callback recovery moves that same lease
from `Running` or `Draining` to `Recovering` and retains it through:

`scan_stop -> wifi_stop -> wifi_deinit -> FIFO barrier -> wifi_init`.

Successful recovery advances the global and BluFi driver incarnations before a
pending request may start. Barrier or reinitialization failure leaves both
controllers fail-closed in draining/recovery state and retries without starting
a new physical scan.

## Locking

- Coordinator methods lock only the coordinator mutex and make no ESP-IDF,
  BLE, scheduling, or barrier calls.
- Scanner event handlers claim/release the coordinator, then access driver AP
  records outside its mutex.
- `WifiManager::mutex_ -> coordinator mutex` is allowed only for manager-owned
  Station/Config lifecycle transitions; the reverse order is forbidden.
- BluFi never nests lifecycle/finalization mutexes with either scan mutex.
- No barrier wait occurs under any scan or manager mutex.
- Ready/retry callbacks are invoked only after all ownership locks are released.

## Failure Handling

- Lease busy is backpressure, not a phone-visible scan failure.
- Synchronous start error sends an owner-bound failure only when no matching
  callback was latched.
- Callback plus synchronous error produces one completion and no duplicate
  failure.
- Barrier failure retains draining ownership and logs only owner, lease ID, and
  driver incarnation.
- No SSID, password, token, or credential value is added to diagnostics.

## Verification

Deterministic tests must prove:

1. Station cancellation plus barrier prevents its queued callback from claiming
   the next BluFi lease.
2. A BluFi callback between driver acceptance and submission commit is latched
   and consumed exactly once.
3. Synchronous start failure without callback releases ownership and produces
   exactly one current-session failure.
4. Callback racing a synchronous error wins without double completion.
5. Station timer and BluFi GET races call `esp_wifi_scan_start()` only once.
6. A fast phone request queues until Station cancellation and barrier complete.
7. Barrier timeout keeps later scanners blocked.
8. Lost-callback recovery advances incarnation before pending work starts.
9. A callback using an old lease/incarnation is rejected.
10. A source inventory proves every direct `esp_wifi_scan_start()` call site
    participates in the lease protocol.

The final firmware gates remain the full pytest suite, production ESP32-S3
build, app-only flash preserving NVS, and physical Android/robot E2E reconnect,
disconnect, outstanding-scan disconnect, repeated-list, and Wi-Fi-change cases.

## Non-Goals

- No reboot or NVS erase for scan ownership recovery.
- No parallel physical scans.
- No unbounded request queue.
- No change to credentials, cloud claim protocol, mobile UI, or advertising
  payloads.
