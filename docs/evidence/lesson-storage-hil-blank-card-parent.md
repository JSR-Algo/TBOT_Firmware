# Lesson Storage HIL Blank-Card Parent Candidate Evidence

## Disposition

- Candidate status: `SOFTWARE_READY_FOR_HIL_REFLASH`.
- This is not a live HIL `PASS`. No candidate image has been flashed or attested on the robot.
- `safeToReflash=false` until the attended reflash preconditions, hardware lock, artifact revalidation, device identity checks, and operator-controlled HIL procedure are satisfied.

## Live Replacement-Card Failure

- Evidence: `/Users/manhhodinh/Documents/TBOT/robot/docs/evidence/artifacts/lesson-hardware-resilience/20260718T035911Z/replacement-card-20260718T061845Z/post-replacement-recovery.json`.
- Evidence SHA-256: `736a30029c4d01732dd5680cb442aad24b4b748749eefeb03c75c723744d4688`.
- Live fixture response: `status=io_failed`, `changed=false`.
- Recovery result: `status=RECOVERY_FAILED`, `errorCode=FIXTURE_STAGE_FAILED`, `coldStarted=false`.
- Root cause: the replacement card mounted successfully but was blank and did not contain `/sdcard/tbot`. The implementation used direct, non-recursive `mkdir` through `CreateFixtureDirectory` on `/sdcard/tbot/lesson-assets`; because the `/sdcard/tbot` parent was absent, fixture staging returned `io_failed` without reporting a mutation.

## RED And GREEN Software Evidence

The RED checkpoint is commit `36d95916711fdcf2ae728324f0e374570bfd8359` (`test(firmware): reproduce blank-card HIL parent failure`). It was reproduced from an archive of that exact commit with:

```text
./scripts/run_host_native_lesson_storage_hil_fixture_test.sh
FAIL: mounted blank card did not stage preservation fixture
exit 1
```

Reviewed implementation and regression commits:

- `c4bf9bd` - `fix(firmware): stage HIL fixtures on blank mounted cards`
- `b7a1eb6` - `test(firmware): cover blank-card parent rollback truth`
- `a9da6cc` - `test(firmware): freeze blank-card HIL parent contract`

Fresh GREEN results at source commit `a9da6ccb06eea9b8d135008e6361e1d9c57d2416`:

```text
./scripts/run_host_native_lesson_storage_hil_fixture_test.sh
lesson storage HIL fixture host checks: 848
exit 0

python3 -m pytest -q tests/test_lesson_storage_hil_contract.py tests/test_lesson_storage_hil_local_config.py tests/test_lesson_storage_hil_artifact_auditor.py
74 passed in 1.25s
exit 0

python3 -m pytest -q tests/test_lesson_storage_hil_contract.py
22 passed in 0.02s
exit 0
```

Design and execution references:

- `0424728` - `docs(firmware): design blank-card HIL parent creation`
- `6cc7990` - `docs(firmware): plan blank-card HIL parent fix`

## HIL Build

Generated local overlay:

```text
python3 scripts/generate_lesson_storage_hil_local_config.py --ota-url http://192.168.100.209:8003/tbot/ota/ --websocket-url ws://192.168.100.209:8000/tbot/v1/ --output build-task14-hil-blank-parent/sdkconfig.defaults.hil-local
```

Overlay SHA-256: `f6988bb7e78a1997859d7b2e38330374e9faf371059eac98c3fd91ac3f36827d`.

The plan's initial build command completed, but ESP-IDF used the repository-root `sdkconfig`, so the fail-closed auditor rejected the candidate with `required artifact missing: sdkconfig`. The repository's earlier immutable HIL build convention requires a build-local `SDKCONFIG`. Adding `set-target esp32s3` was not retained because its implicit `fullclean` removed the generated overlay before configuration. The final mechanically corrected command preserved the exact defaults chain and profile:

```text
source /Users/manhhodinh/esp/esp-idf-v5.5.2/export.sh
idf.py -B build-task14-hil-blank-parent -D SDKCONFIG=build-task14-hil-blank-parent/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local;sdkconfig.defaults.hil-storage;build-task14-hil-blank-parent/sdkconfig.defaults.hil-local' build
```

Final build result:

```text
Project build complete.
xiaozhi.bin binary size 0x37bc50 bytes.
Smallest app partition is 0x3f0000 bytes.
0x743b0 bytes (12%) free.
exit 0
```

