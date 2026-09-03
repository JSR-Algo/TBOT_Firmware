# BluFi Audio Memory Quiesce Evidence

Date: 2026-09-03

## Scope

Release the claimed robot's resident audio task stacks before BluFi starts and
restore them exactly once when the owning provisioning generation terminates.

## Reproduction

Physical failure before this patch:

```text
WifiBoard: WiFi connection timeout, entering config mode
BLUFI_CLASS: heap phase=blufi_init_finish internal_free=6611 internal_largest=6144 internal_dma_free=39 internal_dma_largest=16
BLE_INIT: Malloc failed
BT_HCI: opcode=0x2008, status=07: Memory Full
BT_HCI: opcode=0x2009, status=07: Memory Full
```

Android BLE scanning remained active but no matching TBOT advertisement was
visible.

## Test-First Evidence

- Native RED: `scripts/run_host_native_wake_word_lifecycle_test.sh` failed to
  compile because `audio/provisioning_audio_worker_state.h` did not exist.
- Native GREEN: lifecycle, AFE synchronization, wake-word telemetry, and model
  map executables all exited successfully.
- Source-contract RED: the new worker-drain test failed because
  `BeginWifiProvisioning()` did not stop or wait for resident audio workers.
- Source-contract GREEN: focused drain checks passed (`67 passed, 1 deselected`).
- Provisioning terminal-path gate passed (`149 passed`).
- Focused Wi-Fi/BluFi/audio release gate passed (`310 passed`).
- Full firmware suite passed (`1520 passed, 1 skipped` in 86.68 seconds).

## Production Build

- Branch tip built: `c3ffd2fcde12748f39be508bf18779a5d12757e9`
- ESP-IDF: 5.5.4 using the existing Python 3.9 environment
- Board gate: `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`
- Image: `build/xiaozhi.bin`
- Bytes: `3696432` (`0x386730`)
- SHA-256: `d7384eb128f7896033372815e7f7e9848abaf2ea388876913ba3082c8166e9b7`
- Smallest app partition: `0x3f0000`
- Free app space: `0x698d0` (10%)

The first build invocation selected system Python 3.14 and stopped before
configuration because no matching ESP-IDF virtual environment existed. The
successful build explicitly selected the already installed, previously verified
`idf5.5_py3.9_env`; no dependencies or toolchains were changed.

## Physical E2E

Pending flash and Android/robot verification. Passwords are intentionally not
recorded in this document.
