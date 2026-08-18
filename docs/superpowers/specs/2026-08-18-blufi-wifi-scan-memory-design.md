# BluFi Wi-Fi Scan Memory Design

## Problem

During Android provisioning, the robot can discover nearby Wi-Fi networks but fail while returning the list over BluFi. Hardware evidence from the production robot showed:

- repeated 180-byte internal/DMA allocation failures while an active Wi-Fi scan sent probe requests;
- `BLE_INIT: Malloc failed` when `esp_blufi_send_wifi_list()` attempted to allocate its notification buffers;
- a second BLE connection timing out during DH/AES negotiation until the robot was reset;
- successful provisioning only when the scan step was bypassed with manual SSID entry.

The existing code already caps the outgoing list and frees its C++ scan-result vector before dispatch. The remaining failure is caused by overlapping Wi-Fi scan cleanup and BLE notification work in the same low-memory window.

## Goals

- Return a useful, strongest-first Wi-Fi list without exhausting internal/DMA memory.
- Preserve the BLE session so the following secure provisioning connection works without a robot reset.
- Keep manual SSID entry as a fallback, not the normal recovery path.
- Avoid broad Bluetooth, LWIP, PSRAM, or boot-sequence configuration changes.

## Design

### Passive, bounded scanning

`Blufi::start_wifi_scan()` will pass an explicit `wifi_scan_config_t` to `esp_wifi_scan_start()` and use passive scanning. This removes active probe-request allocations that failed repeatedly on the production robot. The passive dwell time will be bounded so a full-channel scan remains responsive.

On scan completion, the firmware will request only a bounded number of the strongest driver-sorted records. The outgoing BluFi list remains capped at four unique SSIDs. Dense RF environments therefore cannot grow the application-owned result buffer in proportion to every visible AP.

### Deferred BluFi dispatch

The Wi-Fi scan callback will finish driver-result retrieval, clear scan ownership, and return before sending the BluFi notification. It will schedule the list dispatch onto the application task rather than calling `_send_wifi_list()` inline from `WIFI_EVENT_SCAN_DONE`.

The deferred dispatch captures the current provisioning generation and verifies that the generation and BLE session are still current before sending. A disconnect, reset, or new setup window therefore invalidates stale queued work.

### Failure behavior

If scan start, result retrieval, or deferred scheduling fails, the firmware will clear scan state and report `ESP_BLUFI_WIFI_SCAN_FAIL` when the BLE session is still valid. It will not leave `m_scan_in_progress` or `m_send_list_after_scan` latched.

No credentials, SSIDs, tokens, or device identifiers will be added to logs. Heap diagnostics remain numeric only.

## Testing

### Automated regression tests

Source-contract tests will require:

- an explicit passive scan configuration rather than `esp_wifi_scan_start(NULL, false)`;
- a bounded scan-result candidate count;
- release/reset of scan state before dispatch;
- deferred dispatch outside the Wi-Fi event callback;
- provisioning-generation and active-session validation before the delayed send.

The new tests must fail on the current implementation before production code changes and pass afterward. Existing BluFi provisioning, security, Wi-Fi scan, and log-redaction suites must remain green.

### Build and hardware verification

The firmware must build successfully for the connected robot and be flashed locally. A real Android `Doi Wi-Fi` flow will then verify:

1. the robot returns a scanned Wi-Fi list;
2. the target network is selectable without manual SSID fallback;
3. a subsequent DH/AES provisioning connection completes without resetting the robot;
4. credentials are received and the robot reconnects;
5. serial output contains no `heap_alloc_failed`, `BLE_INIT: Malloc failed`, or BluFi security timeout evidence during the flow;
6. the mobile device page reports the robot online after setup.

## Integration And Cleanup

The fix will be committed on `fix/blufi-wifi-scan-memory`, reviewed, merged locally into `main`, and re-verified on the merged commit. Clean auxiliary worktrees will be removed afterward. Branches whose commits are not ancestors of `main` will be preserved even when their worktree checkout is removed.
