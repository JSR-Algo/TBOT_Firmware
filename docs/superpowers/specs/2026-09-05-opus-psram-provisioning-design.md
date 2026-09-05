# Opus PSRAM Provisioning Design

## Problem

During physical Wi-Fi provisioning, the audio service releases its workers and
reserves a contiguous 28 KiB internal-memory block for the Opus task before
starting Bluedroid. On the target ESP32-S3 this leaves Bluedroid unable to
allocate a 4,864-byte internal work queue. The allocation failure is followed
by a LoadProhibited panic and reboot, so BLE provisioning never starts.

The earlier reservation solved a different failure: after Bluedroid teardown,
internal heap fragmentation could leave no contiguous 28 KiB block for the
dynamic Opus stack. Both failures have the same root constraint: the Opus stack
and Bluedroid compete for scarce contiguous internal SRAM.

## Design

Create the Opus codec worker with `xTaskCreateWithCaps` using
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. The project already enables external task
stacks, and the Opus worker performs ordinary codec and queue work that does not
require DMA-capable or cache-disabled internal memory. The task must terminate
with `vTaskDeleteWithCaps` so ESP-IDF releases its capability-allocated TCB and
stack correctly.

Remove the 28 KiB internal Opus reservation from provisioning. Retain the
smaller post-BLE network-headroom reservation because Wi-Fi association and TLS
still need contiguous internal/DMA memory. Audio rearm no longer depends on
releasing an Opus reservation; it recreates the Opus worker directly in PSRAM.

## Failure Handling

- A failed PSRAM task creation remains fail-closed through the existing audio
  worker transaction and retry path.
- BLE setup startup failure rolls provisioning back through the existing token
  ownership path.
- Post-BLE network-headroom allocation failure restores BLE setup rather than
  committing credentials or leaving the robot stuck offline.
- Logs contain allocation sizes and heap metrics only, never Wi-Fi passwords.

## Verification

Automated regression checks must prove that the Opus worker uses the external
memory task API, uses PSRAM capabilities, and deletes itself with the matching
API. Existing provisioning ownership, teardown, scan, rearm, and mobile tests
must remain green. The firmware must build and flash successfully.

Physical validation starts at zero after the new image is flashed. Four full
cycles must each enter setup without BOOT, discover the target AP, provision,
obtain an IP on the exact requested SSID, restore all audio workers and wake
word processing, send a fresh accepted heartbeat, and return the app to an
online state. At least one additional attempt must cancel during BLE scan and
then retry successfully. No cycle may contain allocation failure, watchdog,
assertion, panic, or Guru Meditation.

## Scope

This change is limited to Android-to-robot Wi-Fi provisioning and the Opus task
memory placement required to make that flow stable. It does not alter codec
behavior, credentials, backend protocols, unrelated application screens, Git
history, branches, or worktrees.
