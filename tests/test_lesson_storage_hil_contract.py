from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_hil_storage_config_is_default_off_and_board_restricted():
    kconfig = read("main/Kconfig.projbuild")
    start = kconfig.index("config TBOT_HIL_STORAGE_FAULTS")
    block = kconfig[start:]
    next_config = block.find("\nconfig ", 1)
    if next_config != -1:
        block = block[:next_config]

    assert "default n" in block
    assert "depends on IDF_TARGET_ESP32S3 && BOARD_TYPE_LCDWIKI_ES3C35P" in block


def test_hil_profile_is_separate_from_production_defaults():
    assert read("sdkconfig.defaults.hil-storage") == "CONFIG_TBOT_HIL_STORAGE_FAULTS=y\n"
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS=y" not in read("sdkconfig.defaults")
    assert "sdkconfig.defaults.hil-storage" not in read("CMakeLists.txt")


def test_hil_sources_are_conditionally_compiled():
    cmake = read("main/CMakeLists.txt")
    conditional = cmake[cmake.index("if(CONFIG_TBOT_HIL_STORAGE_FAULTS)") :]
    conditional = conditional[: conditional.index("endif()")]

    assert '"lesson_storage_hil_controller.cc"' in conditional
    assert '"lesson_storage_hil_hooks.cc"' in conditional
    assert '"lesson_storage_hil_fixture.cc"' in conditional
    assert '"lesson_storage_hil_mcp_tools.cc"' in conditional


def test_controller_uses_static_esp_critical_section_without_std_mutex():
    header = read("main/lesson_storage_hil_controller.h")
    source = read("main/lesson_storage_hil_controller.cc")
    mutex_class = header[header.index("class Mutex") : header.index("class LockGuard")]

    assert '#ifdef ESP_PLATFORM\n#include "freertos/FreeRTOS.h"' in header
    assert "portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;" in mutex_class
    assert "portENTER_CRITICAL(&mutex_)" in mutex_class
    assert "portEXIT_CRITICAL(&mutex_)" in mutex_class
    for branch in mutex_class.split("#ifdef ESP_PLATFORM")[1:]:
        assert "std::mutex" not in branch.split("#else", 1)[0]
    assert "std::lock_guard<std::mutex>" not in source
    assert "LockGuard lock(mutex_);" in source


def test_eviction_hil_hook_surface_is_noexcept_and_fixed_outcome():
    header = read("main/lesson_storage_hil_hooks.h")

    assert "enum class LessonStorageHilHookOutcome" in header
    assert "kContinue" in header
    assert "kFail" in header
    assert "kNoSpace" in header
    assert "LessonStorageHilHookOutcome RunLessonStorageHilCheckpoint(" in header
    assert ") noexcept;" in header
    assert "SetLessonStorageHilPauseCallbackForTest" in header


def test_eviction_checkpoints_are_guarded_and_ordered_in_destructive_phase():
    source = read("main/lesson_asset_cache_evict.cc")
    body = source[source.index("int deleted_count = 0;") :]
    first_unlink_checkpoint = body.index("kBeforeFirstUnlink")
    first_unlink = body.index("unlink(child_path.c_str())")
    increment = body.index("++deleted_count")
    after_unlinks = body.index("kAfterUnlinks")
    before_rmdir = body.index("kBeforeRmdir")
    rmdir = body.index("rmdir(leaf_path.c_str())")

    assert first_unlink_checkpoint < first_unlink < increment < after_unlinks
    assert after_unlinks < before_rmdir < rmdir
    assert "TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING" in body
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS" in body

    destructive = body[first_unlink_checkpoint:rmdir].replace(
        "throw std::bad_alloc();", ""
    )
    for banned in (
        "std::string(",
        "std::vector",
        "push_back(",
        "reserve(",
        "MakeResult(",
        "cJSON",
        "throw ",
    ):
        assert banned not in destructive


def test_hil_pause_uses_yielding_delay_and_stable_markers():
    source = read("main/lesson_storage_hil_hooks.cc")

    assert "HIL_STORAGE_CHECKPOINT_REACHED" in source
    assert "HIL_STORAGE_CHECKPOINT_CONTINUED" in source
    assert "kPauseSliceSeconds" in source
    assert "while (remaining_seconds > 0)" in source
    assert "vTaskDelay(pdMS_TO_TICKS(slice_seconds * 1000U))" in source
    assert "esp_task_wdt_reset()" in source
    assert "sleep(" not in source
    corrupt_start = source.index("case LessonStorageHilAction::kCorruptStaging:")
    corrupt = source[corrupt_start : source.index("\n    }", corrupt_start)]
    assert "return LessonStorageHilHookOutcome::kFail;" in corrupt
    for forbidden_field in ("sequence=", "action=", "pause_seconds="):
        assert forbidden_field not in source


