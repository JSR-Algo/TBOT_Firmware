# BluFi Wi-Fi Scan Ownership Recovery Design

## Goal

Make BluFi Wi-Fi discovery recover from an uninitialized or reset Wi-Fi driver
without losing a phone request, sending access points from an older BLE/setup
session, rebooting the robot, or erasing Wi-Fi/NVS state.

## Problem

BluFi scan control currently spans three execution contexts:

- the BluFi/BTC callback task accepts `ESP_BLUFI_EVENT_GET_WIFI_LIST`;
- the default ESP event-loop task receives `WIFI_EVENT_SCAN_DONE`;
- the Application task sends deferred results and performs lifecycle recovery.

The scan flags are plain booleans shared across those contexts. This permits
data races between a new request, scan completion, BLE disconnect, and setup
restart. More importantly, an ESP event already queued before handler
unregistration can be delivered to a newly registered handler. Because a
nonblocking ESP-IDF scan does not expose a request token that can be matched to
`WIFI_EVENT_SCAN_DONE`, an old completion can otherwise be mistaken for the
current setup generation and sent to the wrong phone session.

Mode-read recovery also needs a real driver repair. If `esp_wifi_get_mode()`
returns `ESP_ERR_WIFI_NOT_INIT`, changing a local mode variable to
`WIFI_MODE_NULL` is insufficient because `esp_wifi_set_mode()` will fail with
the same error.

## Architecture

> **Approved ownership amendment:** Physical scan identity is defined by
> `docs/superpowers/specs/2026-08-31-global-wifi-scan-lease-design.md`. The
> global lease is a prerequisite for accepting callbacks during `Starting` and
> for the driver recovery described here.

### Scan State Machine

Add one mutex-owned scan controller with these phases:

- `Idle`: no physical scan or callback is outstanding.
- `Starting`: a request owns a new request ID and is submitting a passive scan.
- `Running`: ESP-IDF accepted the physical scan and exactly one completion is
  owed by the current Wi-Fi driver incarnation.
- `Draining`: the owning BLE/setup session is stale, but the physical scan may
  still produce a queued completion. The completion must be consumed and
  discarded before another physical scan starts.

The controller stores:

- a monotonically increasing software request ID for diagnostics and internal
  two-phase start validation;
- setup generation, BLE session state, and BLE connection epoch captured when
  the request is accepted;
- whether results should update saved-SSID state;
- whether the phone expects a Wi-Fi list response;
- one pending current-session request that arrived while an older scan drains;
- the Wi-Fi driver incarnation that owns the outstanding callback.

All reads and writes of scan phase, response flags, cache ownership, handler
ownership, and pending requests occur through controller helpers under the same
mutex. The request ID is not treated as an ESP callback token.

### Request Flow

On `GET_WIFI_LIST`:

1. Snapshot the current setup generation, BLE session, and connection epoch.
2. If the controller is `Idle`, reserve `Starting` and submit one passive scan.
3. If an old scan is `Running` or `Draining`, store or replace the single
   pending request for the current session. Do not start a second physical scan.
4. If submission fails synchronously, return the controller to `Idle` only if
   the same request ID still owns `Starting`, then send `WIFI_SCAN_FAIL` for
   that request.
5. If submission succeeds, commit `Starting -> Running` for the same request ID.

Repeated requests from the same current BLE session coalesce into the pending
request rather than creating an unbounded queue.

### Completion Flow

The Wi-Fi event handler atomically consumes the one outstanding callback:

- A matching `Running` request may collect bounded AP records only if its setup
  generation, BLE session, and connection epoch are still current.
- A `Draining` completion clears the driver AP list and produces no cache,
  response, saved-SSID update, or error for the stale phone.
- After either completion, the controller becomes `Idle`. If a valid pending
  request exists, schedule exactly one fresh scan on the Application task.

AP records retain the existing cap, deduplication, passive scan configuration,
deferred `ScheduleWifiListSend`, and generation/connection checks before BLE
delivery.

### Disconnect, Restart, And Deinit

BLE disconnect, setup-generation change, and BluFi deinit invalidate the
current request snapshots under the scan mutex:

- `Starting` is cancelled if ESP-IDF has not accepted the scan.
- `Running` becomes `Draining`; its handler remains registered so a queued old
  completion is consumed by the same controller.
- a pending request belonging to the stale session is discarded.

