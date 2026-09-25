# Wake Sync Fairness Implementation Plan

> **For agentic workers:** Use subagent-driven-development with test-first work
> and sequential spec/quality review. Existing checkout, preserve unrelated edits.

**Goal:** Prevent repeated background sync from starving Hi ESP and conversation.
**Architecture:** Add bounded admission state around existing quiet/rearm hooks;
reuse Application audio cleanup and existing MCP busy response, no new tasks.
**Tech Stack:** C++17, ESP-IDF5.5, native pytest compiler fixtures.

- [x] Execute real Application admission/rearm methods in a native fixture using
  fake clock, timers and audio; reproduce repeated requests every420..1450ms.
  Test fails when a new quiet is admitted before successful wake rearm+3000ms.
- [x] Track pending post-sync wake opportunity and one monotonic admission
  deadline; evaluate before quiet CAS/timer stop/audio changes. Once actual
  IsWakeWordRunning is true, start deadline once; repeated busy requests must not
  move it. Retain1500ms rearm settling and existing owner/source guards.
- [x] Reject background sync during conversation; preserve allowed true legacy
  passive behavior only where it cannot represent an active conversation.
- [x] Run new fixture plus tests/test_lesson_sd_sync_worker_contract.py,
  tests/test_chat_source_activation.py and tests/test_speaking_arm_gesture.py.
- [x] Sequential independent spec and quality review; fix concrete findings.
- [x] Run python3 -m pytest tests -q --tb=short and ESP-IDF target build. Record
  new hashes; do not reuse c35037ee identity for modified source.
- [x] Read current MAC/layout/OTA, retain rollback, flash only application and
  verify_flash. No bootloader/NVS/assets rewrite. Fresh attendance before motion.
- [ ] Capture real wake/conversation/audio/motion results; update US-004 evidence.

Do not commit unrelated existing edits or change VPS configuration for this fix.

Review correction: preserve consecutive unclaimed public sync without mic startup.
SetDeviceState publishes atomic invalidation; Application alone resets the plain
deadline. Native tests cover concurrent callback departure and invalidation
during admission. Implementer final focused run: 53 passed, including ASan/UBSan
and TSan. Final root full suite: 1710 passed, 2 optional skips, 197.01s.
Target build passes: app 3815680 bytes, SHA256
ee5ed3594c9aa4bce1e91ecb535dc282eeea033b33f1cec360928462ab5e494d.
Sequential spec and quality re-reviews pass. Hardware gates remain open.
