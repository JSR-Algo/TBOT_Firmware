# Robot Claim Bootstrap Endpoint Design

## Problem

The Android app completes BluFi service discovery, sends the claim bootstrap
token and Wi-Fi credentials, and the robot joins Wi-Fi. The robot then derives
`https://esp.tjbot.vn/tbot/v1/device/bootstrap` from its provisioning-status
configuration. That ESP route returns plain text (`Server is running`) instead
of the backend bootstrap JSON, so firmware cannot obtain `api_url` and cannot
call `/v1/claim/confirm`.

## Design

Point the production provisioning-status configuration at the deployed backend
API origin:

`https://tbot-backend-8wmh.onrender.com/v1/device/provisioning/status`

The existing URL derivation then produces
`https://tbot-backend-8wmh.onrender.com/v1/device/bootstrap`, whose response
contains a non-empty `api_url`. No BLE UUID, characteristic, custom-data TLV,
claim payload, or mobile state-machine behavior changes.

## Validation

1. A configuration regression test requires the Kconfig default and LCDWiki
   build overlay to use the backend provisioning-status endpoint.
2. The focused firmware configuration test passes.
3. The LCDWiki firmware builds and flashes to `/dev/cu.usbmodem1101`.
4. A physical Android provisioning run shows Wi-Fi success followed by device
   config fetch and claim confirmation, and the app leaves the waiting screen.

