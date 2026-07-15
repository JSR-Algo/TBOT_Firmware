# Provisioning Lifecycle Concurrency Design

## Goal

Remove three remaining lifecycle races: wake-word workers acknowledging exit before their final object access, concurrent BluFi stack transitions duplicating SDK teardown, and successful provisioning paths tearing BLE down without one authoritative wake-word rearm.

## Worker Exit Acknowledgement

AFE detection and AFE/Custom encoding workers use FreeRTOS event-group exit bits. Each worker copies the event-group handle into task-local storage before its final object access, completes every member update and queue notification, sets the exit bit through the local handle, and immediately deletes itself. Shutdown waits for these exit bits, so the owning wake-word object cannot be reset while a worker can still dereference it. Task-creation failure sets the relevant exit bit from the creator.

## BluFi Transition Serialization

A host-testable transition gate serializes `init()` and `deinit()` without holding its mutex across ESP-IDF SDK calls. One caller owns the SDK transition. Concurrent callers requesting the same operation wait and receive that operation's result. A conflicting operation waits for the current transition and then becomes the next owner. Same-task reentry fails closed with `ESP_ERR_INVALID_STATE`, preventing callback reentrancy deadlock and duplicate SDK calls. BLE state reports off while a transition is active.

## Successful Teardown Ownership

`Blufi::CompleteSuccessfulProvisioningTeardown(reason)` is the only successful provisioning/claim terminal helper. It logs the reason, cancels the BLE timeout, runs serialized deinit, and calls wake-word rearm only after `ESP_OK`. Audio rearm is conditional on actual provisioning ownership, so duplicate Connected/success notifications do not advance the lifecycle twice and standby-only BLE teardown remains safe.

Success owners are WifiBoard Connected, connected-WiFi token handoff, authenticated-report BLE-off deferral, WiFi credential success, provisioned BLE disconnect, and confirmed claim completion. Hard timeout, failed/disconnected provisioning, claim pre-confirm, and manual/abort `StopBleAdvertising()` keep raw teardown and never rearm.

## Verification

Native tests deterministically hold workers between final access and exit acknowledgement, run real multithread transition-gate races, and verify idempotent rearm. Source contracts enumerate all success and non-success teardown call sites. Existing wake-word native tests, lesson coverage, Python contracts, ESP-IDF build, and CI workflow checks remain required.
