# BluFi Opmode Exact-Connect Ownership Design

## Problem

Physical Android-to-robot provisioning reaches Wi-Fi credential submission but
then times out. Firmware logs stop while the credential connection path calls
`WifiManager::StopStation()`.

The preceding BluFi `ESP_BLUFI_EVENT_SET_WIFI_OPMODE` event currently starts the
station immediately for `WIFI_MODE_STA` and `WIFI_MODE_APSTA`. That station start
launches the saved-network discovery flow. Roughly half a second later,
`ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP` claims the staged BluFi credentials and
starts the exact-credential worker. The worker must first stop the already
running station, so the BluFi callback can block behind the unrelated automatic
scan/session operation.

Starting saved-network discovery from the opmode event is redundant: the exact
credential worker already owns provisioning connection startup through
`StartStationWithCredentialsIfScanIdle()`.

## Constraints

- Preserve automatic entry into BluFi provisioning when saved Wi-Fi is
  unavailable; no BOOT press may be required.
- Preserve the global Wi-Fi scan lease, generation, connection-epoch, and
  credential-candidate ownership checks.
- Connect only with the credentials staged by the active BluFi session.
- Preserve the existing password-receive fallback when a client omits the
  explicit connect control frame.
- Preserve explicit disconnect handling and invalid/default opmode cleanup.
- Do not expose Wi-Fi credentials in source, tests, logs, or QA evidence.
- Keep this fix limited to BluFi opmode dispatch and its regression coverage.

## Considered Approaches

### 1. Make exact-credential connect the sole station-start owner (selected)

For `WIFI_MODE_STA` and `WIFI_MODE_APSTA`, initialize `WifiManager` but do not
call `StartStation()`. Let `ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP`, or the existing
password fallback, claim the staged credentials and exclusively start the
station via the exact-credential worker.

This directly removes the conflicting saved-network scan and keeps provisioning
semantics tied to the network selected on Android.

### 2. Defer the opmode station start until Wi-Fi is idle

The handler could schedule the current automatic start only after scan and
session activity settles. This adds lifecycle state and still duplicates the
later exact-credential start, leaving ordering races to coordinate.

### 3. Force `StopStation()` to cancel every in-flight operation

The stop path could be broadened to interrupt the automatic scan from inside
the BluFi callback. This changes shared Wi-Fi lifecycle behavior and risks
weakening ownership guarantees for callers unrelated to provisioning.

## Design

`ESP_BLUFI_EVENT_SET_WIFI_OPMODE` remains responsible for ensuring that
`WifiManager` is initialized and for handling modes that require configuration
AP or shutdown behavior. Its station-mode branches change as follows:

- `WIFI_MODE_STA`: acknowledge the requested mode without starting the station.
- `WIFI_MODE_APSTA`: retain the unsupported-mode warning, but defer station
  startup to the exact-credential path instead of approximating APSTA by
  starting saved-network discovery.
- `WIFI_MODE_AP`: keep starting the configuration AP.
- Invalid/default mode: keep stopping the station and configuration AP.

No new task, timer, queue, or ownership flag is introduced. The existing
`StartStationConnectFromCredentials()` path remains the sole owner of a BluFi
station connection attempt. Its atomic duplicate guard, staged-candidate claim,
session token, generation binding, and `StartStationWithCredentialsIfScanIdle()`
call remain unchanged.

## Event Flow

1. Android establishes BluFi and submits station opmode.
2. Firmware initializes `WifiManager` but does not start saved-network
   discovery.
3. Android sends SSID and password data, which are staged under the active
   candidate epoch.
4. `REQ_CONNECT_TO_AP`, or the existing password fallback when needed, calls
   `StartStationConnectFromCredentials()`.
5. The worker claims the candidate once, performs the existing station cleanup,
   and starts the station with those exact credentials.
6. Existing connection status reporting and successful provisioning teardown
   continue unchanged.

## Error Handling

- Duplicate connect triggers continue to be rejected by
  `m_wifi_connect_task_started`.
- Missing, incomplete, stale, or already claimed credentials continue to be
  rejected before station startup.
- Failure to initialize `WifiManager` still aborts opmode handling with an
  error log.
- Explicit `REQ_DISCONNECT_FROM_AP` still stops the station and resets local
  connection flags.
- Invalid/default opmode still stops both station and configuration AP.
- Failed exact-credential connection continues through the existing failure
  report and retry/re-entry behavior; this design does not add credential
  fallback to a saved network.

## Test Strategy

Use test-driven development:

1. Add a source contract regression proving that the STA and APSTA branches of
   `ESP_BLUFI_EVENT_SET_WIFI_OPMODE` do not call `StartStation()`.
2. Assert that AP mode and invalid/default cleanup behavior remain intact.
3. Assert that both the explicit connect event and password fallback still
   reach `StartStationConnectFromCredentials()`.
4. Run focused BluFi provisioning, scan ownership, Wi-Fi lifecycle, and native
   controller suites.
5. Run the full firmware test suite and LCDWiki ESP32-S3 production build.
6. Flash the robot and verify automatic BluFi entry, scan result delivery, valid
   exact-credential provisioning, disconnect/reconnect cycles, Wi-Fi switching,
   invalid credentials, and BLE interruption recovery.
7. Repeat one complete physical provisioning cycle from merged `main` before
   removing completed BluFi worktrees.

## Success Criteria

- STA/APSTA opmode handling never starts saved-network station discovery during
  BluFi provisioning.
- The explicit connect request remains the normal exact-credential start owner,
  and the password fallback still works when the control frame is absent.
- Valid physical provisioning proceeds beyond station shutdown and reaches a
  connected state without pressing BOOT.
- Disconnect/reconnect, Wi-Fi switching, invalid-credential, and BLE interruption
  scenarios recover without a hung callback or stale ownership mutation.
- Existing Wi-Fi lifecycle, scan ownership, provisioning, and full firmware
  tests pass, and the production firmware build succeeds.
- No Wi-Fi credentials appear in committed artifacts or QA evidence.
