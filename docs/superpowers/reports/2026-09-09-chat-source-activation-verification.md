# Chat Source Activation Verification

## Scope

Local dirty firmware candidate only. No commit, deployment, serial access, flash,
or motor commands. Existing unrelated checkout changes are preserved.

## Review Corrections

- Lesson shutdown closes producer admission and joins admitted producers before
  freeing queue storage; task retirement also requires kernel-confirmed suspension.
- Storage task body returns before the task entry self-deletes. This releases C++
  owners on normal and failsafe paths. The regression checks completion ownership
  at deletion and permit reclamation without simulated stack unwinding.
- Unsupported codecs retain legacy routing and omit conversationAudioDrainAck.
  The immutable support query defaults false and is true for ES8311; supported
  initialization still requires all workers and six source callbacks.

Full implementation spec review passes, including scoped codec re-review
2passed4.55s. Full quality review passes after its codec finding was corrected;
scoped actual initialization/hello tests2passed2.31s. Neither review approves
target scheduling, acoustic behavior, or production deployment.

## Root Verification

Final expanded suite:68passed92.78s, no failures or skips.

```sh
CHAT_APPLICATION_TSAN=1 CHAT_OUTBOUND_TSAN=1 CHAT_MAILBOX_TSAN=1 python3 -m pytest tests/test_chat_connection_messages.py tests/test_chat_protocol_source.py tests/test_chat_outbound_application.py tests/test_chat_outbound_worker.py tests/test_chat_outbound_mailbox.py tests/test_chat_playout_intake.py tests/test_chat_start_handoff.py tests/test_chat_source_activation.py -q --tb=short
```

Final full suite: `python3 -m pytest tests -q` exits0 with1708passed2opt-in skips
in171.43s, after the codec eligibility correction. Earlier runs remain historical.

ESP-IDF5.5 target build exits0. Binary size0x3a3790; app partition0x3f0000,
free0x4c870(8%). Compiler reports shared_ptr atomic API deprecation and an unused
lesson helper; no warning-free claim.

- App SHA256:c35037ee8200361facfcbab00cb44a0020ce9df7b365633a4439b2d2acdc9c43.
- Partition SHA256:4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5.
- Parsed local layout:ota_0 at0x20000, ota_1 at0x410000, each4032KiB;
  assets at0x800000,8MiB. This is not a fresh device OTA/layout read.
- git diff --check exits0.

Target ELF sizeof values:Application6312, inbound container76, inbound envelope80,
connection lane544, mailbox832, LessonQueueItem616, StaticTask_t352, StackType_t1
bytes. Two existing8192-byte workers plus TCBs total17088bytes; activation adds no
task. Four owned JSON permits bound count, not payload bytes. Two full-text records
allow131070payload bytes plus owner/string overhead. Runtime heap and task-stack
headroom remain unmeasured.

## Remaining Release Gates

- Scoped Git identity for the reviewed candidate without staging unrelated work.
- Complete and independently review the clean server runtime inventory before
  assembly. Server source remains clean5319ad4694b8a873182d43680227a7341f3f228f;
  no clean image has been built or deployed by this verification.
- Matched server/firmware release tests and rollback-preserving targeted flash.
- Fresh attended motor-area safety before boot or conversation, which can trigger
  automatic gestures. Physical wake latency,250ms scheduling, repeated speech,
  silence/echo, interruptions, volume comparison, reconnect and panic/watchdog
  evidence are still required. Synthetic tests do not prove smooth audio.
