# Offline Wake-Word Rearm Design

## Goal

Keep the local "Hi ESP" wake word available whenever a claimed robot is safely idle, even when the passive production WebSocket is slow, disconnected, or retrying.

## Current Failure

Startup prewarms the WakeNet model but delays `EnableWakeWordDetection(true)` until the passive WebSocket worker succeeds. A TLS timeout therefore leaves the robot in `Idle` with `wake_running=0`, the input codec disabled, and no local path that can hear "Hi ESP". The background reconnect loop continues, but the user cannot initiate a foreground connection by voice.

## Behavior

After activation reaches a safe claimed, non-lesson `Idle` state, the application enables wake-word detection independently of passive WebSocket success. Passive preconnect remains an optimization for lesson pull and reduced first-turn latency; it is no longer a prerequisite for local wake detection.

When WakeNet detects "Hi ESP" while the passive socket is unavailable or connecting, the existing wake invocation path records the deferred wake intent, supersedes or joins the passive attempt using the existing generation guards, and opens the foreground audio channel. Successful passive connection continues to consume a deferred wake before falling back to ordinary wake-word rearm.

Wake-word detection remains disabled when microphone ownership belongs to provisioning, lesson playback/listening, an active voice processor, quiet asset synchronization, reset, or another existing unsafe state. This change does not weaken those ownership gates and does not send microphone audio while merely idle.

## Failure Handling

Passive TLS failure, watchdog timeout, and reconnect backoff must leave or restore local wake-word detection when the robot is safely idle. Foreground wake connection failure returns to `Idle`, rearms WakeNet, and keeps the existing bounded reconnect behavior. Duplicate callbacks remain guarded by connection generation and audio-service idempotence.

## Verification

Add a failing source-contract test proving that claimed activation rearms wake-word detection before passive WebSocket success. Extend passive WebSocket failure and watchdog tests to require wake-word availability in safe `Idle`, while retaining tests that prohibit rearm during lessons and quiet synchronization. Run the focused Python contracts, the broader firmware test suite, and an ESP-IDF production build. Flash without writing NVS, then verify on hardware that serial reports `wake_running=1` before any WebSocket session, "Hi ESP" transitions `Idle` to `Connecting`/`Listening`, and a backend response reaches `Speaking`.
