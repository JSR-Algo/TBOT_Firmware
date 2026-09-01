# Cardputer Deferred Wi-Fi Worker Design

## Goal

Remove the remaining cross-task races and all blocking Wi-Fi connection waits from the Application task while preserving exact UI generation ownership.

## Chosen Architecture

Use two process-lifetime FreeRTOS tasks: the existing scan task and a dedicated connection task. A single mixed worker was rejected because a 10-second connection wait could delay scan recovery work; creating a task per request was rejected because allocation failure and task-handle publication would remain part of every request.

Task handles are published through `std::atomic<TaskHandle_t>` with release stores and acquire loads. Tasks are never deleted, so a published non-null handle remains valid for the process lifetime. Request state remains mutex protected and notification failure leaves work pending for the next poll.

## Deferred Intent State

A host-testable `CardputerWifiDeferredIntentState` owns all deferred credential and reconnect intents under one mutex. Each intent carries a monotonically increasing revision and the originating UI generation. Publishing coalesces superseded pending work, cancellation rejects stale generations, and `TakeNotified` transfers exactly one intent to the connection worker. Credentials are copied or moved only while holding the state mutex; board fields never expose mutable strings across tasks.

Credential completion has a separate durable result phase. The connection worker performs SSID persistence, lifecycle calls, and the bounded 10-second connection wait. It then stores an immutable result in the state before scheduling an Application closure. If scheduling fails, the stored result remains deliverable without repeating the connection attempt. Reconnect intents execute `TryWifiConnect()` on the connection worker and complete exactly once.

## Data Flow

1. Keyboard/Application code publishes a credential or reconnect intent and returns immediately.
2. The short periodic Application closure creates missing process-lifetime tasks and, only when external scan/recovery ownership has cleared, notifies the connection worker.
3. The connection worker atomically takes one exact intent without holding UI or deferred-state locks during Wi-Fi calls or waits.
4. Credential results are stored durably and delivered through a short generation-checked Application closure. Reconnect has no UI result.
5. Exiting or replacing a UI generation cancels stale UI delivery without racing string ownership or invalidating a connection already executing.

## Failure Handling

- Task creation and notification failures retain pending intent.
- Notification before task-handle publication is harmless; a later acquire load observes the permanent handle and retries the wake.
- Application scheduling failure retains the immutable result and retries delivery without reconnecting.
- External scan/recovery ownership keeps connection intents pending; no lifecycle call occurs until the manager reports the boundary clear.
- No UI, deferred-intent, or worker-state mutex is held across Wi-Fi driver calls, `TryWifiConnect()`, or the 10-second wait.

## Verification

Native TSan tests model concurrent handle publication/notification, credentials and reconnect publication, stale generation cancellation, notification failure, exactly-once execution, durable result delivery, and an Application sentinel running while the connection worker is blocked. Contract tests prove the Application poll contains no connection wait. Exact Cardputer objects and the existing focused Wi-Fi suites remain required.