The handler is not unregistered while a callback is owed. Normal host teardown
may unregister it only after the controller is `Idle`, or after the recovery
barrier proves the old driver/event incarnation is drained.

### Lost Callback Recovery

An accepted scan callback can be lost during OOM or Wi-Fi driver teardown. A
bounded watchdog owned by the Application task handles a scan that remains
`Running` or `Draining` beyond its deadline:

1. Revalidate that the same request and driver incarnation are still stuck.
2. Quiesce active station/config users; never recycle the driver while a live
   station connection owns it.
3. Call `esp_wifi_scan_stop()`, stop the Wi-Fi radio, and deinitialize only the
   Wi-Fi driver.
4. Post a FIFO barrier event to the default ESP event loop and wait with a
   bounded timeout. The barrier proves all earlier queued `SCAN_DONE` events
   have either run or been discarded before a new handler incarnation starts.
5. Reinitialize only the Wi-Fi driver, preserving NVS, netif initialization,
   the default event loop, saved credentials, and robot process state.
6. Advance the driver incarnation, reset the old scan to `Idle`, and start one
   still-valid pending request.

Recovery never reboots the robot and never erases flash. A recovery retry may
add approximately one to two seconds to Wi-Fi discovery.

### WifiManager Reconciliation

Add a mutex-protected driver reconciliation operation to `WifiManager`:

- probe driver presence with `esp_wifi_get_mode()`;
- return immediately when the driver is healthy;
- on `ESP_ERR_WIFI_NOT_INIT`, require station/config ownership to be inactive,
  then perform driver-only initialization with the existing configuration;
- do not repeat NVS, netif, or default event-loop initialization;
- fail closed for other probe errors or active users.

The scan path invokes reconciliation before mode selection. After a healthy
driver is guaranteed, NULL/AP/nonstation modes move to STA, STA/APSTA modes are
preserved, `ESP_ERR_WIFI_STATE` from start is accepted, and exactly one passive
scan is submitted.

## Locking And Threading

- The scan controller mutex never nests the BluFi lifecycle or provisioning
  finalization mutexes.
- ESP-IDF Wi-Fi calls, event-loop barrier waits, BLE sends, and Application task
  scheduling occur after releasing the scan mutex.
- State transitions use request ID and driver incarnation checks when committing
  results after an external call.
- Driver recovery runs only on the Application task and under WifiManager's
  existing mutex/ownership rules.
- Event handlers remain nonblocking; they snapshot/consume state and defer BLE
  delivery or recovery work.

## Failure Handling

- Synchronous scan-start failure sends `WIFI_SCAN_FAIL` only to the owning
  current request and logs `reason=scan_start_failed`.
- A completed current scan with no reportable APs sends `WIFI_SCAN_FAIL` and
  logs `reason=scan_completed_without_ap_records`.
- Stale or draining completions are silent and clear their driver results.
- Driver reconciliation or barrier failure leaves the controller draining,
  records a bounded diagnostic, and retries recovery without accepting a new
  physical scan behind the unresolved callback.
- Heap diagnostics remain aggregate-only and never log SSIDs, passwords,
  tokens, or other credentials.

## Verification

Test-first verification must include:

1. A native state-machine model where an old completion arrives after setup
   restart and cannot satisfy the new pending request.
2. Threaded GET/completion/disconnect interleavings proving response flags are
   not lost and only one physical scan is outstanding.
3. A lost-callback watchdog model proving driver reset plus event-loop barrier
   occurs before the pending scan starts.
4. WifiManager tests for `initialized=true` plus driver `NOT_INIT`, ensuring
   driver-only reinit happens once and netif/default-loop initialization is not
   repeated.
5. Source contracts preserving passive scanning, bounded/deduplicated AP
   ownership, deferred generation-bound delivery, advertising ledger behavior,
   and lifecycle lock ordering.
6. Focused BluFi/Wi-Fi suites, the full firmware pytest suite, and a production
   ESP32-S3 build.
7. NVS-preserving flash and physical Android/robot E2E covering scan, connect,
   disconnect, reconnect, and Wi-Fi change without pressing BOOT.

## Non-Goals

- No robot reboot or flash erase as scan recovery.
- No unbounded queue of phone scan requests.
- No reliance on `wifi_event_sta_scan_done_t::scan_id` as a request token.
- No change to Wi-Fi credentials, cloud claim protocol, mobile UI, or BluFi
  advertising payloads.
