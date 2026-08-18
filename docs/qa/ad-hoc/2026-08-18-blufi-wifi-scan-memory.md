# BluFi Wi-Fi Scan Memory Verification - 2026-08-18

## Scope

This record covers the `fix/blufi-wifi-scan-memory` branch through implementation
commit `4d838612d6df7486faccdb011f1a3b9cfdf1ed15`. It verifies the source-contract
regressions, the scoped provisioning gate, and an LCDWiki ES3C35P no-flash build.
Physical flashing and the Android Wi-Fi scan/provisioning flow remain pending.

## Root-Cause Evidence

Production-robot serial and operator evidence, recorded without credential, SSID,
token, device-identifier, or account values, showed:

- repeated 180-byte internal/DMA allocation failures during an active Wi-Fi scan;
- `BLE_INIT: Malloc failed` while `esp_blufi_send_wifi_list()` allocated notification
  buffers;
- a later BLE connection timing out during DH/AES negotiation until robot reset;
- provisioning succeeding when robot-side scanning was bypassed with manual SSID
  entry.

The outgoing result list was already capped and its C++ record vector was released
before notification. The remaining low-memory overlap was active scan work, driver
result ownership, and BluFi notification allocation occurring in the same window.

## Commits Under Test

```text
1d647488bf8ed79207f6e9d5ba1af18e5d25cd16 test(blufi): reproduce scan memory starvation
ca5b683e49badc0a5d84b638fbb3746872eaa80f test(blufi): tighten scan memory contracts
fbdde44164ca6ab938fa89a51ff11c0fea3b251a fix(blufi): defer passive Wi-Fi scan delivery
c6d2249131d1a6984f13941d6eddc4da0f01b626 fix(blufi): bind deferred scan to BLE session
4d838612d6df7486faccdb011f1a3b9cfdf1ed15 fix(blufi): own deferred Wi-Fi scan results
```

## Task 1 RED

The plan's exact focused command was run against the final test-only commit
`ca5b683e49badc0a5d84b638fbb3746872eaa80f` before either production fix:

```bash
python3 -m pytest -q \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py -x
```

Result: exit `1`; `1 failed, 1 passed`. The first failure was
`test_blufi_wifi_scan_is_passive_to_preserve_internal_dma_heap`, because
`Blufi::start_wifi_scan()` had no explicit `wifi_scan_config_t scan_config` and
still used the default active scan path.

A supplemental run without `-x` exposed all three intended regression failures:

```text
3 failed, 76 passed; exit 1
test_blufi_wifi_scan_is_passive_to_preserve_internal_dma_heap
test_blufi_wifi_scan_caps_application_owned_candidates
test_blufi_wifi_list_dispatch_is_deferred_and_guarded_until_scan_callback_returns
```

The failures respectively proved the absence of passive scanning, bounded
application-owned candidates, and deferred/session-guarded list dispatch.

## Task 2 GREEN

At branch HEAD, the planned focused GREEN group was:

```bash
python3 -m pytest -q \
  tests/test_blufi_wifi_scan_contract.py \
  tests/test_blufi_provisioning_stability.py \
  tests/test_blufi_security_and_events.py \
  tests/test_provisioning_log_redaction.py
```

Result: exit `0`; `126 passed in 0.08s`.

The implementation uses an explicit bounded passive scan, caps driver-result
retrieval, clears the driver AP list, releases scan ownership before scheduling,
and sends the capped/deduplicated result from the application task.

## Cross-Connection Race Review

Review of the first deferred implementation found that setup generation and a
connected boolean were insufficient: client A could disconnect after scan
completion, client B could reconnect within the same setup generation, and client
A's queued result could then be delivered to client B.

Commit `c6d2249131d1a6984f13941d6eddc4da0f01b626` closes that race. Scan completion
captures the encoded BLE session state and a monotonic `ble_connection_epoch_`
under `provisioning_finalization_mutex_`. Each accepted BLE connection increments
the epoch. Deferred dispatch requires the setup generation, connected session
state, and exact connection epoch all to match; disconnect also clears the pending
send flag. A stale queued list is discarded and its cached records are released
rather than crossing into a replacement connection.

