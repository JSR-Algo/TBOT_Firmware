# BLE Searching Device Copy Design

## Goal

When the robot is actively advertising its BLE Wi-Fi setup service, show the
Vietnamese status `Đang tìm kiếm thiết bị...` instead of `Open TBot app`.

## Scope

- Change only the user-visible status for `TbotConnectState::BLE_SETUP_ADVERTISING`.
- Keep the existing copy for `WIFI_NOT_CONFIGURED` and `AP_SETUP_ACTIVE` because
  those states do not represent active BLE device discovery.
- Do not change BLE advertising, Wi-Fi provisioning, transition rules, timeout
  values, recovery actions, protocol UUIDs, or persisted pairing state.

## Implementation Shape

Add a dedicated localized status key for the BLE searching state, map
`BLE_SETUP_ADVERTISING` to that key, and update its state specification text.
Both English and Vietnamese locale contracts remain complete; the Vietnamese
screen output must be exactly `Đang tìm kiếm thiết bị...`.

## Verification

- Add or update the firmware mapper/locale contract test to distinguish BLE
  searching copy from the existing open-app copy.
- Run the focused TBOT connection-state tests.
- Build and flash the app partition without erasing NVS.
- Enter Wi-Fi setup with a double-click and confirm the robot shows the new text
  while ESP logs report successful BLE advertising.
- Continue the Android-to-robot provisioning E2E run with sanitized logs.
