# Claimed Idle Heartbeat Independence

## Problem

An Android-to-robot provisioning run can finish BLE transfer, Wi-Fi association,
and backend claim, yet the app still times out. The robot opens the passive
lesson WebSocket, the server closes that socket, and the shared close/error
callbacks call `StopHeartbeat()`. The backend then has no fresh management
presence even though Wi-Fi and claim credentials remain valid.

## Desired Behavior

- A claimed, Wi-Fi-connected, idle robot keeps management heartbeat active
  independently of the passive lesson WebSocket lifecycle.
- Passive WebSocket open, close, connection failure, and reconnect backoff do
  not stop management heartbeat while the robot remains claimed and online.
- Heartbeat still stops on actual network loss, claim removal, heartbeat
  authentication failure, or an explicit state that already owns heartbeat
  policy during an active lesson.
- Unclaimed provisioning behavior remains unchanged.
- The Android app receives fresh online status and completes provisioning
  without requiring the BOOT button.

## Approaches Considered

1. Keep the passive lesson WebSocket artificially open. Rejected because
   lesson transport availability should not define device management presence.
2. Increase the Android provisioning timeout. Rejected because it masks the
   missing heartbeat and leaves the backend state incorrect.
3. Separate management heartbeat policy from passive WebSocket policy.
   Selected because it fixes the ownership boundary with the smallest runtime
   change.

## Design

Add a single policy helper that answers whether management heartbeat must stay
active from durable device state: claimed credentials, network connectivity,
and lesson-runtime ownership. WebSocket callbacks use this policy instead of
unconditionally stopping heartbeat.

For a claimed idle robot, passive WebSocket success starts or preserves
heartbeat, while passive close/error preserves it. For an unclaimed robot or
real network disconnect, the existing stop behavior remains. Heartbeat HTTP
401/403 recovery remains authoritative and clears stale claim credentials as it
does today.

## Test Strategy

1. Add a focused source-contract regression test that fails while passive
   WebSocket close/error callbacks unconditionally stop heartbeat.
2. Implement the minimal firmware policy change and make the focused test pass.
3. Run the related claim, heartbeat, passive-WebSocket, and Wi-Fi provisioning
   tests, then build the firmware.
4. Flash the robot without BOOT and run physical Android E2E: BLE discovery,
   robot-side Wi-Fi scan, credential transfer, Wi-Fi association, claim,
   online status, passive WebSocket reconnects, and at least two heartbeat
   intervals without panic.
5. Repeat disconnect/reconnect and Wi-Fi-change journeys after the first pass.

## Acceptance Criteria

- Provisioning reaches the robot online screen instead of timing out.
- Passive WebSocket churn does not make a claimed Wi-Fi-connected robot appear
  offline.
- Real Wi-Fi loss and invalid heartbeat credentials still stop or recover
  heartbeat correctly.
- Robot logs contain no panic, abort, assertion, or reboot during the physical
  verification window.