No `flash`, `esptool write_flash`, serial-port, robot, SD-card, or hardware-lock command was run.

## Candidate Attestation

Audit command and exact result:

```text
python3 scripts/assert_lesson_storage_hil_artifacts.py --build-dir build-task14-hil-blank-parent --profile hil
lesson storage HIL artifact audit: PASS profile=hil commit=a9da6ccb06eea9b8d135008e6361e1d9c57d2416
exit 0
```

- Manifest: `/Users/manhhodinh/Documents/TBOT/.worktrees/tbot-firmware-production-lesson-studio-continued/build-task14-hil-blank-parent/lesson-storage-hil-build.json`.
- Sidecar: `/Users/manhhodinh/Documents/TBOT/.worktrees/tbot-firmware-production-lesson-studio-continued/build-task14-hil-blank-parent/lesson-storage-hil-build.sha256`.
- Sidecar verification: `lesson-storage-hil-build.json: OK`.
- Manifest SHA-256 asserted by the sidecar: `73bbbf65c9fbf4c0d90cb37cbcc813add440c4250c43fec131856833f16ebeac`.
- Sidecar file SHA-256: `d4d807ad8f0c53e329ef09acec62bca7ed6c8800c29d8928daadc9cecf3d547d`.
- Manifest status/profile: `status=PASS`, `profile=hil`.
- Source commit: `a9da6ccb06eea9b8d135008e6361e1d9c57d2416`.
- Target/project: `esp32s3`, `xiaozhi`.
- HIL checks: `hilConfigEnabled=true`, `toolLiterals=present`, `hilSymbols=present`, `bannedApis=absent`.
- Binary: `xiaozhi.bin`, `3652688` bytes, SHA-256 `6713e2fb0ead658fc61ecb2366bbd54d1caf017a77b38682dca9bbbc5910ee1a`.
- ELF: `xiaozhi.elf`, `49813280` bytes, SHA-256 `128cff2de46bf82ed63955babaee1ff0983ebd5ac1513e463839e3322bac4ee9`.
- Map: `xiaozhi.map`, `30365043` bytes, SHA-256 `90931a0d400274b109e3d5544bf53b00e854b42b416174b0f4973230e5b054e5`.
- Main archive: `esp-idf/main/libmain.a`, `40244052` bytes, SHA-256 `c912aba4d195af3ed2ae54f4a159116d85a9462692a1f39df904d9a341fff8e3`.
- SDK config: `sdkconfig`, `126061` bytes, SHA-256 `49ce8f7fd793bd6df552c73c79f6eb1c9e3c0ae640941e1d3b0384caa98160ef`.
- Partition: `partitions/v2/16m.csv`, `4128768` bytes; image `3652688` bytes; free `476080` bytes (`11.5308%`).

Candidate identity is the immutable tuple:

```text
sourceCommit=a9da6ccb06eea9b8d135008e6361e1d9c57d2416
binSha256=6713e2fb0ead658fc61ecb2366bbd54d1caf017a77b38682dca9bbbc5910ee1a
elfSha256=128cff2de46bf82ed63955babaee1ff0983ebd5ac1513e463839e3322bac4ee9
manifestSha256=73bbbf65c9fbf4c0d90cb37cbcc813add440c4250c43fec131856833f16ebeac
profile=hil
target=esp32s3
```

## Scope And Safety

Implementation touched only these firmware sources before this evidence commit:

- `scripts/run_host_native_lesson_storage_hil_fixture_test.sh`
- `tests/native/lesson_storage_hil_fixture_host_test.cc`
- `main/lesson_storage_hil_fixture.cc`
- `tests/test_lesson_storage_hil_contract.py`

The build and manifest pair remain generated, ignored, and uncommitted under `build-task14-hil-blank-parent/`. During implementation and build, no robot filesystem, microSD contents, hardware lock, serial device, running firmware, or production image was touched. `robot/docs/TEST_MATRIX.md` and the parent goal were not edited.

## Review Gates

- The source tree was clean at `a9da6cc` before build and during the successful artifact audit.
- `git diff --check` is required to exit `0` for this evidence change.
- The credential-pattern scan covers the touched firmware sources and this evidence document and is required to return no matches.
- A live result remains pending an attended HIL reflash and recovery run; this document does not authorize or claim that hardware step.
