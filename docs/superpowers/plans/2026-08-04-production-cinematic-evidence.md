# Production Cinematic Evidence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the exact production LCDWiki firmware emit bounded `CINE_EVIDENCE` boot and 19-cue terminal evidence while keeping every HIL, fault-injection, MCP, and network evidence surface disabled.

**Architecture:** Replace the HIL-named cinematic collector with one production-safe collector compiled only by `CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE`. Keep the existing single boot record, single cue record, fixed-width counters, fixed 1,024-byte stack formatting buffer, and renderer/panel hooks. Sample current internal heap and PSRAM without allocation at cue begin, every existing `Record*` boundary, and cue end; retain the minimum samples in fixed cue state while reporting the allocator lifetime internal-heap minimum separately. Rename all public C++ APIs and serial prefixes. Update the production config gate, log/evidence verifiers, artifact contracts, build manifest evidence, and host tests so release builds require evidence-on plus HIL-off.

**Tech Stack:** C++17, ESP-IDF 5.5.2, Kconfig, Python 3/Pytest, native clang++ ASan/UBSan runners, ESP-IDF production build and artifact audit.

---

### Task 1: Lock the production configuration contract

**Files:**
- Modify: `main/Kconfig.projbuild`
- Modify: `main/boards/lcdwiki-es3c35p/config.json`
- Modify: `scripts/assert_lcdwiki_prod_config.py`
- Modify: `tests/test_lcdwiki_es3c35p_board.py`

- [ ] **Step 1: Write failing config tests**

Add assertions that the LCDWiki build appends `CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y`, the production gate rejects its absence, and the gate still rejects both `CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=y` and `CONFIG_TBOT_HIL_STORAGE_FAULTS=y`.

```python
def test_lcdwiki_enables_release_cinematic_evidence():
    assert "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y" in lcdwiki_reference_sdkconfig()


def test_lcdwiki_prod_gate_rejects_missing_release_cinematic_evidence(tmp_path):
    sdkconfig = tmp_path / "sdkconfig.es3c35p-no-release-evidence"
    sdkconfig.write_text(
        lcdwiki_reference_sdkconfig().replace(
            "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y\n", ""
        ),
        encoding="utf-8",
    )
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts/assert_lcdwiki_prod_config.py"), str(sdkconfig)],
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode != 0
    assert "Release cinematic evidence must be enabled" in result.stderr
```

- [ ] **Step 2: Run the focused tests and confirm RED**

Run: `python3 -m pytest tests/test_lcdwiki_es3c35p_board.py -q`

Expected: the new release-evidence assertions fail because the symbol and board append do not exist.

- [ ] **Step 3: Add the production-only symbol and gate**

Add this Kconfig entry after the legacy HIL symbol:

```kconfig
config TBOT_RELEASE_CINEMATIC_EVIDENCE
    bool "Enable production cinematic release evidence"
    depends on IDF_TARGET_ESP32S3 && BOARD_TYPE_LCDWIKI_ES3C35P
    default n
    help
        Emits bounded serial-only CINE_EVIDENCE boot and cue_end records for
        release verification. This does not enable HIL tools or fault injection.
```

Append `"CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y"` to the LCDWiki board `sdkconfig_append`. In `assert_lcdwiki_prod_config.py`, require the exact `y` line while retaining the existing HIL rejection checks.

- [ ] **Step 4: Run the focused tests and confirm GREEN**

Run: `python3 -m pytest tests/test_lcdwiki_es3c35p_board.py -q`

Expected: all tests pass.

- [ ] **Step 5: Commit the config contract**

```bash
git add main/Kconfig.projbuild main/boards/lcdwiki-es3c35p/config.json scripts/assert_lcdwiki_prod_config.py tests/test_lcdwiki_es3c35p_board.py
git commit -m "feat(firmware): require production cinematic evidence"
```

### Task 2: Replace the HIL collector with the production evidence API

**Files:**
- Create: `main/lesson_cinematic_evidence.h`
- Create: `main/lesson_cinematic_evidence.cc`
- Create: `tests/native/lesson_cinematic_evidence_host_test.cc`
- Create: `scripts/run_host_native_lesson_cinematic_evidence_test.sh`
- Delete: `main/lesson_cinematic_hil_telemetry.h`
- Delete: `main/lesson_cinematic_hil_telemetry.cc`
- Delete: `tests/native/lesson_cinematic_hil_telemetry_host_test.cc`
- Delete: `scripts/run_host_native_lesson_cinematic_hil_telemetry_test.sh`
- Modify: `main/CMakeLists.txt`
- Modify: `scripts/run_host_native_lesson_coverage.sh`