## Deferred-Result Ownership Race Review

Review after the connection-epoch fix found a second race in the first deferred
implementation. The queued callback still accessed the shared `m_ap_records`
member. A repeated list request could start or consume newer scan state before the
queued response ran, and an invalidated stale callback could clear records now
owned by a newer request. Session identity prevented cross-client delivery, but it
did not establish exclusive ownership of the result vector or serialize retries
behind the queued dispatch.

The ownership regression was verified by running the current focused tests against
the prior `c6d2249` production files:

```text
2 failed, 124 passed; exit 1
test_blufi_wifi_list_retry_waits_for_owned_deferred_dispatch
test_blufi_stale_deferred_dispatch_only_destroys_its_owned_records
```

Commit `4d838612d6df7486faccdb011f1a3b9cfdf1ed15` fixes the race by transferring
the completed scan vector out of shared state and moving it into the deferred
callback. A monotonic dispatch epoch
and pending-dispatch token reject retries while that owned response is queued and
invalidate it on disconnect, restart, init, or deinit. The callback can destroy
only its captured vector; it no longer clears or sends whatever records happen to
be in `m_ap_records` when it eventually runs. Review confirmed that the pending
token is cleared with compare-and-exchange only by its owning dispatch and that
the existing setup-generation, BLE-session-state, and BLE-connection-epoch gates
remain required before send.

## Scoped Automated Gate

Command:

```bash
python3 -m pytest -q \
  tests/test_blufi_*.py \
  tests/test_wifi_board_provisioning.py \
  tests/test_wifi_provisioning_brand.py \
  tests/test_provisioning_log_redaction.py \
  tests/test_provisioning_success_teardown_contract.py \
  tests/test_tbot_connect_runtime_fsm_contract.py
```

Result: exit `0`; `203 passed in 0.37s`.

## LCDWiki Production Build

Command:

```bash
PATH="/usr/bin:$PATH" ./build-lcdwiki.sh --no-flash
```

Result: exit `0`.

```text
board guard: OK: LCDWiki ES3C35P board confirmed in sdkconfig.
build result: DONE: built + verified LCDWiki image (skipped flash; --no-flash).
artifact: build/xiaozhi.bin
artifact size: 3778752 bytes (0x39a8c0)
smallest app partition free: 0x55740 bytes (8%)
SHA-256: 7479cdea7fe0f1778a19b1a82dd1f633f3e7da2bef48ee8e2a6c65bfaa071964
```

The build emitted one non-fatal `-Wunused-but-set-variable` warning for
`provisioning_token` in `Blufi::StartStationConnectFromCredentials`. No tracked
configuration was changed; generated `sdkconfig`, `dependencies.lock`,
`managed_components`, language output, and `build` content remain ignored.

## Hardware Checklist - PENDING

- [ ] **PENDING:** Verify the intended ESP32-S3 serial target and Android device.
- [ ] **PENDING:** Flash the exact merged-main artifact and verify flash hashes.
- [ ] **PENDING:** Run the Android `Doi Wi-Fi` flow and receive a robot-scanned list.
- [ ] **PENDING:** Select the target network without manual SSID fallback.
- [ ] **PENDING:** Complete the following DH/AES BluFi connection without robot reset.
- [ ] **PENDING:** Confirm credentials are accepted and the robot reconnects.
- [ ] **PENDING:** Confirm serial output has no scan allocation, BLE allocation, or
  BluFi security-timeout evidence during the physical flow.
- [ ] **PENDING:** Confirm the mobile device page reports the robot online.

No firmware was flashed and no live Wi-Fi scan or provisioning success is claimed
by this branch-verification record.

## Self-Review

- No credential, SSID, token, account, device identifier, or other secret value is
  present.
- No incomplete evidence marker is presented as completed evidence.
- Automated and build results are limited to the exact commands and branch HEAD
  recorded here.
- Hardware validation is explicitly pending; there is no flash or live-scan claim.
- `git diff --check` is required to pass before this record is committed.
