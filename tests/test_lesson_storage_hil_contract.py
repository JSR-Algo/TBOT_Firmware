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
    assert "HIL_STORAGE_FAULT_CONSUMED" in source
    assert "HIL_STORAGE_CHECKPOINT_CONTINUED" in source
    assert "kPauseSliceSeconds" in source
    assert "while (remaining_seconds > 0)" in source
    assert "vTaskDelay(pdMS_TO_TICKS(slice_seconds * 1000U))" in source
    assert "esp_task_wdt_reset()" in source
    assert "sleep(" not in source
    execute_start = source.index("LessonStorageHilHookOutcome ExecuteDecision(")
    corrupt_start = source.index(
        "case LessonStorageHilAction::kCorruptStaging:", execute_start
    )
    corrupt = source[corrupt_start : source.index("\n    }", corrupt_start)]
    assert "return LessonStorageHilHookOutcome::kFail;" in corrupt
    assert "reached_sequence=" in source
    assert "consumed_sequence=" in source
    assert "action=" in source
    assert "pause_seconds=" not in source


def test_hil_evidence_sequences_avoid_nano_printf_uint64_formatting():
    sources = {
        path: read(path)
        for path in (
            "main/lesson_storage_hil_hooks.cc",
            "main/lesson_storage_hil_mcp_tools.cc",
            "main/mcp_server.cc",
        )
    }

    for source in sources.values():
        assert "PRIu64" not in source
    for marker in (
        "HIL_STORAGE_ARM",
        "HIL_STORAGE_FIXTURE",
        "HIL_STORAGE_CHECKPOINT_REACHED",
        "HIL_STORAGE_FAULT_CONSUMED",
        "HIL_STORAGE_REFUSAL",
    ):
        matching = [source for source in sources.values() if marker in source]
        assert matching, marker
        assert all("lesson_storage_hil_u64_format.h" in source for source in matching)
    assert sources["main/lesson_storage_hil_hooks.cc"].count("reached_sequence=%s") == 1
    assert sources["main/lesson_storage_hil_hooks.cc"].count("consumed_sequence=%s") == 1
    assert sources["main/lesson_storage_hil_mcp_tools.cc"].count("sequence=%s") >= 3
    assert sources["main/mcp_server.cc"].count("sequence=%s") == 2


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
        "lstat(",
        "openat(",
        "fstatat(",
        "fdopendir(",
        "unlinkat(",
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
    assert "kInspectionPerFileBytes" in source
    assert "kInspectionAggregateBytes" in source
    assert "kDirectoryReadCallCap" in source
    assert "esp_task_wdt_reset()" in source
    assert "taskYIELD()" in source
    assert "EncodeLabelComponent(" in source
    assert 'constexpr char kHex[] = "0123456789ABCDEF"' in source
    assert "TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING" in source
    assert "SetLessonStorageHilFixtureMkdirCallbackForTest" in source
    assert "SetLessonStorageHilFixtureFsyncCallbackForTest" in source
    assert "SetLessonStorageHilFixtureWriteCallbackForTest" in source
    assert "SetLessonStorageHilFixtureOpenCallbackForTest" in source
    assert "SetLessonStorageHilFixtureReadCallbackForTest" in source
    assert "SetLessonStorageHilFixtureDirectoryReadCallbackForTest" in source
    assert "SetLessonStorageHilFixtureInspectFailureCallbackForTest" in source
    assert "SetLessonStorageHilFixtureUnlinkCallbackForTest" in source
    assert "SetLessonStorageHilFixtureRmdirCallbackForTest" in source
    assert "std::sort(inspection.entries.begin(), inspection.entries.end()" in source
    inspect = source[
        source.index("LessonStorageHilInspection InspectLessonStorageHilStorage(") :
    ]
    for forbidden in ("mkdir(", "unlink(", "rmdir(", "WriteExactFile("):
        assert forbidden not in inspect


