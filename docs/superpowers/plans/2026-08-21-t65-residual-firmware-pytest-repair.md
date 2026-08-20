# T6.5 Residual Firmware Pytest Repair Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the firmware pytest release gate self-contained with no unexplained failures while preserving current WebSocket replacement/epoch ownership and all lesson disconnect/audio repairs.

**Architecture:** Repair source-contract tests so they assert the current ownership model instead of retired implementation spellings. Separate maintained sdkconfig defaults from the ignored, build-generated resolved `sdkconfig`; static pytest checks maintained inputs, while the existing build preflight remains responsible for resolved configuration.

**Tech Stack:** Python 3 pytest source contracts, Bash release-gate runner, ESP-IDF 5.5.4 verification when product/build inputs change.

---

### Task 1: Freeze the seven-failure baseline and classification

**Files:**
- Create: `docs/qa/ad-hoc/2026-08-21-t65-residual-firmware-pytest.md`

- [x] **Step 1: Record the exact baseline**

Record `python3 -m pytest -q tests` as `7 failed, 1231 passed, 1 skipped` and list all seven node IDs.

- [x] **Step 2: Record root-cause evidence**

Classify the four WebSocket assertions as stale static contracts and the three missing `sdkconfig` assertions as environment-generated prerequisites. Record why none justify changing runtime ownership.

### Task 2: Repair stale WebSocket source contracts

**Files:**
- Modify: `tests/test_goal2_canonical_port_contract.py`
- Modify: `tests/test_goal2_firmware_integration_rebind_contract.py`
- Modify: `tests/test_tbot_connect_config.py`

- [x] **Step 1: Confirm RED**

Run the four failing WebSocket node IDs and confirm they fail on retired `websocket_->Connect` or the obsolete exact capture list.

- [x] **Step 2: Assert current ownership**

Require build headers and query parameters before `replacement_websocket->Connect`, require the callback to be installed on `candidate_websocket`, and require the capture list to include `connection_epoch`, `callback_transport_epoch`, and the connection-local hello signal.

- [x] **Step 3: Confirm GREEN**

Run the same four node IDs and the complete affected Python files.

### Task 3: Make sdkconfig contracts checkout-safe

**Files:**
- Modify: `tests/test_lesson_jpeg_decode_contract.py`
- Modify: `tests/test_tbot_connect_config.py`
- Modify: `tests/test_goal2_firmware_integration_rebind_contract.py`

- [x] **Step 1: Confirm RED**

Run the three failing sdkconfig node IDs from a clean checkout without generated `sdkconfig`.

- [x] **Step 2: Separate maintained and generated inputs**

Check endpoint seeds only in tracked `sdkconfig.defaults*`. Check the JPEG ROM default in the component Kconfig and keep the resolved-config assertion in `scripts/verify_rom_jpeg_build.sh`. Freeze the full-Python runner as the reproducible generated-sdkconfig preflight.

- [x] **Step 3: Confirm GREEN and cleanup truth**

Run the three node IDs directly and through `scripts/run_goal2_firmware_rebind_full_python.sh`; verify the runner restores an absent or existing `sdkconfig` and leaves the worktree unchanged.

### Task 4: Verify and review

**Files:**
- Modify: `docs/qa/ad-hoc/2026-08-21-t65-residual-firmware-pytest.md`

- [x] **Step 1: Run affected native and host gates**

Run the WebSocket passive-liveness, lesson handler, lesson renderer trace, audio, and JPEG host-native runners affected by the preserved repair stack.

- [x] **Step 2: Run complete software gates**

Run `python3 -m pytest -q tests`, `scripts/run_goal2_firmware_rebind_full_python.sh tests`, and `git diff --check`.

- [x] **Step 3: Apply the conditional build gate**

If any firmware product or build input changed, source `/Users/manhhodinh/esp/esp-idf/export.sh`, confirm ESP-IDF 5.5.4, and build a clean LCDWiki ESP32-S3 production configuration. Otherwise record that the build trigger was not met.

- [x] **Step 4: Obtain independent review and commit**

Request an independent code review of the scoped diff, repair all important findings, rerun verification, and commit only the test/gate/evidence changes.
