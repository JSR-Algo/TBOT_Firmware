# TBOT Connection-Flow — Build & On-Hardware Verification Runbook

> Scope: the phone-to-robot claim/connection firmware work (plan §2.1, §3.x, §4
> C4/C5/C8/C-OTA, §6 P2/P3, §9). Active profile = **`build-blufi`** (BLE/BluFi
> provisioning; SoftAP is compiled OUT). There is **no ESP-IDF toolchain on the
> authoring machine** — this runbook is for the hardware box. The only check
> runnable off-hardware is the pytest contract suite (`python3 -m pytest`,
> see the last section).

This runbook assumes the orchestrator's Kconfig / sdkconfig changes are already
in the tree (corrected OTA host, `wss://` fallback, BLE timeout 300s). The
firmware **code** for the claim poll, local `expires_at` timeout, the 21-state
runtime mapper, the heartbeat POST, the BLE re-advertise cap, and the new copy
strings is implemented (see "Files changed" in the final report).

---

## 0. Prerequisites (hardware box)

```bash
# ESP-IDF must be installed and exported (v5.x as pinned by the project).
. $IDF_PATH/export.sh           # or: get_idf
idf.py --version                # sanity: prints the IDF version

cd /path/to/robot/TBOT-Firmware
```

The build directory `build-blufi` is the BLE/BluFi profile. If you do not have
it yet, create it from the project's blufi sdkconfig defaults (the orchestrator
owns those files; do not hand-edit them here).

---

## 1. Reconfigure to pick up the orchestrator's Kconfig / sdkconfig changes

The OTA host, the `wss://` fallback and `CONFIG_BLE_SETUP_TIMEOUT_SEC=300` are
**compiled** values. A plain incremental build will NOT re-read changed
`Kconfig.projbuild` / `sdkconfig.defaults*`; you must reconfigure:

```bash
idf.py -B build-blufi reconfigure
```

Confirm the compiled values landed:

```bash
grep -E 'CONFIG_(OTA_URL|WEBSOCKET_URL|BLE_SETUP_TIMEOUT_SEC)=' \
  build-blufi/config/sdkconfig
```

Expected (per locked decisions §9):
- `CONFIG_OTA_URL` = the current real ESP host (`luggage-spears-…`).
- `CONFIG_WEBSOCKET_URL` = the `wss://…` fallback (or the agreed placeholder).
- `CONFIG_BLE_SETUP_TIMEOUT_SEC=300`.