def test_hil_mcp_registers_exact_user_only_tools_once_under_guard():
    source = read("main/mcp_server.cc")
    tools = read("main/lesson_storage_hil_mcp_tools.cc")
    names = (
        "self.lesson_assets.hil.arm_fault",
        "self.lesson_assets.hil.status",
        "self.lesson_assets.hil.stage_fixture",
        "self.lesson_assets.hil.cleanup_fixture",
        "self.lesson_assets.hil.inspect",
    )

    registration = source[source.index("void McpServer::AddUserOnlyTools()") :]
    guarded_call = registration[registration.index("#if CONFIG_TBOT_HIL_STORAGE_FAULTS") :]
    guarded_call = guarded_call[: guarded_call.index("#endif")]
    assert guarded_call.count("RegisterLessonStorageHilMcpTools(*this)") == 1
    assert "lesson_storage_hil_mcp_tools.h" in source
    for name in names:
        assert tools.count(f'"{name}"') == 1
    for constant in ("kArmTool", "kStatusTool", "kStageTool", "kCleanupTool", "kInspectTool"):
        assert tools.count(f"AddUserOnlyTool({constant}") == 1
    assert "byteThreshold" not in tools


def test_hil_mcp_raw_validation_precedes_property_conversion_and_mutation():
    server = read("main/mcp_server.cc")
    tools = read("main/lesson_storage_hil_mcp_tools.cc")
    dispatch = server[server.index("void McpServer::DoToolCall(") :]

    validation = dispatch.index("ValidateLessonStorageHilRawArguments(")
    conversion = dispatch.index("PropertyList arguments")
    assert validation < conversion
    assert "IsExactLessonStorageHilToolName(tool_name)" in dispatch[:conversion]
    assert "cJSON_ArrayForEach" in tools
    assert "duplicate" in tools.lower()
    assert "unknown" in tools.lower()
    assert "valuedouble == static_cast<double>(value->valueint)" in tools
    assert "TryBeginMutation(" in tools
    assert tools.index("ValidateFixtureRequest") < tools.index("TryBeginMutation(")
    assert tools.index("ValidateArmRequest") < tools.index(".Arm(")
    assert '"lesson storage HIL operation failed"' in server
    assert "if (is_lesson_storage_hil)" in dispatch


def test_hil_mcp_inspect_rejects_invalid_sibling_pairs_before_filesystem_access():
    tools = read("main/lesson_storage_hil_mcp_tools.cc")

    validator = tools[
        tools.index("bool ValidateFixtureRequest(") :
        tools.index("const char* OperationName(")
    ]
    inspection = validator[
        validator.index("if (inspection)") :
        validator.index("std::string fixture_text")
    ]
    assert "ValidateSiblingPair(cache_key, sibling)" in inspection
    assert "sibling == cache_key" in tools
    assert "cache_key.substr(0, primary_slash)" in tools


def test_hil_mcp_schemas_and_stable_response_fields_are_complete():
    tools = read("main/lesson_storage_hil_mcp_tools.cc")
    for required in (
        'Property("cacheKey", kPropertyTypeString)',
        'Property("operation", kPropertyTypeString)',
        'Property("checkpoint", kPropertyTypeString)',
        'Property("action", kPropertyTypeString)',
        'Property("threshold", kPropertyTypeInteger, 0, 0, 524288)',
        'Property("declaredAssetBytes", kPropertyTypeInteger, 0, 0, 524288)',
        'Property("pauseSeconds", kPropertyTypeInteger, 0, 0, 60)',
        'Property("fixture", kPropertyTypeString)',
        'Property("siblingCacheKey", kPropertyTypeString, std::string())',
    ):
        assert required in tools

    for field in (
        '"cacheKey"', '"status"', '"operation"', '"checkpoint"',
        '"action"', '"threshold"', '"declaredAssetBytes"',
        '"pauseSeconds"', '"armSequence"', '"armed"', '"reached"',
        '"consumed"', '"reachedSequence"', '"consumedSequence"',
        '"siblingCacheKey"', '"fixture"', '"changed"', '"truncated"',
        '"entries"', '"label"', '"nodeType"', '"bytes"', '"sha256"',
    ):
        assert field in tools


