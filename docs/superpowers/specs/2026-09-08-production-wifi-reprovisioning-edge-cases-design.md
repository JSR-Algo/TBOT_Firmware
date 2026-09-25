# Production Wi-Fi Reprovisioning Edge-Case Design

## Goal

Make changing an owned robot's Wi-Fi a recoverable cross-system transaction
across Android, the backend command channel, BluFi, firmware Wi-Fi lifecycle,
the access point, and the final cloud heartbeat. The user must be able to move
the robot to a new location or choose another network without pressing BOOT,
factory-resetting, losing ownership, or leaving either device in an indefinite
loading state.

This design extends the existing automatic offline BLE recovery, scan ownership,
transactional credential rollback, and fresh-online proof. It does not replace
those mechanisms.

## Scope

The change covers three cooperating systems:

- Android owns user intent, permissions, BLE discovery, credentials entry,
  progress presentation, cancellation, and process-death recovery.
- The backend authorizes the operation, assigns an attempt identity, remotely
  requests setup when the robot is online, records progress, and provides a
  fresh final online proof.
- Firmware serializes radio lifecycle changes, advertises BluFi when needed,
  scans networks, tests replacement credentials, commits them transactionally,
  and reports bounded diagnostic state.

Router configuration, captive-portal login automation, enterprise 802.1X, and
changing the existing BluFi UUIDs are out of scope. Unsupported networks must
fail clearly without damaging the robot's existing state.

## Existing Foundations

The implementation must preserve these existing contracts:

- An owned robot continuously offline for 60 seconds enters credential-only
  BluFi recovery without erasing ownership or saved Wi-Fi.
- An explicit `wifi_setup` command may enter setup immediately when the robot is
  online.
- Only one physical Wi-Fi scan lease is active; stale scan completions are
  drained or rejected by session and driver-incarnation ownership.
- Replacement credentials are not authoritative until association succeeds.
- A failed association restores BluFi automatically so the phone can retry.
- Successful reprovisioning uses lightweight claimed-device activation, opens
  the passive WebSocket, and resumes heartbeat and audio workers.
- Mobile success requires fresh connectivity evidence rather than cached online
  state.

## Approaches Considered

### Independent retry hardening

Each component could add more local retries and longer timeouts. This is small,
but the components can still disagree about which operation is active. A late
BLE callback, backend response, or heartbeat can incorrectly complete a newer
user action. This approach is rejected as the primary architecture.

### BLE-first reprovisioning

The phone could always wait for the robot's automatic BLE fallback and avoid the
backend command channel. This handles offline robots well but adds unnecessary
latency for online robots and does not provide a durable cross-system audit or
remote setup acknowledgement. BLE remains the offline fallback, not the sole
coordinator.

### Attempt-scoped cross-system state machine

Every user action receives a unique attempt identity. All asynchronous work is
accepted only when it belongs to the current attempt or its local generation.
The backend accelerates online setup, while direct BLE discovery remains usable
when the backend or old Wi-Fi is unavailable. This is the selected design.

## Attempt Identity And Ownership

Each **Change Wi-Fi** action creates a cryptographically random
`provisioning_attempt_id`. This is a Wi-Fi-reprovision attempt, distinct from a
new-device claim attempt.

The backend stores:

- attempt ID;
- device ID and owning household ID;
- initiating account ID;
- monotonic server creation/update timestamps;
- current state and terminal reason;
- requested setup command generation;
- last robot progress generation;
- fresh-online proof timestamps and reported SSID fingerprint;
- expiry time.

Only one non-terminal Wi-Fi attempt may exist per device. Repeating a request
with the same idempotency key returns that attempt. Starting a genuinely new
attempt supersedes the old attempt and makes every late old update inert.

The Wi-Fi password never enters backend persistence. The SSID may be displayed
to the owner, but logs and metrics use a one-way bounded fingerprint rather than
the raw name.

## Canonical State Machine

The canonical progress states are:

1. `requested`
2. `robot_setup_requested`
3. `ble_discovering`
4. `ble_connected`
5. `wifi_scanning`
6. `credentials_sent`
7. `associating`
8. `online_confirmed`
9. `completed`

Terminal unsuccessful states are:

- `cancelled`
- `credential_rejected`
- `network_unreachable`
- `bluetooth_failed`
- `permission_blocked`
- `unsupported_network`
- `superseded`
- `timed_out`

The backend state is authoritative for authorization and remote command audit.
Android is authoritative for local BLE progress and UX. Firmware is
authoritative for radio and association progress. A component may report only
forward transitions for the active attempt. It must ignore duplicate updates
and reject backward or terminal-to-nonterminal transitions.

