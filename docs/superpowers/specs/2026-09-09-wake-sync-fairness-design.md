# Wake Word And Asset Sync Fairness

Operator approved conversation-first scheduling on2026-09-09. Existing trial
server6cddfc0f560e and firmwarec35037ee are installed, not production accepted.
Serial evidence shows27quiet intervals between24176ms and93926ms. All26 gaps
are shorter than the1500ms wake rearm delay; no detected wake in that capture.

Preserve single-flight SD/network quiet and its teardown guarantees. Do not run
wake processing concurrently with storage quiet or lower the existing1500ms
settling delay. After quiet ends, defer subsequent background sync until wake
detection is actually running and has a3000ms uninterrupted opportunity. Busy
admission uses the existing MCP busy response; rejection must not stop the rearm
timer, alter audio, or extend the opportunity deadline on every retry.

Conversation connecting/listening/speaking takes precedence over background asset
sync. Do not infer that voice-silent listening means no active conversation.
Legacy passive paths, lessons, explicit motion, Neweye, agent-only API key and
playback/echo policies otherwise retain existing behavior. No new worker task.

Tests must execute real admission/rearm code with fake clock/audio boundaries,
not only match source strings. Cover repeated requests at observed gaps, wake
not yet running, admitted sync after opportunity, failure cleanup, and active
conversation refusal. Existing lifecycle/gesture tests, full suite and target
build precede flash. No guarantee of acoustic success before real robot tests.

After reviewed flash: Hi ESP,30-second response+15-second silence, explicit
left/right arm commands, then simultaneous speech/gesture under fresh attended
clearance. UART acknowledgement alone does not prove physical movement.
