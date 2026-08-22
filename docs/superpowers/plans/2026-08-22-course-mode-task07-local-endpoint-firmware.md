# Course Mode Task 07 Local Endpoint Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a default-off, fail-closed temporary firmware application that connects only to Task 07 private lab endpoints without endpoint/claim NVS mutation or production traffic.

**Architecture:** A host-testable endpoint policy validates compile-time URLs. OTA owns transient response configuration, WebSocket consumes it without NVS, activation skips production ownership traffic in local mode, and build identity labels the image lab-only. The normal flag-off path remains unchanged.

**Tech Stack:** ESP-IDF 5.5.4, C++17, Kconfig, Python 3/pytest, host-native shell tests

---

### Task 1: Define and test the local endpoint overlay contract

**Files:**
- Modify: `main/Kconfig.projbuild`
- Modify: `scripts/generate_lesson_storage_hil_local_config.py`
- Modify: `tests/test_lesson_storage_hil_local_config.py`

- [ ] Add failing tests requiring `--ota-url` and `--websocket-url` to emit `CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT=y`, rejecting DNS, loopback, link-local, public IP, credentials, query, fragment, malformed port, control/non-ASCII, and wrong schemes.
- [ ] Run `pytest -q tests/test_lesson_storage_hil_local_config.py` and confirm the new assertions fail for the missing flag/stricter policy.
- [ ] Add the default-off Kconfig flag and exact URL dependencies; implement the minimal generator changes.
- [ ] Rerun the focused test and confirm it passes.

### Task 2: Add a host-tested endpoint policy

**Files:**
- Create: `main/course_mode_local_endpoint_policy.h`
- Create: `main/course_mode_local_endpoint_policy.cc`
- Create: `tests/native/course_mode_local_endpoint_policy_test.cc`
- Create: `scripts/run_host_native_course_mode_local_endpoint_policy_test.sh`
- Modify: `main/CMakeLists.txt`

- [ ] Write failing native cases for approved private literal-IP HTTP/WS URLs and every rejected URL class from Task 1.
- [ ] Run the native script and confirm failure because the policy does not exist.
- [ ] Implement allocation-bounded parsing with no DNS/network access and explicit OTA/WebSocket entry points.
- [ ] Rerun the native script and confirm all cases pass.

### Task 3: Make OTA configuration RAM-only and fail closed

**Files:**
- Modify: `main/ota.h`
- Modify: `main/ota.cc`
- Create: `tests/test_course_mode_local_endpoint_firmware_contract.py`

- [ ] Add failing contract tests proving local mode never reads `wifi/ota_url`, persists recovered OTA, constructs endpoint/claim writers, accepts firmware/MQTT/API/claim-reset response fields, or produces more than the compiled OTA URL.
- [ ] Run `pytest -q tests/test_course_mode_local_endpoint_firmware_contract.py` and confirm the expected failures.
- [ ] Add transient WebSocket URL/token accessors and guarded local-mode OTA parsing; preserve the existing flag-off code path.
- [ ] Rerun the new test plus OTA-focused existing tests and confirm pass.

### Task 4: Bypass WebSocket NVS and production ownership traffic

**Files:**
- Modify: `main/protocols/websocket_protocol.h`
- Modify: `main/protocols/websocket_protocol.cc`
- Modify: `main/application.cc`
- Modify: `tests/test_course_mode_local_endpoint_firmware_contract.py`

- [ ] Add failing tests requiring transient configuration in local mode, forbidding WebSocket endpoint NVS reads, config refresh, heartbeat, claim polling/reset/release network paths, and allowing the existing production branch when the flag is off.
- [ ] Run the focused contract test and confirm the failures.
- [ ] Add the minimal transient constructor/setter and compile-time guards around activation/network ownership paths.
- [ ] Rerun focused claim/config/cloud-link/SD-sync regressions and confirm pass.

### Task 5: Pin lab-only build identity

**Files:**
- Modify: `main/esp_build_identity.cc`
- Modify: `tests/test_esp_build_identity_flash_safety_contract.py`
- Modify: `scripts/run_host_native_esp_build_identity_test.sh`

- [ ] Add failing assertions that local mode embeds `course-mode-task07-local-endpoint` and cannot embed `production`.
- [ ] Run the Python and native identity tests and confirm failure.
- [ ] Add the local-mode identity branch ahead of the existing HIL/production branches.
- [ ] Rerun both identity gates and confirm pass.

### Task 6: Run software regression and independent reviews

**Files:** All modified Task 1-5 files only.

- [ ] Run the focused tests from Tasks 1-5.
- [ ] Run `pytest -q tests/test_tbot_connect_config.py tests/test_tbot_cloudflare_links.py tests/test_tbot_claim_confirmation_contract.py tests/test_claim_confirmation_edge_cases.py tests/test_lesson_sd_sync_no_claim_gate_contract.py`.
- [ ] Run every relevant host-native script and the full firmware Python/native suites required by repository precedent.
- [ ] Run `git diff --check`, source scans for endpoint NVS writers/production traffic in the local branch, and a complete diff review.
- [ ] Obtain spec-compliance review, then code-quality/safety review; fix findings test-first and repeat reviews.

### Task 7: Produce the immutable temporary application bundle

**Files:**
- Create under task artifacts only: manifest, SHA256SUMS, source bundle, sdkconfig inputs, app binary, build logs, test logs, flash/readback/restore procedure.

- [ ] Generate the exact private-LAN overlay without recording credentials or secrets.
- [ ] Build twice in distinct task-owned directories using pinned ESP-IDF 5.5.4 inputs and `CONFIG_APP_REPRODUCIBLE_BUILD=y`.
- [ ] Require byte-identical `xiaozhi.bin`, size `<=3612672`, correct `0x20000` partition fit, expected lab-only identity, and no changed bootloader/partition/OTA/assets requirement.
- [ ] Pin every artifact hash and bundle-root hash; independently verify the manifest and restoration candidate before any flash.

### Task 8: App-only physical route and restoration

**Files:** Task artifact evidence only; no repository source changes.

- [ ] Reconfirm AC:20 identity, sole lease, two adults, stop/power isolation, serial owner, candidate app readback, and NVS SHA before mutation.
- [ ] Flash only the reviewed temporary app at `0x20000`; read back its exact byte length and compare SHA-256.
- [ ] Run only the attended local TFT/HIL lanes, stopping on privacy, identity, motion, heat, power, audio, or endpoint drift.
- [ ] Restore only the immutable candidate app at `0x20000`; read back the full 3,612,672-byte candidate region and NVS, and byte-compare both.
- [ ] Record that the temporary route does not itself satisfy calibrated Task 07 lanes or authorize Task 08.
