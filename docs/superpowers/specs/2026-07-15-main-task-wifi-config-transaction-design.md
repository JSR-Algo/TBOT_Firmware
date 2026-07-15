# Main-Task WiFi Configuration Transaction Design

## Goal

WiFi configuration entry must synchronously settle realtime protocol and audio
on the Application task before BLUFI allocates memory. Every failure restores a
usable prior path or remains deliberately fail-closed.

## Entry Serialization

All public, timer, and delayed entry paths enqueue one Application-task request.
An atomic pending flag makes enqueue idempotent until the request starts. The
Application-task callback owns preparation, BLUFI setup, state publication, and
rollback; no caller uses `ScheduleAndWait`.

## Preparation

`Application::PrepareWifiConfigEntry()` returns a fixed-state ticket containing
the original state and reconnect intent. It rejects lessons, unsupported states,
`connect_in_flight_`, and `reset_pending_` before mutation. It then cancels stale
reconnect work, synchronously settles Speaking or Listening, closes the audio
channel without destroying `protocol_`, disables realtime audio, and checks the
transition to a stable state that can enter `WifiConfiguring`.

## Commit And Rollback

After preparation, WifiBoard reserves the binding, begins audio provisioning,
commits the generation, and conditionally initializes BLUFI. Device-state
publication is checked before station/config-flag mutation. Successful entry
keeps the existing notification timing within the committed transaction.

Failures after token binding use an exact-token abort: claim the binding,
transactionally deinitialize BLE, generation-rearm audio, then consume the
binding. Application rollback runs only after cleanup succeeds. It restores
Starting/WifiConfiguring/Activating where valid, returns Idle to Idle, and
resumes Connecting/Listening/Speaking through the normal checked Connecting
path using the preserved protocol. Cleanup failure does not resume realtime
audio and retains fail-closed ownership.

## Tests

Contracts cover main-task-only enqueueing, idempotent requests, preparation
before BLUFI initialization, checked transitions, in-flight rejection, exact
token cleanup, and rollback from Listening, Speaking, and Connecting.
