# WiFi Config Entry Transaction Design

## Goal

Rejecting or failing BLUFI setup must leave the current station connection,
device state, and `in_config_mode_` unchanged. Every WiFi configuration entry
path must cross the same setup transaction boundary.

## Transaction Boundary

`WifiBoard::StartWifiConfigMode()` is the sole owner of entry side effects.
Callers may check eligibility or delay an entry, but they do not stop the
station, stop the connection timer, reset the protocol, show the entry
notification, or change configuration state.

For BLUFI, `StartWifiConfigMode()` performs these steps before visible mutation:

1. Reserve the provisioning-session binding.
2. Begin and quiesce the audio provisioning lifecycle.
3. Commit the exact generation token to the binding.
4. Initialize BLUFI when its state is off or timed out and verify `ESP_OK`.

Only after the preflight succeeds does the function stop the connection timer
and station, reset the protocol, set `in_config_mode_`, transition to
`kDeviceStateWifiConfiguring`, show the notification, and arm the BLE timeout.
Hotspot and acoustic builds retain their existing setup behavior, with the
common visible mutations still owned by `StartWifiConfigMode()`.

## Failure Handling

- Reservation failure returns without calling Begin or changing external state.
- Begin failure releases the RAII reservation and changes no board/network state.
- Commit failure rearms the exact valid audio generation and returns without
  changing board/network state.
- BLUFI init failure first relies on transactional BLUFI cleanup. If BLE is
  proven off, the exact binding is cleared and the exact generation is rearmed.
  If BLE is not off, binding and lifecycle ownership remain fail-closed and an
  error is logged; station and board state still remain unchanged.

The binding clear operation is generation-specific, rejects active completion
or reservation ownership, and performs no allocation.

## Regression Coverage

Native lifecycle coverage models an active completion owner and proves rejected
entry leaves Begin calls, station stops, config flag changes, and device-state
updates at zero. Source contracts enforce that callers have no pre-entry
mutation and that BLUFI preflight, including successful init, precedes every
common visible mutation. Failure contracts cover exact binding clear and
generation-bound rearm semantics.
