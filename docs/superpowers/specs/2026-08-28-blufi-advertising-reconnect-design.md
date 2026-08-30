# BluFi Advertising And Reconnect Design

## Goal

Make a robot that re-enters Wi-Fi setup advertise a complete, Android-discoverable BluFi identity, then prove repeated GATT lifecycle reuse without changing NVS, claim state, or stored Wi-Fi.

## Design

Keep the compact payload split already used by the firmware: flags and BluFi UUID `0xFFFF` in ADV, full `TBOT-*` identity in scan response. Replace the eager advertising start with a small Bluedroid GAP wrapper that forwards every event to ESP-IDF's BluFi handler and starts advertising only after both raw payload completion events report success. A configuration failure falls back to `esp_blufi_adv_start()` instead of leaving setup unavailable.

The wrapper resets its pending state for every setup initialization so a connect/deinit/reinit cycle cannot reuse stale completion bits. The change is limited to `main/boards/common/blufi.cpp` and a source-contract regression test.

## Verification

1. Demonstrate RED before implementation and GREEN after it.
2. Run the focused BluFi, Wi-Fi provisioning, security, and redaction suites.
3. Build the LCDWiki ESP32-S3 application and flash only offset `0x20000`.
4. On the attached Android phone, run at least three consecutive cycles of connect, BluFi service discovery, local disconnect, and reconnect without rebooting the robot.

