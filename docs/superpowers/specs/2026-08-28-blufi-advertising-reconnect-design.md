# BluFi Advertising And Reconnect Design

## Goal

Make a robot that re-enters Wi-Fi setup advertise a complete, Android-discoverable BluFi identity, then prove repeated GATT lifecycle reuse without changing NVS, claim state, or stored Wi-Fi.

## Design

Keep the compact payload split already used by the firmware: flags and BluFi UUID `0xFFFF` in ADV, full `TBOT-*` identity in scan response. Replace the eager advertising start with a small Bluedroid GAP wrapper that forwards every event to ESP-IDF's BluFi handler and starts advertising only after both raw payload completion events report success. A configuration failure falls back to `esp_blufi_adv_start()` instead of leaving setup unavailable.

The wrapper resets its pending state for every setup initialization so a connect/deinit/reinit cycle cannot reuse stale completion bits. The change is limited to `main/boards/common/blufi.cpp` and a source-contract regression test.

## Lifecycle Race Closure

Treat station-association BLE release as one lifecycle transaction without
blocking BluFi callbacks on `provisioning_finalization_mutex_`. Acquire the
callback-safe `ble_lifecycle_mutex_` across generation validation, timer
cancellation, advertising stop, and BLE deinit. Use short finalization-lock
scopes only to commit or validate generation-bound state before and after the
blocking transition. Generation-bound claim-confirm results follow the same
two-phase ownership: commit the accepted result under the finalization lock,
release it, then let the lifecycle lock own blocking teardown. This prevents a
stale worker from tearing down a newer setup while allowing disconnect callbacks
needed by Bluedroid deinit to complete.

Generation-bound claim worker dispatch uses the same global order. After the
short state-commit gate, acquire lifecycle ownership, briefly revalidate the
setup generation under the finalization lock, release finalization, and commit
only bounded worker creation or timer/state fallback work before releasing
lifecycle. If confirmation worker creation fails, re-arm claim polling inside
that reservation. If fetch worker creation fails, enter a second equivalent
generation-aware standby reservation before changing claim substate, rendering,
ensuring BLE advertising, or re-arming polling. A BOOT restart between dispatch
failure and fallback therefore suppresses every stale fallback effect.

Generation-aware standby ensure is also one lifecycle transaction. It must not
read BLE state before acquiring lifecycle ownership: a station-association
release may have observed active BLE and be waiting to deinitialize it. Once the
ensure operation owns lifecycle, it briefly validates the setup generation,
rechecks the post-release BLE state, prepares and binds the provisioning audio
token only if initialization is required, initializes with the owned primitive,
and arms the hard timeout before returning success. Preparation, initialization,
or timer failure performs provisioning abort/audio rearm through an owned helper,
without recursively entering a public lifecycle API. Success means BLE is still
active and its setup timer is armed when lifecycle ownership is released.

Callbacks accepted by public generation lifecycle APIs execute while lifecycle
or finalization ownership is held. They are bounded, perform no network I/O, do
not wait on BluFi callbacks, and never call another public BluFi lifecycle API.

Tag every advertising start completion that the wrapper intentionally owns,
including the default BluFi fallback path. A fallback start from an older host
lifecycle must never consume the queued epoch for a newer compact advertising
start. Events without a matching active epoch remain forwarded to ESP-IDF but
are ignored by the TBOT lifecycle state machine.

The scan-mode integration must be applied hunk-by-hunk on top of current
`main`. It may update scan initialization and diagnostic logging, but it must
not replace the newer advertising, session binding, deferred Wi-Fi list, or
station-association lifecycle code.

## Verification

1. Demonstrate RED before implementation and GREEN after it.
2. Run the focused BluFi, Wi-Fi provisioning, security, and redaction suites.
3. Build the LCDWiki ESP32-S3 application and flash only offset `0x20000`.
4. On the attached Android phone, run at least three consecutive cycles of connect, BluFi service discovery, local disconnect, and reconnect without rebooting the robot.
5. Prove with source-contract tests that release and generation-bound claim
   completion hold the lifecycle lock across blocking deinit, never the
   finalization lock, and that fallback advertising completions carry lifecycle
   ownership.
6. Prove deterministically that restart after claim dispatch failure cannot run
   stale confirmation polling or fetch standby fallback state/render/BLE/poll
   effects.
7. Prove with a threaded release/drain interleaving that generation-aware ensure
   rechecks BLE state under lifecycle ownership, restores an off stack, arms the
   timeout, and cannot report success while BLE remains off.
