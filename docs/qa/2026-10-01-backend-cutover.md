# Backend endpoint cutover - 2026-10-01

Status: PARTIAL physical-device acceptance; configuration cutover verified.

Requested scope: firmware, ESP server, mobile use https://backend.tjbot.vn/v1. ESP remains on 160.187.240.56. Database/account/device migration was not requested; the new backend uses a fresh database.

Changes:
- Firmware provisioning fallback and LCDWiki build overlay use the new backend. OTA/WSS stay on esp.tjbot.vn. Existing NVS wifi/provisioning_url overrides must be inspected on the physical unit; no NVS erasure or fleet OTA was performed.
- ESP live container uses the unchanged image, new backend and lesson URLs, and matching JWT public key/device mint secret. Persisted /opt/tbot/.env and a final Compose override. Original configuration and rollback command are in /opt/tbot/backend-cutover-20261001. No ESP storage migration.
- Mobile local runtime, hosted fallback, production/staging-device build profiles use the new endpoint. Protocol, token storage and COPPA policy remain unchanged. Existing installed binaries require a new build/reload. Development COPPA bypass remains restricted to its pre-existing old host.
- Deployment templates updated for future builds. Running admin-web and staging containers were not redeployed.

Verification:
- Mobile: npm run typecheck, npm run lint; npm test -- --runInBand (249 suites / 3404 tests); npm run test:integration -- --runInBand (3 suites / 6 tests); targeted config tests (12).
- Mobile validators: flows:validate, sequences:fast (103 files), erd:validate, usecases:check (157 cases), check:token-parity (7 files), check:route-coverage (127 routes), check:screen-prop-types all passed. Flow single-writer provenance is warning-only and is not claimed as ownership proof.
- Firmware: python3 -m pytest tests/test_tbot_cloudflare_links.py tests/test_tbot_connect_config.py tests/test_tbot_claim_confirmation_contract.py -q (28 passed); idf.py build with existing ESP-IDF/Python 3.9 environment passed; python3 scripts/assert_lcdwiki_prod_config.py sdkconfig passed.
- ESP: pytest test_config_from_api_template.py, test_lesson_assignment_console.py, test_ota_websocket_url.py, test_config_loader_lesson_posture.py (45 passed); test_scaleout_deploy_topology.py (38 passed).
- Live: ESP effective server.api_url and lesson.api_base match new backend; health 200. Actual httpx client: absent mint key 401, matching key and empty body 400 MAC_REQUIRED (auth succeeded, no device created). Public OTA returns new api_url and unchanged WSS. Firmware User-Agent bootstrap 200. Container remains running with zero restarts.

Limits: no physical robot/phone E2E; new backend CA registration configuration remains missing. Cloudflare blocks Python urllib default UA with 403/1010, whereas actual ESP httpx and firmware UA probes passed. No weakened protection or auth bypass was introduced. Full mobile UI E2E requires an appropriate test account/device setup. Preserve pre-existing source edits; evidence hashes include the current dirty candidate.

Critique: configuration source and persisted live override changed together; native/unit tests cover URL resolution and bootstrap boundaries, not robot behavior. New database and device CA configuration remain material acceptance gaps. Reproduction commands and logs are under task-artifacts/backend-vps-20261001/cutover in the workspace.

Backend-to-ESP authenticated probe also passed: POST lesson-nudge for an all-zero test UUID returned 202 device-offline, confirming the new mint secret works in both directions without nudging a real device.

Native mobile build: E2E_IOS_API_URL=https://backend.tjbot.vn E2E_IOS_AI_URL=https://backend.tjbot.vn/api/ai npm run detox:build:ios passed (BUILD SUCCEEDED). Generated Release-iphonesimulator/TJBOT.app/main.jsbundle contains backend.tjbot.vn. This simulator/test-harness build is not an App Store or physical phone release.

Detox iOS focused cold-start check passed (1 passed; login test excluded by testNamePattern, reported as 1 skipped). Command: E2E_IOS_API_URL=https://backend.tjbot.vn E2E_IOS_AI_URL=https://backend.tjbot.vn/api/ai E2E_LOCAL_API_URL=https://backend.tjbot.vn E2E_AUTH_MODE=existing npm run detox:test:ios -- e2e/smoke.test.ts --testNamePattern="cold-starts on the Login screen". No test account seeded. This is not full auth/robot E2E evidence. Regenerated local runtime env afterwards to restore normal app flags.