- [ ] **Step 1: Rename the host contract and make it fail on the new API**

Port the existing exhaustive collector test to `LessonCinematicEvidence*`, `LessonCinematicCueEndReason`, and `LessonCinematicFault`. Require formatted lines to begin with `CINE_EVIDENCE `, and compile the ESP-enabled variant with only:

```bash
-DESP_PLATFORM -DCONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=1 \
  -DCONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=0
```

Also compile a disabled variant with `CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=0` and assert every runtime method is a no-op.

- [ ] **Step 2: Run the renamed runner and confirm RED**

Run: `bash scripts/run_host_native_lesson_cinematic_evidence_test.sh`

Expected: compilation fails because `lesson_cinematic_evidence.h/.cc` and the renamed API do not exist.

- [ ] **Step 3: Implement the renamed bounded collector**

Move the existing collector state and counters into the new files. Enable ESP-only heap/random/ROM calls exclusively with:

```cpp
#if defined(ESP_PLATFORM) && defined(CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE) && \
    CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE
#define TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED 1
#else
#define TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED 0
#endif
```

Expose only `LessonCinematicEvidenceEnabled`, `Boot`, `BeginCue`, the existing record methods, `FormatCueEnd`, `EmitCueEnd`, and test setters. Emit exactly:

```cpp
esp_rom_printf("CINE_EVIDENCE event=boot boot_nonce=0x%" PRIx64
               " reset_reason=%s lifetime_internal_heap_min=%u psram_heap_min=%u\n", ...);
```

and format cue terminal lines with `CINE_EVIDENCE event=cue_end`. Preserve `CueCounters`, `BootCounters`, `std::mutex`, and `char cue_id[40]`. Define a documented 1,024-byte line-capacity constant and use it for the stack formatting buffer; the maximum-width canonical-cue test must retain at least 256 bytes of margin.

Do not use `heap_caps_monitor_local_minimum_free_size_start/stop`. ESP-IDF 5.5.2 implements that monitor with a process-global snapshot array allocated by `heap_caps_malloc` and asserts if allocation fails. Instead, call `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` at `BeginCue`, every existing `Record*` boundary, and cue end, then retain the lowest samples in the fixed cue record. Refresh `lifetime_internal_heap_min` separately with `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`. Document and test that `internal_heap_min` and `psram_heap_min` are boundary-sampled minima rather than continuous local-monitor minima. Retain no `new`, `malloc`, container growth, MCP registration, HTTP, WebSocket, or dynamic allocation.

- [ ] **Step 4: Update build and coverage source lists**

Replace every source-list reference to `lesson_cinematic_hil_telemetry.cc` with `lesson_cinematic_evidence.cc`; do not retain both collectors.

- [ ] **Step 5: Run collector tests under normal, disabled, enabled, ASan and UBSan variants**

Run: `bash scripts/run_host_native_lesson_cinematic_evidence_test.sh`

Expected: every variant prints `lesson cinematic evidence tests passed` and exits `0`. Tests must prove minima update across begin/record/end samples, a sampled zero cannot be overwritten by a later recovery sample, no heap-monitor symbol or call exists, the exact boot schema is captured with a nonzero nonce, and maximum-width formatting retains at least 256 bytes in the 1,024-byte buffer.

- [ ] **Step 6: Commit the collector replacement**

```bash
git add main/lesson_cinematic_evidence.* tests/native/lesson_cinematic_evidence_host_test.cc scripts/run_host_native_lesson_cinematic_evidence_test.sh main/CMakeLists.txt scripts/run_host_native_lesson_coverage.sh
git rm main/lesson_cinematic_hil_telemetry.* tests/native/lesson_cinematic_hil_telemetry_host_test.cc scripts/run_host_native_lesson_cinematic_hil_telemetry_test.sh
git commit -m "feat(firmware): emit bounded cinematic release evidence"
```

### Task 3: Rebind boot, renderer, and panel hooks without changing playback