> If these are wrong, fix the Kconfig/sdkconfig (orchestrator's files) and
> re-run `reconfigure` — do not patch `build-blufi/config/sdkconfig.h` by hand.

---

## 2. Build

```bash
idf.py -B build-blufi build
```

A clean build of the connection-flow change touches:
`main/application.cc`, `main/application.h`, `main/tbot_connect_mapper.{h,cc}`,
`main/provisioning/claim_confirmation_reporter.{h,cc}`,
`main/boards/common/blufi.{h,cpp}`, the language assets, and `main/CMakeLists.txt`.
The new `tbot_connect_mapper.cc` must appear in the link line.

> The language header `main/assets/lang_config.h` is normally regenerated from
> `language.json` by `scripts/gen_lang.py` during the build. The new copy keys
> were also hand-synced into `lang_config.h`; after a real build, diff it to be
> sure the regenerated header still carries the six new keys
> (`READY_TO_CONNECT`, `OPEN_TBOT_APP`, `PRESS_BUTTON_TO_CONFIRM`,
> `SETUP_EXPIRED`, `SERVER_UNAVAILABLE_RETRYING`, `WIFI_FAILED_CHECK_PASSWORD`).

---

## 3. Flash + monitor

```bash
idf.py -B build-blufi -p /dev/ttyUSB0 flash monitor      # adjust the port
```

Keep the monitor open for the verification steps — every acceptance item below
emits a log line (`ESP_LOGI/ESP_LOGW`, tags `Application` / `BLUFI` /
`ClaimConfirm`).

---

## 4. On-hardware verification (per acceptance item)

### 4.1 Fresh-flash bootstrap reaches a LIVE OTA host (C-OTA)

1. Erase NVS so the device starts with empty creds:
   `idf.py -B build-blufi -p /dev/ttyUSB0 erase-flash` then flash again.
2. Provision Wi-Fi via the app (BluFi).
3. **Expect:** the monitor shows `CheckVersion` hitting the *current* OTA host
   (the `CONFIG_OTA_URL` from step 1), a 200, and the device persisting
   `backend.api_url` + `websocket.url` into NVS, then reaching Idle/ONLINE.
   - PASS if it bootstraps and connects; FAIL if it logs the stale
     `luggage-spears-…` tunnel or "Check version URL is not properly set".

### 4.2 Claim poll catches the parent's tap (C4)

1. With the robot Wi-Fi-configured but **unowned**, watch for
   `Claim poll started (every 4s, 300s cap)` and the screen showing
   **"Ready to connect"** (`READY_TO_CONNECT`).
2. In the app, start a claim (`POST /v1/claim/request`) for this device.
3. **Expect:** within ~4s a poll tick fetches `/device/config`, sees
   `status=WAITING_PHYSICAL_CONFIRM`, stops the poll, arms the expiry timer
   (`Claim expiry armed in <N>s`), and the screen shows
   **"Allow connection? Press button."** (`CLAIM_WAITING_CONFIRM`).
4. Press the robot button.
   - **Expect:** `POST /v1/claim/confirm` 2xx, creds persisted, screen
     **"Connected."**, and the device opens the ws session.
   - PASS if the tap is caught without a tight loop (ticks are ~4s apart, never
     back-to-back) and confirm succeeds.

### 4.3 `expires_at` local timeout shows "Setup expired" (C4)

1. Start a claim, then **do NOT press** the button.
2. **Expect:** when the backend `expires_at` elapses (or the 5-minute poll cap
   is reached, whichever first), the monitor logs
   `Claim poll window elapsed -> CLAIM_CONFIRM_TIMEOUT` or the expiry timer
   fires, and the screen shows **"Setup expired"** (`SETUP_EXPIRED`).
   - Negative check: pressing the button *after* expiry logs
     `Claim confirm ignored: window expired` and re-shows "Setup expired" —
     it must NOT blind-confirm.
   - PASS = no infinite "Allow connection?" spinner.

### 4.4 Heartbeat reports ble/ap = off when ONLINE (C5)

1. With the device claimed and online, watch for
   `Heartbeat started (every 20s)`.
2. On the backend, confirm `POST /v1/device/heartbeat` arrivals every ~20s.
3. **Expect:** the heartbeat body carries `"ble_state":"off"` and
   `"ap_state":"off"` (BLE is only up during explicit Wi-Fi setup in this
   build), plus `chip.temperature`.
   - Pre-claim negative check: before the device has a ws token, the heartbeat
     sender self-suppresses (no POST) — verify no heartbeat traffic until after
     a successful claim.
   - PASS = steady 15–30s cadence with ble/ap=off.

### 4.5 BLE stops after 300s and does not tight-loop (C8)

1. Enter Wi-Fi setup (long-press / first boot) so BluFi advertises; the screen
   shows **"Open TBot app"**.
2. Repeatedly connect+drop a BLE central (e.g. toggle the phone's BT, or a BLE
   scanner that connects then disconnects).
3. **Expect:** the monitor logs `BLE re-advertise 1/5 … 5/5 after disconnect`,
   and on the 6th disconnect logs
   `BLE re-advertise cap reached (5) — NOT restarting advertising`. Advertising
   does not restart after the cap.
4. Leave it idle past the timeout.
   - **Expect:** at 300s, `BLE setup TIMEOUT -> teardown posted to Application
     task`, the stack deinits, and the screen reflects the timeout
     (`BLE_SETUP_TIMEOUT` → "Setup expired").
   - PASS = no tight-loop restart under a flapping peer; BLE radio is off after
     timeout (the next heartbeat then reports `ble_state":"off"`).

### 4.6 Runtime state always clear (no silent failure / spinner)

Spot-check that each runtime phase renders a defined screen, all sourced from
the connect-state contract (`main/tbot_connect_state.h`) via the mapper
(`main/tbot_connect_mapper.cc`):
- Boot → "Starting"; Activating → "Loading setup..."; Connecting →
  "Connecting..."; Idle/online → "Connected".
- Kill the backend mid-session: screen shows
  **"Server unavailable. Retrying..."** (`OFFLINE_RETRY`), then recovers when
  the backend returns (no permanent spinner).

> Note: `AP_SETUP_ACTIVE` / `AP_SETUP_TIMEOUT` and the QR / pairing-code
> fallback states are **defined-but-dormant** in build-blufi (SoftAP compiled
> out; QR is mobile-driven). They are intentionally never reached here; do not
> treat their absence on-screen as a defect in this build.

---

## 5. Off-hardware check (authoring machine) — pytest contract suite

These text-scrape source (they do NOT compile firmware) and assert the new
behaviour exists. Run from the firmware root:

```bash
python3 -m pytest tests/ -q
```

Connection-flow-relevant tests (all expected to pass):
- `tests/test_tbot_claim_runtime_contract.py` — bounded poll + `expires_at`
  clock comparison + unowned→standby trigger.
- `tests/test_tbot_claim_confirmation_contract.py` — `expires_at`→epoch parse.
- `tests/test_tbot_connect_runtime_fsm_contract.py` — heartbeat POST sender,
  BLE re-advertise cap, the six new copy strings, the FSM mapper referencing
  the claim states.
- `tests/test_tbot_connect_state_contract.py` — the 21-state contract table.

> Known pre-existing failures unrelated to this work (config migration owned by
> the orchestrator + a docs-branding lint): `test_tbot_cloudflare_links.py`,
> `test_tbot_connect_config.py` (two cases), and
> `test_wifi_provisioning_brand.py`. These are NOT introduced by the
> connection-flow code change.
```
