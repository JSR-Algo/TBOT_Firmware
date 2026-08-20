# T6.5 Residual Firmware Pytest Repair Evidence

Date: 2026-08-21

Scope: firmware-only software release gates on branch
`fix/t65-firmware-product-failures`, based on local `main` (`737b398`) plus
`965b07d`, `ac1688b`, and `9ea50a7` in that order. No hardware, deployment,
flash, main push, or T6.5/T7 status edit was performed.

## Exact Baseline

Command:

```sh
python3 -m pytest -q tests
```

Result at `9ea50a7`: `7 failed, 1231 passed, 1 skipped in 41.11s`.

The seven failing node IDs and classifications were:

1. `tests/test_goal2_canonical_port_contract.py::test_goal2_runtime_integration_points_exist_on_canonical_architecture`
   - Classification: **(b) stale static-contract test**.
   - Evidence: the test required `websocket_->Connect` after setting headers on
     `replacement_websocket`; the runtime has intentionally connected the
     replacement socket before publishing it into `websocket_` since
     `cacb2d7`, preserving connection-local ownership.
2. `tests/test_goal2_canonical_port_contract.py::test_incoming_lesson_frame_uses_originating_transport_epoch`
   - Classification: **(b) stale static-contract test**.
   - Evidence: the test required an exact three-item capture list, but the
     connection-local `hello_signal` added by `cacb2d7` must also be captured.
     The callback still captures and forwards both `connection_epoch` and
     `callback_transport_epoch`.
3. `tests/test_goal2_firmware_integration_rebind_contract.py::test_websocket_emits_build_identity_on_canonical_transport_before_connect`
   - Classification: **(b) stale static-contract test**.
   - Evidence: identical retired `websocket_->Connect` spelling; the build
     identity headers are set on and connected through the unpublished
     replacement socket.
4. `tests/test_lesson_jpeg_decode_contract.py::test_rom_decoder_component_is_direct_and_new_jpeg_remains_encoder_only`
   - Classification: **(c) environment-generated sdkconfig prerequisite**.
   - Evidence: repository `.gitignore` excludes resolved `sdkconfig`; ESP-IDF
     generates it per build. The component Kconfig defaults `JD_USE_ROM=y`
     when ROM JPEG decode is available, and `scripts/verify_rom_jpeg_build.sh`
     verifies the resolved target config and linked symbols.
5. `tests/test_tbot_connect_config.py::test_local_firmware_build_configs_compile_no_ephemeral_websocket_seed`
   - Classification: **(c) environment-generated sdkconfig prerequisite**.
   - Evidence: the static test prepended an ignored, absent resolved
     `sdkconfig` to the maintained defaults list.
6. `tests/test_tbot_connect_config.py::test_local_firmware_build_configs_compile_only_current_production_ota_seed`
   - Classification: **(c) environment-generated sdkconfig prerequisite**.
   - Evidence: identical absent generated-input assumption. The resolved
     production OTA seed remains enforced by
     `scripts/assert_lcdwiki_prod_config.py`.
7. `tests/test_tbot_connect_config.py::test_websocket_protocol_adds_identity_query_params_without_auth_query_or_logging_token`
   - Classification: **(b) stale static-contract test**.
   - Evidence: query construction and token privacy remain intact, but the
     final connect correctly uses `replacement_websocket` instead of publishing
     ownership early through `websocket_`.

No failure was classified as **(a) a real current product/build defect**. This
classification does not dismiss the failures: each stale assertion was rebound
to the current ownership contract, and each generated-config dependency was
split into a checkout-safe static check plus an explicit resolved-build
preflight.

## RED To GREEN

- Original seven-node focused run: `7 failed in 0.11s`.
- Repaired seven-node focused run: `7 passed in 0.08s`.
- New resolved-config RED: a production LCDWiki config containing
  `CONFIG_WEBSOCKET_URL="wss://preview.example/tbot/v1/"` incorrectly passed the
  production preflight.
- New resolved-config GREEN: the same fixture is rejected with
  `WebSocket URL must be ...`; the canonical LCDWiki reference config passes.
- The full-Python runner no longer writes a synthetic root `sdkconfig`; it runs
  pytest directly and retains the before/after tracked-tree integrity check.

## Verification

Affected Python contracts:

```text
61 passed in 0.16s
```

Preserved repair contracts:

```text
29 passed in 0.10s
lesson host test OK (2646 checks)
passive websocket liveness host test passed
jpeg_to_image host external-TJPG parity and ASan boundary checks passed
PASS: esp build identity native contract
Lesson storage HIL status contention tests passed
```

The physical SD identity, SD FAT session guard, SD guard registry lifecycle,
and renderer trace native runners also exited `0`.

Full direct firmware pytest:

```text
1239 passed, 1 skipped in 28.17s
```

Tree-integrity wrapper:

```text
1239 passed, 1 skipped in 27.45s
WORKTREE_SNAPSHOT_SHA256=78b10104ff2c51268aad7209db35f3c56d734db4fbf357d6826bef93e3591149
WORKTREE_SNAPSHOT_ENTRIES=2118
```

ESP-IDF build:

- Toolchain: `ESP-IDF v5.5.4`
- Build directory: `build-t65-residual`
- Target/profile: LCDWiki ESP32-S3 production defaults chain
- Result: success; `xiaozhi.bin` size `3596896`, app partition `13%` free
- Resolved `sdkconfig` SHA-256:
  `2d9949bef4d0000a1e8d2abd49812020118bc85ebe140c60484bf0602368c008`
- Binary SHA-256:
  `b1184f36d27cdf3f3e0a01a3d6bed7e5252613274da797ea9009f6bb22cbbe0c`
- ELF SHA-256:
  `d5c146902cbe7d0ef2d68c32077d4d154236179dda1a4c2528c250aa4dfa74d3`
- Map SHA-256:
  `07457ee7fadc3deda05a04957580ef0fc6496472e1e6adab2bce036232c76143`
- `python3 scripts/assert_lcdwiki_prod_config.py build-t65-residual/sdkconfig`:
  `LCDWiki production build config OK`
- Resolved proof includes `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`, production
  OTA/WebSocket URLs, and `CONFIG_JD_USE_ROM=y`.

`git diff --check` exited `0`. Generated build artifacts remain ignored and
uncommitted.

## Independent Review

An independent code-review agent inspected the nine changed files, relevant
runtime ownership, and release-gate behavior. It reported `0` critical, high,
medium, or low findings and recommended approval for commit. Its independent
checks included `git diff --check`, Python compilation, the exact seven nodes,
the wrapper on those nodes, all modified contract files, and both passing and
rejecting LCDWiki production-config fixtures.
