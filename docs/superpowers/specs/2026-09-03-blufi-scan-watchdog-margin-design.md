# BluFi Scan Watchdog Margin Design

## Problem

Physical Android-to-robot testing shows that BluFi advertising and GATT setup
now succeed, but the Wi-Fi list remains stuck at `Dang quet tu Robot...`.
Firmware logs show this repeating sequence:

1. BluFi starts an owned passive Wi-Fi scan.
2. The scan completes approximately 5 seconds later.
3. BluFi rejects `WIFI_EVENT_SCAN_DONE` as not owned.
4. Recovery restarts the Wi-Fi driver and submits another scan.

The BluFi scan watchdog is also armed for exactly 5 seconds. The watchdog can
therefore begin lease recovery at the same boundary where the ESP-IDF passive
scan posts its completion event. Once the lease enters recovery, the otherwise
valid completion callback is correctly rejected by the ownership coordinator.

## Constraints

- Preserve passive scanning and the existing global scan-lease ownership model.
- Do not weaken generation, connection-epoch, or lease validation.
- Preserve recovery for a genuinely stalled ESP-IDF scan.
- Do not expose Wi-Fi credentials in tests, logs, or QA evidence.
- Keep the change limited to the scan watchdog timing and its regression tests.

## Considered Approaches

### 1. Add a watchdog margin (selected)

Arm the BluFi scan watchdog at 20 seconds instead of 5 seconds. Physical testing
showed that an 8-second watchdog still aborted every scan at its exact deadline.
A temporary 30-second diagnostic build completed the same passive scan and made
the list visible on Android in approximately 14 seconds including UI polling.
Twenty seconds leaves margin for BLE/Wi-Fi coexistence while retaining bounded
recovery for a stalled scan.

This is the smallest change and does not alter ownership semantics or scan
results.

### 2. Switch BluFi to active scanning

Active scans may finish sooner, but change radio behavior, power use, and which
networks are discovered. This is broader than the observed defect.

### 3. Accept completion callbacks after recovery begins

Allowing a late callback to reclaim a draining lease would complicate the
coordinator and could permit stale callbacks to mutate a newer scan session.
This weakens the safety property the ownership layer is designed to enforce.

## Design

Introduce a named BluFi scan watchdog duration of 20 seconds and use it when
arming `wifi_scan_watchdog_timer_`. Keep the current exact tuple of request ID,
lease ID, driver incarnation, setup generation, BLE session state, and BLE
connection epoch unchanged.

The watchdog continues to request recovery only when the exact tuple is still
current and the logical controller still owes a callback. A normal passive scan,
including the measured coexistence-delayed case, is consumed before the watchdog
fires, which disarms the watchdog through the existing completion path.

## Test Strategy

Use test-driven development:

1. Add a source/contract regression test requiring the measured 20-second
   watchdog duration, greater than the nominal 5-second passive scan boundary.
2. Add or extend the native timer/controller test to show that a delayed normal
   completion at 12 seconds wins and prevents later recovery at 20 seconds.
3. Run the focused BluFi Wi-Fi scan contract and native suites.
4. Run the full firmware test suite and the LCDWiki ESP32-S3 production build.
5. Flash the robot and verify physically that Android receives a Wi-Fi list.
6. Continue provisioning with `Van Phong Tam Dentist` and exercise reconnect and
   Wi-Fi-switch scenarios without pressing BOOT.

## Success Criteria

- No repeated `Ignoring WiFi scan done event not owned by BluFi` loop during a
  normal scan.
- Android receives and displays the robot's Wi-Fi list.
- A genuinely stalled scan still enters recovery after the 20-second watchdog.
- Existing scan ownership, recovery, lifecycle, and provisioning tests pass.
- Physical provisioning continues without `Memory Full`, heap allocation
  failures, or manual BOOT entry.
