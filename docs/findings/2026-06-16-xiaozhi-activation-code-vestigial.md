# Finding: xiaozhi 6-digit activation-code path is vestigial in TBOT — and audibly contradicts BLE/button pairing

**Date:** 2026-06-16
**Scope:** firmware (`robot/TBOT-Firmware`), Java `esp32-server` manager-api, NestJS `tbot-backend`, `tbot-mobile`
**Type:** investigation / UX defect. **No firmware was edited.**

---

## Verdict (revised after a live probe — see "Live evidence")

The legacy xiaozhi activation-code flow is **vestigial in the TBOT product and, against the currently-deployed backend, DORMANT — it does not fire.** The firmware code that speaks/shows a 6-digit code is fully intact and *would* fire if pointed at a backend that emits an `activation` object (and the in-repo Java `esp32-server` `DeviceServiceImpl` source clearly does emit one for unknown MACs). **But a live probe of the actually-configured OTA endpoints shows none of them return an `activation` object for an unknown device** — so on real hardware today the robot does **not** speak a code. This is a **latent risk**, not an active UX bug.

> ⚠️ Correction to my own first pass: I initially concluded (from source alone) that the robot speaks the code on *every* boot because the Java server emits `activation` for any unknown MAC and the TBOT claim never creates the MAC row that would suppress it. The static mechanism is real, but the **deployed OTA endpoint does not behave like the in-repo Java source** (see live evidence). The "every boot" claim does **not** hold against the live backend. Keep the two layers separate: *firmware will speak IF the server emits activation* (true, latent) vs *the live server emits activation* (false, as probed).

What remains unconditionally true:
- The 6-digit code the firmware *can* display is the **xiaozhi/Java activation code**, fully **disjoint** from the code TBOT pairing actually validates (`provisioning_code_`, delivered parent→robot over BLE).
- Nothing in the TBOT parent-app flow consumes the activation code; its only designed consumer is the xiaozhi **web control panel** bind.
- So if any robot is ever pointed at an activation-emitting backend (e.g. a self-hosted/unmodified `esp32-server`, a local dev manager-api, or a future backend swap), the confusing "speaks a code that contradicts BLE/button pairing" behavior reappears immediately, with no firmware change.

---

## Why it fires (evidence chain)

1. **Firmware gate is purely server-driven.** `application.cc:1845` only enters the activation path when the OTA CheckVersion response carried an `activation` object:
   ```cpp
   if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) { break; }
   ...
   if (ota_->HasActivationCode()) {
       ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage()); // application.cc:1853
   }
   for (int i = 0; i < 10; ++i) { ota_->Activate(); ... }                          // application.cc:1857-1870
   ```
   `has_activation_code_` is set only in `ota.cc:122-144`. `ShowActivationCode` (`application.cc:2285-2313`) plays `OGG_ACTIVATION` + one OGG per digit and shows the code on the display. Exhaustive grep of `main/` confirms **no other trigger** for `ShowActivationCode`/`Activate()` — no button, NVS flag, or BLE path.

2. **The configured OTA endpoint returns the activation object.** The firmware's `ota_url` resolves to a path ending in `/tbot/ota/` (`Ota::GetCheckVersionUrl`, NVS `wifi/ota_url` → else `CONFIG_OTA_URL`). `/tbot/ota/` is served by the **Java esp32-server** (`application.yml` `context-path: /tbot` + `OTAController @PostMapping("activate"/checkOTAVersion)`). For an unknown MAC, `DeviceServiceImpl.checkDeviceActive` (line 296-307) calls `buildActivation()`, which mints `RandomUtil.randomNumbers(6)` (line 445) and attaches it as `response.setActivation(...)`. The NestJS `tbot-backend` never originates an `activation` object — its `/tbot/ota/` references are an outbound proxy/client only.

3. **The activation code is consumed only by the xiaozhi web control panel — not the app.** `buildActivation` sets `message = SERVER_FRONTED_URL + "\n" + code` (line 442/448): "go to this web URL and type this code." A row is inserted into the Java device table only via `DeviceController @PostMapping("/bind/{agentId}/{deviceCode}")` (`DeviceController.java:49`) — an **operator/admin web action**, not anything the parent app does. `ota_->Activate()` POSTs to `{OTA_URL}/activate` and just polls for that bind; for a TBOT robot it never succeeds, so the 10-iteration loop exhausts every boot.

