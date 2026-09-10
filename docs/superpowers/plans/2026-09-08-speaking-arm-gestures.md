# Speaking Arm Gestures Implementation Plan

> **For agentic workers:** Use subagent-driven-development with spec then quality review for each slice.

**Goal:** Small automatic arm gestures during actual conversational playback without blocking audio.

**Architecture:** A native-testable response-scoped controller owns cadence and cancellation. An isolated UART worker consumes current playback observations and dispatches only eligible targets. Explicit commands invalidate automatic ownership before UART serialization.

**Tech Stack:** C++17, ESP-IDF, FreeRTOS, UART, native ASan/UBSan and pytest.

Work in the user's current firmware checkout; preserve existing dirty audio/neweye
work. No server changes, motor test or flash until software verification and
correct target/setup are confirmed.

## 1. Controller

Files: new main/speaking_arm_gesture.h, tests/native/speaking_arm_gesture_test.cc,
tests/test_speaking_arm_gesture.py.

- [x] Write native RED tests: no target without playback, targets within0..20,
  at least1000ms between targets, no burst after delay, stale response rejected,
  same-response explicit cancellation persists, new response can restart.
- [x] Implement Observe(response, eligible, playback, now) and Cancel ownership;
  alternate left/right targets with a bounded four-step20/0 sequence. Return
  an optional target rather than sending hardware commands.
- [x] Run sanitizer tests; independent spec then quality review.
  Fresh coordinator run: 2026-09-08, pytest 1 passed (eight native scenarios).
  Spec and quality reviewers reported no actionable findings.

## 2. Playback And UART Integration

Files: main/audio/audio_service.h/.cc, main/application.h/.cc,
main/robot_uart.h/.cc, main/CMakeLists.txt if adding a worker implementation,
tests/test_speaking_arm_gesture.py and native controlled-effect fixtures.

- [x] Add failing tests for real playback evidence, stale queued playback,
  disconnect/lesson/state cancellation and explicit arm-command priority.
- [x] Publish response-scoped playback observation without UART/locking waits
  in the audio callback. No gesture on thinking, wake or sound effects.
- [x] Use a single bounded latest observation, not a growing motion queue.
  UART worker checks ownership immediately before write; unavailable UART skips.
  Explicit arm commands invalidate automatic ownership before serialization.
- [x] Preserve global servo speed and mirrored arm mapping; do not auto-center
  head or return arms after explicit commands. Already-sent servo movement is
  not represented as physically cancelled.
- [x] Test actual production controller and worker dispatch with fake transport;
  run robot UART, audio snapshot, realtime state and lesson regressions.
  Coordinator fresh run: 267 passed in 6.54s. Spec review in progress.
  Auto transport uses configured primary pins only, no alternate discovery.
  Output callback is digital software evidence, not acoustic proof.
- [x] Independent spec and quality review; correct findings before build release.
  Quality found missing HIL stop/rest ownership cancellation; two RED regressions
  added, corrected both entries, quality re-review has no remaining findings.
  Final combined verification: 347 passed in 31.98s; git diff --check clean.

## 3. Build And Handoff

- [x] Full ESP32-S3 build and neweye mapping/conversion tests; record exact hash.
  Final app: 3723536 bytes, 10% free, SHA256
  1810605a3e600abb5d600c266b06306f72601993ac9cfeb557cafcad09776907.
  Neweye: 4227994 bytes, SHA256
  6e32a0cff0231b5554570b9f662e4611a9bda1aed7273eb9f0a9a5e469a6dfb3.
- [x] Update product/story evidence truthfully, including physical NOT RUN.
- [x] Before flash resolve attached MAC (last observed14:c1:9f:d1:ac:20 differs
  from prior28:84:85:85:1a:80), screen attachment and safe robot setup. Back up
  app/assets before writes, preserve NVS/pairing/Wi-Fi. No motor command without
  attended safety confirmation. Hash verification alone is not E2E acceptance.
  User confirmed target/safety; read actual partition+OTA CRC, backed up both
  apps/all assets+metadata, flashed app/assets only, verified both and unchanged
  NVS/metadata. Boot2.2.93 and WebSocket/wake observed. Physical E2E pending.
