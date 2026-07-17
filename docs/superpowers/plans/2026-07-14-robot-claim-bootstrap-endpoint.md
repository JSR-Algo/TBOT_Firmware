# Robot Claim Bootstrap Endpoint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route firmware provisioning bootstrap and status requests to the deployed backend so a Wi-Fi-connected robot can confirm its claim.

**Architecture:** Keep the existing bootstrap URL derivation and claim flow. Correct only the production provisioning-status configuration that supplies its origin, with a source-level regression test and a physical robot/Android verification.

**Tech Stack:** ESP-IDF/CMake Kconfig, Python pytest, Android ADB, ESP32-S3 UART.

---

### Task 1: Lock the production endpoint contract

**Files:**
- Modify: `tests/test_tbot_connect_config.py`

- [ ] **Step 1: Write the failing test**

Add assertions that `PROVISIONING_STATUS_URL` in `main/Kconfig.projbuild` and
`sdkconfig.defaults.local` equals
`https://tbot-backend-8wmh.onrender.com/v1/device/provisioning/status` and is
not hosted by `esp.tjbot.vn`.

- [ ] **Step 2: Verify the test fails for the current ESP health route**

Run: `python3 -m pytest tests/test_tbot_connect_config.py -q`

Expected: failure showing the current value is
`https://esp.tjbot.vn/tbot/v1/device/provisioning/status`.

### Task 2: Correct the firmware configuration

**Files:**
- Modify: `main/Kconfig.projbuild`
- Modify: `sdkconfig.defaults.local`

- [ ] **Step 1: Change the Kconfig default and LCDWiki overlay**

Set both production values to
`https://tbot-backend-8wmh.onrender.com/v1/device/provisioning/status`.

- [ ] **Step 2: Verify the focused test passes**

Run: `python3 -m pytest tests/test_tbot_connect_config.py -q`

Expected: all tests pass.

### Task 3: Build, flash, and run physical provisioning

**Files:**
- Generated: `build/xiaozhi.bin`

- [ ] **Step 1: Build and flash the LCDWiki firmware**

Run: `./build-lcdwiki.sh /dev/cu.usbmodem1101`

Expected: board guard passes, firmware builds, and flash reports success.

- [ ] **Step 2: Install and launch the Android release app**

Run:

```bash
adb install -r android/app/build/outputs/apk/release/app-release.apk
adb shell monkey -p com.TJBotmobile -c android.intent.category.LAUNCHER 1
```

Expected: `com.TJBotmobile/.MainActivity` is resumed.

- [ ] **Step 3: Capture UART and Android logs during one provisioning run**

Expected firmware evidence: Wi-Fi gets an IP, backend `api_url` is recovered,
device config is fetched, and claim confirmation returns HTTP 2xx.

Expected app evidence: the waiting screen advances instead of ending at
`Robot chưa xác nhận`.