Firmware does not need the attempt ID inside every existing BluFi frame. It may
bind the BLE connection epoch and provisioning generation to the attempt using
an authenticated metadata exchange added without changing service or
characteristic UUIDs. Until all deployed app and firmware versions support that
metadata, the existing generation and connection-epoch guards remain the local
authority and the backend accepts only final proof from the expected device.

## End-To-End Flow

### Robot currently online

1. Android creates or resumes an attempt through the authenticated backend.
2. The backend verifies current ownership and sends `wifi_setup` with the
   attempt ID through the live device command channel.
3. Firmware acknowledges command admission before station teardown, then enters
   `wifi_configuring` and starts BluFi advertising.
4. Android scans for the expected owned robot, connects, requests the Wi-Fi
   list, and submits credentials directly over encrypted BluFi.
5. Firmware quiesces BLE, associates to the candidate network, and commits the
   candidate only after obtaining an IP.
6. Firmware resumes claimed runtime and reports connectivity.
7. The backend records a heartbeat newer than the attempt start and returns the
   matching online proof to Android.
8. Android marks the attempt complete and returns to the online device screen.

### Robot offline or backend unavailable

1. Android creates a local durable attempt even if the server request cannot
   complete.
2. Android scans for the expected robot for a window covering the firmware's
   60-second offline fallback plus scheduling margin.
3. Firmware retains one fixed offline deadline. Repeated disconnects or station
   start failures cannot postpone it.
4. At expiry, firmware enters BluFi while preserving claim and credentials.
5. Android continues at the BLE stage. Backend synchronization is retried after
   the phone or robot regains Internet.

### Candidate Wi-Fi fails

1. Firmware classifies association failure without logging credentials.
2. Candidate credentials remain uncommitted.
3. Firmware tears down the failed station attempt and restores BluFi
   advertising under the same active setup transaction.
4. Android shows the narrowest actionable reason and lets the user edit the
   password, choose another AP, rescan, or cancel.
5. The cloud attempt remains non-terminal while the local retry budget and
   attempt expiry permit another credential submission.

## Deadlines And Retry Policy

Each stage has an independent deadline so one slow boundary does not consume all
recovery time:

- backend setup admission: short bounded request plus idempotent retry;
- offline BLE appearance: at least 80 seconds, covering the 60-second firmware
  fallback and scheduling variance;
- BLE connection and security negotiation: bounded per connection epoch;
- Wi-Fi scan: bounded by the firmware scan watchdog plus delivery margin;
- association and DHCP: bounded independently;
- fresh backend online proof: bounded after the robot obtains IP.

Retry uses capped exponential backoff with jitter for transient backend, BLE,
and Internet failures. Credential rejection is not blindly retried. Manual
retry reuses the current attempt until it expires or is superseded. Retry
budgets are reset only by meaningful forward progress, never by duplicate
callbacks.

## Cancellation And Resume

Cancellation is idempotent and attempt-scoped:

- Android marks its local attempt cancelled and stops scan/connect work.
- The backend records cancellation only if the attempt is still active.
- Firmware invalidates the corresponding provisioning generation if it has
  admitted the attempt.
- Callbacks and timers from the cancelled attempt cannot change current state.

If no candidate credentials were committed, cancellation restores the safest
available runtime:

- reconnect to the preserved old network when reachable; otherwise
- remain in owned BluFi recovery so the user is not locked out at a new
  location.

Android persists only non-secret attempt metadata. On app relaunch it queries
the backend, checks current BLE work, and either resumes the correct screen or
shows the terminal reason. Passwords are never persisted in AsyncStorage,
MMKV, logs, analytics, navigation parameters, screenshots, or crash reports.

## Fresh Success Proof

An attempt reaches `completed` only when all of these are true:

- the proof belongs to the expected device and active attempt;
- firmware reported association success after credentials were sent;
- a valid IP was obtained;
- the active SSID fingerprint matches the selected candidate;
- a heartbeat or authenticated WebSocket session is newer than the attempt;
- ownership and heartbeat credentials are still valid.

Cached device status, a heartbeat from before the attempt, BLE disconnection,
or merely receiving an IP is insufficient. A network with no Internet remains
`network_unreachable` or degraded and must not be presented as fully online.

## Concurrency And Ordering Rules

- One device has at most one active Wi-Fi attempt.
- One Android process has at most one active BLE connection for that attempt.
- Firmware has at most one config transaction and one physical Wi-Fi scan.
- New explicit user intent supersedes conditional offline recovery safely.
- Timer callbacks carry a generation and revalidate live state before acting.
- BLE callbacks carry connection epoch and setup generation.
- Backend command acknowledgements and progress updates carry attempt ID and
  monotonic generation.
