# Automatic Offline Wi-Fi BLE Recovery Design

## Goal

Allow an owned robot moved to a location where its saved Wi-Fi is unavailable
to become discoverable from the mobile app's **Doi Wi-Fi** flow without pressing
BOOT, factory-resetting, or removing cloud ownership.

## Current Failure

`WifiBoard::TryWifiConnect()` starts the saved-network station path and normally
arms a 60-second connection timeout. When `StartStationIfScanIdle()` reports a
busy or failed lifecycle start, the function returns without arming that timeout.
No later event is guaranteed to repair this state. The robot can therefore stay
offline indefinitely with BLE provisioning disabled while the phone scans for a
device that never advertises.

The backend/WebSocket recovery window does not cover this case because it runs
only when Wi-Fi is already connected and the passive backend channel repeatedly
fails.

## Firmware Behavior

### Fixed recovery deadline

Whenever saved Wi-Fi exists but the robot is not connected, firmware must arm a
single 60-second recovery deadline. This includes:

- the station starting normally;
- the station already being active but not connected;
- station startup being temporarily busy or failing;
- a connected station later becoming disconnected.

Repeated start failures and disconnect callbacks must not move the deadline
forward. The deadline represents continuous time without a working Wi-Fi
connection.

### Successful recovery

A confirmed Wi-Fi connection cancels the recovery deadline. A stale timeout
callback must re-check the live connection state before changing radio modes. If
the station has recovered, the callback does nothing.

### BLE provisioning fallback

If the deadline expires while the robot is still disconnected, firmware enters
the existing conditional Wi-Fi configuration path. That path must:

- preserve the robot's cloud ownership and device credentials;
- quiesce realtime/audio workers through the existing preparation transaction;
- stop station mode;
- restart BluFi and advertise the owned robot for credential-only reprovisioning;
- display the Wi-Fi setup state;
- retain the existing bounded BLE setup timeout;
- accept a new SSID/password and return to the normal claimed online runtime.

The fallback must not erase saved credentials before replacement credentials
have connected successfully. Existing transactional SSID rollback remains
authoritative when the new credentials fail.

## Mobile Behavior

The **Doi Wi-Fi** action remains the user entry point and does not unpair the
robot. It may issue the cloud `wifi_setup` command when the robot is online, but
must also work when the robot is already offline.

Reconnect discovery uses two 40-second BLE scans. This 80-second window covers
the firmware's 60-second automatic fallback plus scan scheduling variance. Once
the owned robot appears, the app routes to the robot Wi-Fi scan and sends only
new credentials over the credential-only BluFi path.

The destructive **Huy ket noi Robot** action is not required to change Wi-Fi and
must not be presented as the recovery step for a moved robot.

## State And Race Safety

- Only one Wi-Fi recovery deadline may be active at a time.
- A connected event cancels pending conditional entry.
- Conditional entry checks connection state before preparation, before publish,
  and immediately before station teardown.
- Explicit **Doi Wi-Fi** commands may enter setup immediately and take priority
  over the conditional deadline.
- Lesson runtime keeps the existing deferral behavior; the same fixed deadline
  is retried after the protected activity rather than silently discarded.
- Provisioning generation tokens continue to reject stale BLE completion work.

## Observability

Logs must distinguish:

- recovery deadline armed;
- an existing deadline retained;
- deadline cancelled because Wi-Fi recovered;
- station start busy while deadline remains armed;
- deadline expired and conditional BLE setup requested;
- conditional setup cancelled because Wi-Fi recovered at a race boundary.

Logs must never include SSIDs, passwords, bootstrap tokens, or device secrets.

## Verification

Automated coverage must prove:

1. A normal saved-Wi-Fi start arms the fixed deadline.
2. Busy/failed and already-active station starts also arm it.
3. Repeated failures do not postpone it.
4. Runtime disconnection arms it when no deadline exists.
5. Connection success cancels it.
6. Expiry while offline requests conditional Wi-Fi configuration.
7. Expiry after recovery does not open BLE.
8. Owned identity and previous SSID survive entry into setup mode.
9. Failed replacement credentials roll back without corrupting the saved tuple.
10. Mobile offline **Doi Wi-Fi** discovery spans the firmware fallback window.

Physical Android/robot E2E verification must cover moving away from the saved
network, automatic BLE advertising without BOOT, discovery through **Doi Wi-Fi**,
robot-side Wi-Fi scanning, provisioning to the selected network, WebSocket
authentication, heartbeat acceptance, and an online device state in the app.