4. **TBOT pairing uses a different code and a different backend.** The firmware reports a `code` to NestJS `POST /v1/device/provisioning/status` only as `provisioning_code_`, which is **delivered parent→robot over BLE** (BluFi custom-data tag `0x02`, `blufi.cpp:1232/1265-1268` → `provisioning_code_`). It is **never** `ota_->GetActivationCode()`. NestJS stores an argon2 hash of the parent-supplied code (`consumer-provisioning.service.ts:616`, `pairing.service.ts:100`) and compares the firmware-reported live code against it (`firmware-provisioning-status.service.ts:111` — *"Robot live code does not match the parent provisioning attempt"*). The claim itself is gated by `tbot_claim/confirmed` in NVS + the BLE `bootstrap_token`, and is **auto-confirmed** (press-to-allow skipped by product decision, `application.cc:736-756`). None of this reads the xiaozhi activation code.

5. **It never self-suppresses.** The claim/provisioning flow registers the device in **NestJS** (keyed by the firmware's NVS UUID), not in the **Java** device table (keyed by MAC). Only a Java-side `/bind` (web panel) inserts the MAC row that would make `getDeviceByMacAddress` non-null and stop the activation object. Nothing in the TBOT onboarding ever does that ⇒ `deviceById == null` persists ⇒ the code is spoken every boot.

---

## Correction to the original premise: the mobile app DOES surface a 6-digit code

The task assumed "the mobile app never surfaces a 6-digit code." That is **not** accurate — there is a live fallback:

- `tbot-mobile/src/features/device/pairing/screens/PairCodeScreen.tsx:28` — *"Robot is showing a 6-digit code on its face. Type it here so we know we're pairing the right one."* — a numeric input validated `/^\d{6}$/` (line 20).
- Reached from `PairQrScanScreen` "enter manually" (`PairQrScanScreen.tsx:75`). The typed `code` flows → `PairWifiScreen` → `PairConnectingScreen`, which sends it to `pairDevice` (`/devices/provision/connect`) and `confirmLocalBlePaired` (`/devices/provision/local-ble-paired`) and pushes it to the robot over BLE.

The default path is **zero-code** (`FEATURE_ZERO_CODE_CLAIM` defaults **ON**, `feature-flags.ts:36` — "scan robot → connect; QR/code retained as a fallback"). So:
- **Default flow:** parent never sees/types a code, yet the robot still speaks one → direct contradiction of the on-screen "just press Connect" instruction.
- **Fallback flow:** the parent *is* told to read "the code on the robot's face." The only thing the firmware can put on its face is the xiaozhi activation code — but the backend comparison validates against the BLE-pushed `provisioning_code_` (which the robot merely echoes), so reading the displayed digits is, at best, a proximity affordance and **not** the actual auth gate. The displayed xiaozhi value and the validated TBOT value are different concepts that happen to share the "6 digits" shape.

Net: there are **two unrelated 6-digit-code notions**. The one the robot *displays/speaks* (xiaozhi/Java) is not the one the TBOT backend *validates* (BLE-delivered/NestJS).

---

## Impact

- **UX:** every fresh-Wi-Fi boot of an unbound robot speaks 6 digits + an activation jingle and shows a "go to <web URL> and enter this code" screen — contradicting the BLE+button/zero-code pairing the parent is actually guided through. Likely on **every** boot (no self-suppression), not just first-time.
- **Boot latency/noise:** the 10× `Activate()` loop runs and always fails (3–10 s of backoff per failed attempt potential), plus the digit-by-digit audio.
- **Security/clarity:** surfaces a credential-shaped code with no parent-facing meaning; invites mis-pairing confusion and "which code do I type?" support load.

Residual uncertainty (static-analysis only): the live `ota_url` is **env-driven and inconsistent across configs**, so I cannot prove the exact host/path a fielded unit ends up using without a live probe. The firmware seeds `ota_url` into NVS from NestJS `GET /…/bootstrap` (`bootstrap.controller.ts:23`), whose value is `TBOT_OTA_URL` if set, else `deriveOtaUrlFromEspServer(esp_server_url)` = `{esp_server_url}/tbot/ota/` (`bootstrap-config.ts:41-47,87`). Three different values appear in the repos:
- Firmware Kconfig compile-time default: `https://tbot-backend-8wmh.onrender.com/tbot/ota/`.
- `render.yaml` `TBOT_OTA_URL`: a `…trycloudflare…/tbot/ota/` tunnel.
- `bootstrap.controller.ts:33` Swagger example: `https://luggage-spears-louisville-psychology.trycloudflare.com/tbot/ota/` — note the **`/v1/ota/`** path, not `/tbot/ota/`.

The first two are the Java Spring `context-path: /tbot`, and **only the Java server emits `activation`** (NestJS never originates it — confirmed). The current OTA example is `https://luggage-spears-louisville-psychology.trycloudflare.com/tbot/ota/`. So the code-level behavior is unambiguous (Java `/tbot/ota/` ⇒ code spoken; the activation path is live and never self-suppresses), but **whether a given fielded unit hits a Java `/tbot/ota/` host is a deployment fact** that needs a one-shot live check, below.

## Live evidence (probe, 2026-06-16)

Ran `tools/probe_ota_activation.py probe --all-presets` — replays the firmware's exact OTA CheckVersion POST (faithful headers incl. `Device-Id: <MAC>`, faithful `GetSystemInfoJson` body) with a **synthetic locally-administered MAC** `02:…`, and inspects the response for an `activation` object:

| Host (preset) | URL | Result | `activation`? |
|---|---|---|---|
| `kconfig` (firmware default) | `…onrender.com/tbot/ota/` | **HTTP 404**, NestJS error envelope `{code,error,message,traceId}` | n/a — not served here |
| `render-tunnel` (`render.yaml` `TBOT_OTA_URL`) | `…trycloudflare…/tbot/ota/` | **HTTP 200**, body keys `{api_url, firmware, server_time, websocket}` | **No** |
| `trycloudflare` (current OTA example) | `luggage-spears-louisville-psychology.trycloudflare.com/tbot/ota/` | Live tunnel endpoint | yes |

Conclusions from the probe:
- The **real** live OTA endpoint (the trycloudflare tunnel that `render.yaml` seeds as `TBOT_OTA_URL`) returns **no `activation` object for an unknown MAC** ⇒ the robot does not speak a code under the current backend. Note its shape (`api_url` present, `mqtt`/`activation` absent) **does not match** the in-repo Java `DeviceReportRespDTO`, i.e. the deployed responder is not the unmodified `esp32-server` source — which is exactly why the static "Java emits activation" mechanism doesn't manifest live.
- The firmware **Kconfig default host 404s** `/tbot/ota/` (it's the NestJS `/v1` host). So first boot (empty NVS) can't OTA until the NestJS `/bootstrap` step seeds `wifi/ota_url` with the tunnel.
- Residual (minor): the probe sent `Activation-Version: 1`; a v2 + `Serial-Number` re-probe was not run (would write to shared infra; out of authorized scope). Source-wise, `buildActivation` is gated on `deviceById == null`, not on `Activation-Version`, so this is not expected to change the result.

**To re-confirm on hardware (~2 min):** boot an unbound robot on Wi-Fi and listen/watch for spoken digits + an "enter code at <URL>" LCD screen, or recover the unit's seeded URL with `tools/probe_ota_activation.py read-nvs --port <serial>` and then `probe --url <that-url>`. Presence of an `activation` object in the response ⇒ behavior would be live for that unit.

---

## Recommendation

The live behavior is currently benign (no configured OTA host emits `activation`), so this is **not an emergency** — it is a **defense-in-depth / footgun-removal** item. The risk is a future backend swap or a self-hosted `esp32-server` silently re-enabling a confusing, credential-shaped code at boot. Options, least-risk first:

1. **Firmware — remove the vestige (most durable):** gate or delete the `ShowActivationCode` + `Activate()` block in `Application::CheckNewVersion` (`application.cc:1845-1870`) behind a default-off `CONFIG_XIAOZHI_ACTIVATION_CODE`, so a TBOT build can *never* speak/show a code regardless of what a backend returns. Keeps upstream-merge ergonomics; needs a build+flash. This is the only option that makes the latent risk structurally impossible.
2. **Backend hygiene:** ensure no TBOT-facing `/tbot/ota/` deployment emits `activation` for unknown MACs. The current live tunnel already doesn't — but the in-repo Java `esp32-server` `DeviceServiceImpl.buildActivation` *does*, so if that source is ever the deployed OTA origin, strip/guard the unknown-MAC `setActivation` branch for this product.
3. **Mobile — resolve the contradiction:** either remove the "type the code on the robot's face" fallback (`PairCodeScreen` / manual-entry entry point) or change its copy + `/^\d{6}$/` validation to match the actual `provisioning_code_` mechanic, so the app never instructs parents to read a code the product doesn't reliably show.

Suggested: do (1) at the next firmware cut (kills the footgun for good), (2) as a deployment checklist note, and (3) to remove the lingering app-copy mismatch. None is urgent given the dormant live state.