def test_sync_staging_corruption_is_typed_bounded_and_fail_closed_elsewhere():
    header = read("main/lesson_storage_hil_hooks.h")
    source = read("main/lesson_storage_hil_hooks.cc")
    staging = read("main/lesson_asset_download_staging.cc")

    assert "RunLessonStorageHilStagingCheckpoint(" in header
    assert "const char* staging_path" in header
    assert "CorruptLessonStorageHilStagingFile(" in source
    assert "std::fread(&byte, 1, 1, file)" in source
    assert "byte ^= 0x01U" in source
    assert "std::fwrite(&byte, 1, 1, file)" in source
    assert "std::fflush(file)" in source
    assert "fsync(descriptor)" in source
    assert "checkpoint != LessonStorageHilCheckpoint::kBeforeChecksumVerify" in source
    execute = source[
        source.index("LessonStorageHilHookOutcome ExecuteDecision(") :
        source.index("bool CorruptLessonStorageHilStagingFile(")
    ]
    assert "case LessonStorageHilAction::kCorruptStaging:" in execute
    assert "return LessonStorageHilHookOutcome::kFail;" in execute
    assert "RunLessonStorageHilStagingCheckpoint(" in staging


def test_sync_commit_checkpoints_are_guarded_and_use_existing_restore_branch():
    source = read("main/lesson_asset_download_staging.cc")
    checksum = source.index("kBeforeChecksumVerify")
    verify = source.index("VerifyLessonAssetSha256(staging.path()")
    backup = source.index("std::rename(destination.c_str(), backup.c_str())")
    commit_checkpoint = source.index("kBeforeCommitRename", backup)
    replacement = source.index("std::rename(staging.path().c_str(), destination.c_str())")
    restore = source.index("std::rename(backup.c_str(), destination.c_str())", replacement)

    assert checksum < verify < backup < commit_checkpoint < replacement < restore
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS" in source
    assert "TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING" in source
    assert "replace_failed = true" in source[commit_checkpoint:replacement]


def test_fixture_surface_has_fixed_root_and_checked_mutation_lease():
    header = read("main/lesson_storage_hil_fixture.h")
    source = read("main/lesson_storage_hil_fixture.cc")

    for token in (
        "enum class LessonStorageHilFixture",
        "kNestedDirectory",
        "kLeafRegularFile",
        "kPreservationSet",
        "enum class LessonStorageHilFixtureCode",
        "LessonStorageHilFixtureResult",
        "LessonStorageHilInspectionEntry",
        "LessonStorageHilInspection",
        "StageLessonStorageHilFixture(",
        "CleanupLessonStorageHilFixture(",
        "InspectLessonStorageHilStorage(",
    ):
        assert token in header

    assert '#define TBOT_LESSON_STORAGE_HIL_ROOT "/sdcard/tbot/lesson-assets"' in source
    assert "if (!mutation)" in source
    assert "TryBeginMutation(" not in source
    assert "IsCanonicalLessonCacheKey(cache_key)" in source
    assert 'slug.compare(0, 4, "hil-")' in source


def test_fixture_mutation_is_exact_nonrecursive_and_uses_fixed_sentinels():
    source = read("main/lesson_storage_hil_fixture.cc")

    assert '".tbot-hil-nested"' in source
    assert '".tbot-hil-sentinel"' in source
    assert "TBOT-HIL-LEAF-FIXTURE-V1" in source
    assert "TBOT-HIL-PRESERVATION-PRIMARY-V1" in source
    assert "TBOT-HIL-PRESERVATION-SIBLING-V1" in source
    for banned in (
        "remove_all",
        "recursive_directory_iterator",
        "nftw(",
        "FTW_",
        "system(",
        '"rm ',
        "glob(",
    ):
        assert banned not in source


def test_fixture_inspection_is_bounded_read_only_and_uses_mbedtls_sha256():
    source = read("main/lesson_storage_hil_fixture.cc")

    assert "kInspectionDirectChildCap" in source
    assert "mbedtls_sha256_starts" in source
    assert "mbedtls_sha256_update" in source
    assert "mbedtls_sha256_finish" in source
    assert '"lesson-assets/current.json"' in source
    assert '"lesson-assets/pvg"' in source
    assert '"lesson-assets/shared"' in source
    assert '"/lesson-assets' not in source
    assert "kInspectionRawNameMaxBytes" in source
    assert "kInspectionLabelMaxBytes" in source
    assert "EncodeLabelComponent(" in source
    assert 'constexpr char kHex[] = "0123456789ABCDEF"' in source
    assert "TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING" in source
    assert "SetLessonStorageHilFixtureUnlinkCallbackForTest" in source
    assert "SetLessonStorageHilFixtureRmdirCallbackForTest" in source
    assert "std::sort(inspection.entries.begin(), inspection.entries.end()" in source
    inspect = source[
        source.index("LessonStorageHilInspection InspectLessonStorageHilStorage(") :
    ]
    for forbidden in ("mkdir(", "unlink(", "rmdir(", "WriteExactFile("):
        assert forbidden not in inspect