- A late `Connected`, `Disconnected`, scan completion, setup acknowledgement,
  timeout, heartbeat, or app navigation callback cannot complete or roll back a
  newer attempt.
- Duplicate taps, HTTP retries, WebSocket delivery retries, and repeated BLE
  frames are idempotent.

## Edge-Case Matrix

### Android lifecycle and permissions

- Bluetooth, nearby-device, location, notification, or Wi-Fi permission is
  denied, later granted, or revoked while scanning.
- Bluetooth or phone Wi-Fi is toggled during discovery, connection, credential
  submission, or online proof.
- The app backgrounds, the screen locks, Android kills the process, the app
  crashes, or the user relaunches during every state.
- The user presses Back, cancels, double-taps, navigates away, rotates the
  device, or starts the flow from two app surfaces.
- The phone loses Internet while BLE remains available, or changes its own
  network while provisioning the robot.

Expected behavior: no duplicate attempt, no infinite spinner, resumable state,
specific permission guidance, and no secret persistence.

### BLE discovery and transport

- No advertisement, delayed advertisement, duplicate results, weak RSSI, scan
  API busy, BLE adapter reset, or transient GATT error.
- Multiple nearby robots advertise simultaneously.
- A different or unowned robot has a stronger signal.
- BLE disconnect occurs during security negotiation, Wi-Fi scan, credential
  transfer, or after transfer before acknowledgement.
- Callbacks arrive duplicated, reordered, after disconnect, or after a new
  connection epoch.

Expected behavior: select only the expected owned serial/device identity,
bounded retry, stale callback rejection, and actionable recovery.

### SSID and authentication

- Empty, hidden, duplicated, Unicode, whitespace-bearing, special-character,
  and maximum-length SSIDs.
- Open, WPA2, WPA3 transition, mixed security, and unsupported enterprise or
  5-GHz-only networks.
- Empty, too short, maximum-length, special-character, and incorrect passwords.
- The selected AP changes channel, BSSID, security mode, or disappears after
  scan.

Expected behavior: preserve exact valid user input, classify unsupported or
rejected credentials, never log secrets, and return to scan/edit without
corrupting the previous tuple.

### Association and Internet

- Authentication timeout, association rejection, DHCP delay/failure, duplicate
  IP, gateway unavailable, DNS failure, TLS failure, captive portal, backend
  outage, weak signal, router reboot, and roaming across BSSIDs sharing an SSID.
- The old AP reappears during conditional recovery.
- Candidate association succeeds after its timeout or after cancellation.

Expected behavior: distinguish local Wi-Fi from Internet reachability, accept
only active-attempt success, and choose old-network recovery versus BluFi using
current state rather than stale events.

### Firmware lifecycle and resource pressure

- Robot boot, reboot, brownout, or USB/power interruption during every state.
- Station stop/start, BluFi init/deinit, timer creation, scan start/cleanup,
  driver recovery, or DHCP start fails.
- A station scan, config scan, scan recovery debt, or lifecycle transition is
  already active when setup is requested.
- Heap is low, allocation fails, event queues are delayed, or callbacks race on
  different ESP tasks.
- Setup is repeated many times and a hard timeout overlaps late success.

Expected behavior: transactional admission, fail-fast initialization where
recovery is impossible, bounded retry where it is possible, no deadlock, no
leaked task/timer/BLE handle/socket, and no NVS erase.

### Backend and multi-client behavior

- Robot presence is stale, WebSocket ownership moved between gateway instances,
  command delivery is duplicated, or HTTP times out after the robot received
  the command.
- Backend or gateway restarts during an attempt.
- Two phones for the same household start or retry concurrently.
- Ownership changes, auth expires, or the user loses authorization during an
  attempt.
- Heartbeat is rate-limited, delayed, duplicated, or received from an old
  network session.

Expected behavior: owner authorization on every mutation, one active attempt,
idempotent command delivery, durable state, terminal authorization failure, and
fresh proof not confused with stale presence.

## Error Taxonomy And User Experience

Internal errors map to stable non-secret reason codes. Android renders a
specific recovery action:

- permission blocked: open the relevant permission flow;
- Bluetooth unavailable: enable Bluetooth and resume;
- robot not found: keep scanning through the offline fallback window or verify
  the selected robot identity;
- credential rejected: edit the password;
- AP disappeared: rescan or choose another network;
- unsupported network: choose a supported 2.4-GHz personal network;
- local Wi-Fi connected without Internet: choose another network or retry when
  Internet is available;