**Files:**
- Modify: `main/main.cc`
- Modify: `main/lesson_flattened_cinematic_renderer.cc`
- Modify: `main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc`
- Modify: `scripts/run_host_native_lesson_handler_test.sh`
- Modify: `scripts/run_host_native_lesson_flattened_cinematic_renderer_test.sh`
- Modify: `scripts/run_host_native_lesson_renderer_trace_test.sh`
- Modify: `scripts/run_host_native_lesson_cinematic_renderer_test.sh`
- Modify: `tests/native/lesson_flattened_cinematic_renderer_test.cc`

- [ ] **Step 1: Add failing static and behavioral hook assertions**

Require the three production files to include `lesson_cinematic_evidence.h`, contain only `LessonCinematicEvidence*` calls, and contain no `LessonCinematicHilTelemetry` or `HIL_CINE`. Retain the renderer tests that prove cue start/terminal emission, parser/I/O/DMA/queue fault counters, late ticks, SD read latency, panel completion, stop/cancel/replacement/discard paths, and cue replacement ordering.

- [ ] **Step 2: Run renderer suites and confirm RED**

Run:

```bash
bash scripts/run_host_native_lesson_flattened_cinematic_renderer_test.sh
bash scripts/run_host_native_lesson_renderer_trace_test.sh
bash scripts/run_host_native_lesson_cinematic_renderer_test.sh
```

Expected: compile/static failures reference the removed HIL header/API.

- [ ] **Step 3: Mechanically rebind hooks to the evidence API**

Rename includes, enum types, and calls one-for-one. Do not change renderer timing, queueing, layer compositing, cue lifecycle, SD reads, DMA behavior, or panel callbacks. `main.cc` must call `LessonCinematicEvidenceBoot()` once in the same boot location.

- [ ] **Step 4: Run all affected native runners**

Run the three commands from Step 2 plus `bash scripts/run_host_native_lesson_handler_test.sh`.

Expected: all runners pass with unchanged playback assertions.

- [ ] **Step 5: Commit the integration**

```bash
git add main/main.cc main/lesson_flattened_cinematic_renderer.cc main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc scripts/run_host_native_lesson_* tests/native/lesson_flattened_cinematic_renderer_test.cc
git commit -m "refactor(firmware): bind cinematic renderer to release evidence"
```

### Task 4: Update production log and five-pass evidence verification

**Files:**
- Rename: `scripts/lesson_cinematic_hil_log_verify.py` to `scripts/lesson_cinematic_evidence_log_verify.py`
- Rename: `scripts/lesson_cinematic_hil_evidence_verify.py` to `scripts/lesson_cinematic_release_evidence_verify.py`
- Rename: `tests/test_lesson_cinematic_hil_log_verifier.py` to `tests/test_lesson_cinematic_evidence_log_verifier.py`
- Rename: `tests/test_lesson_cinematic_hil_evidence_verifier.py` to `tests/test_lesson_cinematic_release_evidence_verifier.py`

- [ ] **Step 1: Change fixtures to `CINE_EVIDENCE` and confirm RED**

Update boot/cue fixture constructors and require the verifiers to reject legacy-only `HIL_CINE` logs. Preserve the canonical 19-cue order, one boot line, nonzero nonce, zero fault counters, per-cue internal heap `>=20480`, PSRAM `>0`, accepted reset/terminal reasons, Google Live `vi-VN`/Kore identity, gentle wrong/silence retry, five unique runs/log hashes/nonces, readback hash, and app offset `0x20000`.

- [ ] **Step 2: Run verifier tests and confirm RED**

Run:

```bash
python3 -m pytest tests/test_lesson_cinematic_evidence_log_verifier.py tests/test_lesson_cinematic_release_evidence_verifier.py -q
```

Expected: failures until both verifier scripts parse only `CINE_EVIDENCE`.

- [ ] **Step 3: Update verifier parsing and user-facing output**

Filter boot and cue lines with `line.startswith("CINE_EVIDENCE ")`. Error messages must name `CINE_EVIDENCE`; legacy `HIL_CINE` must not satisfy any count. Keep the evidence schema and all safety thresholds unchanged unless a schema rename is required, in which case reject the old schema explicitly.

- [ ] **Step 4: Run verifier tests and confirm GREEN**

Run the command from Step 2.

Expected: all positive and adversarial tests pass.

- [ ] **Step 5: Commit verifier migration**

```bash
git add scripts/lesson_cinematic_* tests/test_lesson_cinematic_*
git commit -m "test(firmware): verify production cinematic evidence"
```

### Task 5: Extend artifact audit, run full gates, and freeze the binary