def test_hil_mutating_mcp_responses_are_prepared_before_side_effects():
    header = read("main/mcp_server.h")
    tools = read("main/lesson_storage_hil_mcp_tools.cc")

    assert "class PreparedMcpTextResult" in header
    assert "cJSON_PrintPreallocated" in header
    assert "PreparedMcpCall" in header
    assert "return prepared_callback_(properties);" in header
    for function, mutation in (
        ("CallArmFault", ".Arm("),
        ("CallFixtureMutation", "StageLessonStorageHilFixture("),
        ("CallFixtureMutation", "CleanupLessonStorageHilFixture("),
    ):
        body = tools[tools.index(function) :]
        body = body[: body.index("\n}")]
        assert body.index("PreparedMcpTextResult") < body.index(mutation)
        assert body.index("SealLessonStorageHilResponse") < body.index(mutation)
    assert "kInspectionPayloadBytes = 49152" in tools
    assert "kInspectionResultBytes = 65536" in tools


def test_hil_status_carries_truthful_fixed_cache_key_without_allocation():
    header = read("main/lesson_storage_hil_controller.h")
    source = read("main/lesson_storage_hil_controller.cc")

    status = header[header.index("struct LessonStorageHilStatus") :]
    status = status[: status.index("};")]
    assert "std::array<char, kLessonAssetCacheKeyMaxBytes + 1> cache_key" in status
    status_impl = source[source.index("LessonStorageHilStatus LessonStorageHilController::Status") :]
    assert "cache_key_" in status_impl[: status_impl.index("void LessonStorageHilController::Reset")]
    clear = source[source.index("void LessonStorageHilController::ClearStatus") :]
    assert "cache_key_.fill('\\0')" in clear


def test_hil_build_identity_is_guarded_and_emitted_before_board_singleton():
    app = read("main/application.cc")
    board = read("main/boards/common/board.cc")
    initialize = app[app.index("void Application::Initialize()") :]
    initialize = initialize[: initialize.index("void Application::Run()")]

    assert initialize.index("TBOT_HIL_STORAGE_FAULTS_ENABLED non-production-image") < initialize.index("Board::GetInstance()")
    warning = initialize[initialize.index("#if CONFIG_TBOT_HIL_STORAGE_FAULTS") :]
    assert "TBOT_HIL_STORAGE_FAULTS_ENABLED non-production-image" in warning[: warning.index("#endif")]
    capability = board[board.index("std::string Board::GetSystemInfoJson()") :]
    assert '#if CONFIG_TBOT_HIL_STORAGE_FAULTS' in capability
    assert 'R"("lessonStorageHilFaults":true,)"' in capability


def test_hil_artifact_auditor_has_fail_closed_profiles_and_atomic_outputs():
    auditor = read("scripts/assert_lesson_storage_hil_artifacts.py")

    for token in (
        'choices=("production", "hil")',
        '"xiaozhi.bin"', '"xiaozhi.elf"', '"xiaozhi.map"',
        'libmain.a', '"project_description.json"', '"sdkconfig"',
        'lesson-storage-hil-build.json', 'lesson-storage-hil-build.sha256',
        'CONFIG_TBOT_HIL_STORAGE_FAULTS', 'sdkconfig.defaults.hil-storage',
        'git rev-parse HEAD', 'git status --porcelain', 'nm',
        'partition', 'sha256', 'os.replace',
    ):
        assert token in auditor
    assert "production-lesson-studio" not in auditor
    assert 'description.get("c_compiler"' not in auditor
    publish = auditor[auditor.index("def publish_manifest_pair(") :]
    publish = publish[: publish.index("def audit(")]
    assert publish.index("atomic_write(sha_path") < publish.index(
        "atomic_write(manifest_path"
    )
    assert publish.index("atomic_write(manifest_path") < publish.index(
        "post_publish_verify()"
    )
    assert publish.index("post_publish_verify()") < publish.index(
        "verify_published_manifest_pair(manifest_path, sha_path)"
    )
    for banned in ("lstat", "openat", "fstatat", "fdopendir", "unlinkat"):
        assert banned in auditor
