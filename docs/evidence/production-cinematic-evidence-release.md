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

- Source commit: `de193c171f5c13fd85a80ec6a238928896d141eb`
- Source tree clean: `true` (`git status --porcelain` empty at HEAD)
- Build command: `idf.py -B build-release-de193c1 build` (target `esp32s3`, profile `production`)
- Config-default chain and SHA-256 values:
  - `sdkconfig.defaults`: `85d07725a6650acd127e39fe2bca86794f13bd248cec67aca16db2643ab8d385`
  - `sdkconfig.defaults.esp32s3`: `9f40206ae31a7a4aa2bc34623f3a7b9d354eb4beb81fd4aadbf1f80aac9882c9`
  - `sdkconfig.defaults.local`: `ae75957a103d25aa7a1e010a69360216ed143657458b9964aa196a15b8727059`
- Resolved sdkconfig SHA-256: `958f7b7d6c06a431b21f9da54d9ef3ed13a27330898c13358ec0421d84697d9e`
- Binary path / bytes / SHA-256: `build-release-de193c1/xiaozhi.bin` / `3753360` / `8b35da93f90db7dc892b4ef3a2b2f3679597b46ffb7c9be1334a59804dc27f26`
- ELF path / bytes / SHA-256: `build-release-de193c1/xiaozhi.elf` / `51339184` / `0b1b80a3bfdb7a06278c3962af256a868c654243ec9b50ac49e322f1f8641039`
- Map path / bytes / SHA-256: `build-release-de193c1/xiaozhi.map` / `29911235` / `bbc92cc5be43624eb6713ee8305591430b1e1d373dac37f2cd5e1bba584fd074`
- Main archive path / bytes / SHA-256: `build-release-de193c1/esp-idf/main/libmain.a` / `44115678` / `ab5a4ed7756e5e62e975d24f70882012c44d8ccfae92cb1b1ce35cf9298d173c`
- Partition table path / SHA-256: `build-release-de193c1/partition_table/partition-table.bin` / `4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5`
- App partition bytes / free bytes / free percent: `4128768` / `375408` / `9.092494`
- Artifact-audit manifest path / SHA-256: `build-release-de193c1/lesson-storage-hil-build.json` / `a89a6807371983848f0f5253e7aa03a00e5a5e9afb5a7e6eeb9768b6b953cbfc`
- Artifact-audit output: `lesson storage HIL artifact audit: PASS profile=production commit=de193c171f5c13fd85a80ec6a238928896d141eb`

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
