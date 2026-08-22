# Course Mode Task 07 Local Endpoint Firmware Design

## Purpose

Provide a temporary, checksum-pinned firmware application for the attended Task
07 lab only. The image connects the approved internal robot to private local OTA
and WebSocket endpoints without reading or mutating persisted endpoint, claim,
or backend configuration. It is not a production candidate and cannot authorize
Task 08 or Task 09.

## Selected Approach

Add a default-off Kconfig mode, `CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT`, with
literal private-LAN OTA and WebSocket URLs. In this mode the application:

- uses only the compiled OTA URL and rejects every alternate URL;
- keeps OTA-returned WebSocket URL/token in RAM for the current boot;
- never reads or writes endpoint, claim-reset, factory-test, bootstrap-token, or
  backend API state in NVS;
- rejects firmware-update, MQTT, claim-reset, and backend API data from the OTA
  response;
- skips claimed-device production config refresh, heartbeat, claim polling,
  release, and reset network paths;
- identifies itself as `course-mode-task07-local-endpoint`, never `production`;
- rejects DNS names, loopback/link-local/public IPs, credentials, query strings,
  fragments, malformed ports, control bytes, non-ASCII, and non-HTTP/non-WS
  schemes.

A plain URL overlay is rejected because `Ota::GetCheckVersionUrl()` and
`WebsocketProtocol::RefreshSettings()` prefer NVS, while activation refresh and
heartbeat paths can still contact persisted production endpoints. NVS editing
and DNS/TLS interception are rejected because they weaken restoration and
identity evidence.

## Components

### Local endpoint policy

Create a small dependency-free policy unit that validates the compiled URLs and
exposes the local-mode decision to OTA, WebSocket, application, and build
identity code. The policy accepts literal RFC1918 IPv4 or ULA IPv6 only. OTA is
plain `http://`; WebSocket is plain `ws://`. Loopback and link-local addresses
are rejected because the robot must reach the host over the lab LAN.

### OTA transient configuration

In normal builds, `Ota` retains existing behavior. In local mode,
`GetCheckVersionUrl()` returns only `CONFIG_OTA_URL`, the retry list contains
only that URL, and recovery never persists it. The response may supply exactly
a validated local WebSocket URL and token; those values are held by `Ota` and
passed to `WebsocketProtocol`. Firmware, MQTT, `api_url`, `claim_reset`, and
claim/factory-test state make the response fail closed.

### WebSocket and activation isolation

`WebsocketProtocol` accepts an optional transient URL/token. Local mode requires
that transient configuration and never calls the NVS resolver. Activation skips
claimed config fetch, heartbeat, and other backend ownership network paths. The
normal production branch remains unchanged under the default-off flag.

### Build and restoration identity

The overlay generator writes the mode flag plus exact OTA/WS URLs after strict
validation. Build identity emits the lab-only profile. The application is built
twice with ESP-IDF 5.5.4 and pinned candidate inputs; both `xiaozhi.bin` files
must match byte-for-byte and be no larger than 3,612,672 bytes.

Only the app partition at `0x20000` may be flashed. NVS at
`0x9000..0xcfff`, bootloader, partition table, OTA data, and assets remain
untouched. Restoration writes the immutable candidate app SHA-256
`84c999ece0c90eb6e69a410e335c7791f330e9c0fd39c30dfd4162bb7c4cfc6e`
at `0x20000`, then readback-compares the full app region and NVS SHA-256
`a7a87f72416be20388298cb70cfff306ec78e77f0e8b09231d16113f3d82404e`.

## Failure Handling

The build fails if local mode lacks valid endpoints. Runtime fails before any
network request if endpoint policy rejects the compiled values. Any prohibited
OTA response field fails the OTA check without NVS writes. Missing transient
WebSocket configuration prevents protocol start. Any unexpected production
hostname, endpoint persistence, identity drift, app oversize, non-reproducible
build, flash/readback mismatch, NVS mismatch, privacy uplink, or safety event
stops the physical session and restores the immutable candidate.

## Verification

- Python contract tests prove generator validation and source-level isolation.
- Host-native tests prove URL policy and build identity.
- Existing claim, config, cloud-link, and SD-sync tests prove production paths
  remain intact when the flag is off.
- Two isolated builds prove app reproducibility and size.
- Artifact metadata pins source, toolchain, sdkconfig, offsets, hashes, and exact
  restoration commands before any device access.

This design closes only local routing. It does not waive the calibrated
acoustic, power, thermal, timing, E-stop, visual, recovery, or rollback gates.
