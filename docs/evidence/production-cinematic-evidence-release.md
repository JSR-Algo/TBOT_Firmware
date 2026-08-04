# Production Cinematic Evidence Release

This record freezes the software and hardware evidence required before the
LCDWiki firmware can be called production-ready. Values marked `PENDING` must
be replaced from one exact, clean production build; stale artifacts do not
satisfy this record.

## Contract

- Build profile: `production`
- Target: `esp32s3`
- Project: `xiaozhi`
- Embedded profile: `TBOT_EMBEDDED_PROFILE=production`
- Release evidence: `CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y`
- Immutable LCDWiki defaults chain: release evidence is enabled in
  `sdkconfig.defaults.local`; the shared defaults remain unchanged, and Kconfig
  restricts the option to LCDWiki ESP32-S3 builds.
- Legacy cinematic HIL: disabled
- Storage-fault HIL: disabled
- Required artifact evidence: `CINE_EVIDENCE` literal and
  `LessonCinematicEvidence` symbols
- Forbidden artifact evidence: `HIL_CINE`,
  `LessonCinematicHilTelemetry`, storage HIL tools/banner/symbols, banned file
  APIs, and non-production embedded profiles

## Software-Only Evidence

- Implementation base: `b30f875`
- RED command:
  `python3 -m pytest -q tests/test_lesson_storage_hil_artifact_auditor.py`
- RED result: 4 expected failures proving the old auditor did not require the
  release flag, release literal/symbol, or reject legacy cinematic leakage.
- GREEN command:
  `python3 -m pytest -q tests/test_lesson_storage_hil_artifact_auditor.py tests/test_lcdwiki_es3c35p_board.py`
- Initial GREEN result: `61 passed in 0.79s`
- Review-hardening GREEN result: `79 passed in 0.56s`. This includes exact
  embedded-profile tokens, canonical/unique critical sdkconfig booleans,
  linked-ELF defined-symbol evidence, boot/cue marker boundaries, and the
  immutable direct defaults chain.
- Python syntax check:
  `python3 -m py_compile scripts/assert_lesson_storage_hil_artifacts.py scripts/assert_lcdwiki_prod_config.py`
- Current checked-in `sdkconfig` gate: expected failure because it predates the
  approved release-evidence config. The immutable direct defaults chain is now
  fixed; the exact LCDWiki production build must regenerate resolved
  `sdkconfig` and pass the gate before artifact audit.
- No full ESP-IDF build was performed for this task.
- No flash or reset was performed.

## Exact Build Freeze

- Source commit: `PENDING`
- Source tree clean: `PENDING`
- Build command: `PENDING`
- Config-default chain and SHA-256 values: `PENDING`
- Resolved sdkconfig SHA-256: `PENDING`
- Binary path / bytes / SHA-256: `PENDING`
- ELF path / bytes / SHA-256: `PENDING`
- Map path / bytes / SHA-256: `PENDING`
- Main archive path / bytes / SHA-256: `PENDING`
- Partition table path / SHA-256: `PENDING`
- App partition bytes / free bytes / free percent: `PENDING`
- Artifact-audit manifest path / SHA-256: `PENDING`
- Artifact-audit output: `PENDING`

## Remaining Non-Hardware Gates

```sh
python3 -m pytest -q tests
bash scripts/run_host_native_lesson_coverage.sh
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
# Run the established clean LCDWiki ESP-IDF production build.
python3 scripts/assert_lesson_storage_hil_artifacts.py \
  --profile production \
  --build-dir BUILD_DIRECTORY
rg -n "HIL_CINE|LessonCinematicHilTelemetry|lesson_cinematic_hil_telemetry" \
  main scripts tests
```

The artifact audit must emit a coherent manifest/checksum pair containing the
source commit, defaults hashes, artifact hashes, partition metrics, and these
exact checks:

```json
{
  "releaseCinematicEvidence": true,
  "hilCinematicTelemetry": false,
  "hilStorageFaults": false
}
```

## Remaining Hardware Gates

- App-only flash at offset `0x20000`, preserving NVS, Wi-Fi, and SD data.
- Read back the exact application bytes and match the frozen binary SHA-256.
- Capture five distinct consecutive runs with unique nonzero boot nonces.
- Verify exactly 19 ordered `CINE_EVIDENCE event=cue_end` records per run.
- Verify Google Live `vi-VN` / Kore, gentle wrong and silence coaching, and
  barn-to-hay progression.
- Reject WDT, Guru Meditation, OOM, unexpected reset, fallback, or degraded
  operation; require internal heap at least 20,480 bytes and PSRAM above zero
  for every cue.

Production-ready status remains blocked until every `PENDING` field and every
hardware gate above has exact passing evidence.