- backend proof delayed: keep the robot connection intact and refresh status;
- superseded/cancelled: exit cleanly without showing failure for the new
  attempt.

No error copy instructs the user to press BOOT for ordinary Wi-Fi recovery.
No terminal state lacks a retry, edit, rescan, cancel, or safe-exit action.

## Observability And Privacy

Every component records attempt-scoped structured events containing only:

- attempt ID or a truncated non-secret correlation ID;
- device ID in the existing approved diagnostic form;
- state transition and reason code;
- elapsed stage time, retry count, RSSI bucket, and firmware generation;
- boolean proof flags and aggregate resource measurements.

Logs, analytics, backend rows, crash reports, and test artifacts must never
contain Wi-Fi passwords, bootstrap tokens, heartbeat tokens, raw authorization
headers, or decrypted BluFi payloads. Raw SSIDs are excluded from logs and
metrics. Debug UI and accessibility dumps must redact password fields.

Metrics include completion rate, failure by stage/reason, p50/p95/p99 duration,
automatic-fallback usage, retry counts, stale-event rejection counts, resource
high-water marks, and attempts requiring manual support.

## Verification Strategy

### Automated tests

- Native and property/model tests enumerate legal state transitions, duplicate
  messages, arbitrary callback ordering, cancellation, supersession, and timer
  races.
- Firmware fault injection covers Wi-Fi/BLE lifecycle errors, scan ownership,
  driver recovery, association outcomes, DHCP timing, low-memory allocation,
  reboot recovery, and resource cleanup.
- Backend unit and integration tests cover authorization, one-active-attempt
  constraints, idempotency, stale presence, gateway restart, command timeout,
  progress ordering, cancellation, and fresh-online proof.
- Android unit/integration tests cover persisted non-secret state, process
  relaunch, permissions, scan result identity selection, BLE epoch guards,
  stage-specific timeout UX, double taps, cancellation, and backend-offline BLE
  fallback.
- Cross-system contract tests lock reason codes, state names, attempt identity,
  monotonic generation, privacy redaction, and compatibility behavior.

### Physical E2E

The real Android phone and robot must exercise at least:

1. Online robot changing to a valid new network.
2. Robot moved away from its saved network and discovered after automatic BLE
   fallback without BOOT.
3. Incorrect password followed by correction under the same attempt.
4. AP disappearance during association followed by rescan and another AP.
5. Bluetooth off/on during discovery and reconnect.
6. App background, force-stop, relaunch, and resume.
7. Robot power interruption during setup and safe recovery after boot.
8. Backend unavailable while direct BLE provisioning succeeds, followed by
   later cloud reconciliation.
9. Two nearby robots, proving the app selects only the expected owned robot.
10. Cancel during scan and during association, followed by a fresh successful
    attempt.

### Soak and release gate

Run at least 30 automated or hardware-assisted reprovision cycles mixing valid
credentials, invalid credentials, cancel/retry, radio toggles, app relaunch,
router loss, and robot reboot. Record internal heap, minimum heap, task count,
timer ownership, BLE handles, scan leases, sockets, and heartbeat recovery.

Release requires:

- all firmware, Android, backend, contract, and integration suites green;
- an ESP32-S3 production build and NVS-preserving application flash;
- no unbounded or unreachable state in model tests;
- no stale event completing or rolling back another attempt;
- no loss of ownership or previous credentials on failed attempts;
- no credential/token leakage in source, logs, artifacts, or analytics;
- no monotonic resource loss across the soak run;
- repeated physical completion with fresh WebSocket and heartbeat evidence;
- documented residual limitations for captive portals, enterprise Wi-Fi, and
  unsupported radio bands.

Passing tests demonstrates the covered contracts, not a mathematical guarantee
that Wi-Fi can never fail. Production readiness means every supported failure
is bounded, observable, recoverable, privacy-safe, and leaves the robot in a
known state.

## Rollout And Compatibility

Roll out in this order:

1. Backend accepts and stores attempt-scoped state while remaining compatible
   with old mobile and firmware clients.
2. Firmware accepts optional attempt metadata and reports progress, retaining
   all existing local generation guards when metadata is absent.
3. Android enables durable attempt orchestration only after backend capability
   detection and continues the existing BLE fallback for older firmware.
4. Observe completion/failure distributions before making the new coordinator
   mandatory.

Rollback must not require firmware downgrade or NVS erase. Disabling the new
backend/mobile orchestration returns clients to the already-shipped explicit
`wifi_setup` plus automatic offline BluFi behavior.