**Files:**
- Modify: `scripts/assert_lcdwiki_prod_config.py`
- Modify: `scripts/assert_lesson_storage_hil_artifacts.py`
- Modify: `tests/test_lesson_storage_hil_artifact_auditor.py`
- Create: `docs/evidence/production-cinematic-evidence-release.md`

- [ ] **Step 1: Add failing artifact tests**

Require the manifest/audit to record:

```json
{
  "releaseCinematicEvidence": true,
  "hilCinematicTelemetry": false,
  "hilStorageFaults": false
}
```

Require the ELF to contain `CINE_EVIDENCE` and `LessonCinematicEvidence`, and reject `HIL_CINE`, `LessonCinematicHilTelemetry`, storage-fault banners/tools, HIL MCP registrations, banned APIs, or a non-production embedded profile.

- [ ] **Step 2: Run focused audit tests and confirm RED**

Run:

```bash
python3 -m pytest tests/test_lcdwiki_es3c35p_board.py tests/test_lesson_storage_hil_artifact_auditor.py -q
```

Expected: missing release-evidence fields/symbol checks fail.

- [ ] **Step 3: Implement the manifest and symbol assertions**

Read the resolved production `sdkconfig`, ELF strings/symbols, binary metadata, partition usage, commit SHA, and defaults chain. Fail closed unless evidence is on and both HIL options are off.

- [ ] **Step 4: Run all non-hardware gates**

Run:

```bash
python3 -m pytest -q tests
bash scripts/run_host_native_lesson_coverage.sh
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

Then execute the established LCDWiki ESP-IDF production build and artifact-audit command. Expected: all Pytests pass, all 27 native ASan/UBSan runners pass, lesson coverage remains `2825/2825` or higher with no uncovered regression, production config/audit pass, and `xiaozhi.bin` fits the app partition.

- [ ] **Step 5: Scan for forbidden remnants**

Run:

```bash
rg -n "HIL_CINE|LessonCinematicHilTelemetry|lesson_cinematic_hil_telemetry" main scripts tests
```

Expected: no production/source/verifier remnants; only historical documentation may match.

- [ ] **Step 6: Record frozen artifact evidence and commit**

Document source commit, build command, config state, binary size/SHA-256, partition free bytes/percent, ELF SHA-256, audit output, and explicit `no flash/reset performed` statement in `docs/evidence/production-cinematic-evidence-release.md`.

```bash
git add scripts tests docs/evidence/production-cinematic-evidence-release.md
git commit -m "chore(firmware): audit cinematic production evidence artifact"
```

### Task 6: Independent review and hardware release gate

**Files:**
- Modify after capture: `docs/evidence/production-cinematic-evidence-release.md`
- Create after capture: five unique serial logs, five unique server logs, and the release evidence manifest under the release artifact directory

- [ ] **Step 1: Request independent code and artifact review**

Reviewer must confirm the exact `main` commit, no duplicate collector state, no dynamic allocation/network/tool surface, release evidence enabled, both HIL configs disabled, all tests/build/audit passing, and no unrelated repository changes included.

- [ ] **Step 2: Hold hardware until external gates pass**

Do not flash until backend/server/admin exact deployments are attested, generation is `current=connected=1` with `retrying=failed=0` twice at least ten seconds apart, Farm v8 exists in the accepted materialized generation, exactly one fresh assignment is active, and `farm_v8_publish_validator.py` reports `882 PASS / 0 FAIL`.

- [ ] **Step 3: App-only flash and readback**

Flash only the frozen `xiaozhi.bin` at `0x20000`, preserving NVS/Wi-Fi/SD. Read back the exact app byte length and require its SHA-256 to equal the frozen binary SHA-256.

- [ ] **Step 4: Capture five consecutive full journeys**

Each distinct boot must produce one nonzero unique nonce and exactly the canonical 19 ordered `CINE_EVIDENCE event=cue_end` lines, Google Live `vi-VN`/Kore evidence, gentle wrong and silence coaching, barn-to-hay progression, internal heap `>=20480`, PSRAM `>0`, and no WDT/Guru/OOM/reset/fallback/degraded marker.

- [ ] **Step 5: Verify and close release**

Run `python3 scripts/lesson_cinematic_release_evidence_verify.py <manifest.json>`. Expected: `verified 5 consecutive cinematic release passes`. Append hashes and verifier output to the evidence document; production-ready may be claimed only after this command and the full E2E gate both pass.
