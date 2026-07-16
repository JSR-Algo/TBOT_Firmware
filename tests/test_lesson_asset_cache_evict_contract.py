from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MCP_SOURCE = ROOT / "main" / "mcp_server.cc"
HELPER_HEADER = ROOT / "main" / "lesson_asset_cache_evict.h"
HELPER_SOURCE = ROOT / "main" / "lesson_asset_cache_evict.cc"
MAIN_CMAKE = ROOT / "main" / "CMakeLists.txt"
COORDINATOR_HEADER = ROOT / "main" / "lesson_asset_storage_coordinator.h"
COORDINATOR_SOURCE = ROOT / "main" / "lesson_asset_storage_coordinator.cc"
SYNC_POLICY_HEADER = ROOT / "main" / "lesson_asset_sync_path_policy.h"
SYNC_POLICY_SOURCE = ROOT / "main" / "lesson_asset_sync_path_policy.cc"


def test_exact_eviction_helper_is_built_and_exposed_as_user_only_after_task4():
    source = MCP_SOURCE.read_text(encoding="utf-8")
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    helper = HELPER_SOURCE.read_text(encoding="utf-8")

    assert '#include "lesson_asset_cache_evict.h"' in source
    registration = source[source.index('AddUserOnlyTool("self.lesson_assets.evict_cache_key"') :]
    registration = registration[: registration.index("\n    AddUserOnlyTool(", 1)]
    assert registration.count("Property(") == 1
    assert 'Property("cacheKey", kPropertyTypeString)' in registration
    assert "EvictLessonAssetCacheKey(cache_key, false)" in registration
    assert '"lesson_asset_cache_evict.cc"' in cmake
    assert '"/sdcard/tbot/lesson-assets"' in helper


def test_sync_path_policy_reuses_the_canonical_key_parser_and_is_built():
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    header = SYNC_POLICY_HEADER.read_text(encoding="utf-8")
    source = SYNC_POLICY_SOURCE.read_text(encoding="utf-8")

    assert '"lesson_asset_sync_path_policy.cc"' in cmake
    assert '#include "lesson_asset_cache_evict.h"' in source
    assert "IsCanonicalLessonCacheKey(std::string(cache_key))" in source
    assert "kLessonAssetCacheKeyMaxBytes" in source
    assert "ValidateLessonAssetSyncPath(" in header
    assert "LessonAssetSyncDestinationsCollide(" in header
    assert "IsExactLowerLessonAssetSha256(" in header
    assert "IsAllowedLessonAssetSyncUrl(" in header
    assert "lowered.back() == '.'" in source
    assert "remove_suffix(1)" in source


def test_all_lesson_asset_mutators_are_lease_guarded():
    source = MCP_SOURCE.read_text(encoding="utf-8")

    for helper in (
        "EnsureLessonAssetParentDirs(",
        "DownloadLessonAssetToVerifiedFile(",
        "EnsureSampleLessonAssetDir(",
        "DownloadLessonAssetToFile(",
    ):
        definition = re.search(
            rf"\n(?:void|bool)\s+{re.escape(helper[:-1])}\([^;]*?\)\s*\{{",
            source,
            re.DOTALL,
        )
        assert definition is not None
        body_start = source.index("{", definition.start())
        body_end = source.index("\n}", body_start)
        body = source[body_start:body_end]
        assert "RequireLessonAssetMutationLease(mutation);" in body

    mutation_tokens = ("mkdir(", "remove(", 'fopen(tmp_path.c_str(), "wb")', "rename(")
    for token in mutation_tokens:
        for offset in _all_indexes(source, token):
            enclosing_start = max(source.rfind("\nvoid ", 0, offset), source.rfind("\nbool ", 0, offset))
            lease_guard = source.rfind("RequireLessonAssetMutationLease(mutation);", enclosing_start, offset)
            assert lease_guard != -1, f"unguarded lesson asset mutation: {token} at {offset}"


def _all_indexes(text: str, needle: str):
    start = 0
    while True:
        found = text.find(needle, start)
        if found == -1:
            return
        yield found
        start = found + len(needle)


def test_public_helper_surface_and_privacy_contract_are_stable():
    header = HELPER_HEADER.read_text(encoding="utf-8")
    source = HELPER_SOURCE.read_text(encoding="utf-8")

    for code in (
        "kEvicted",
        "kNotFound",
        "kInvalidCacheKey",
        "kLessonSessionActive",
        "kPathMismatch",
        "kNestedDirectory",
        "kUnexpectedNodeType",
        "kScanFailed",
        "kUnlinkFailed",
        "kRmdirFailed",
        "kPartialEvictRecoveryRequired",
    ):
        assert code in header
    assert "bool IsCanonicalLessonCacheKey(const std::string& value);" in header
    assert "const char* LessonAssetCacheEvictCodeName(" in header
    assert "LessonAssetCacheEvictResult EvictLessonAssetCacheKey(" in header
    assert "result.cache_key.clear()" in source
    assert "realpath(" not in source
    assert "std::filesystem" not in source
    assert "remove_all" not in source
    assert "kLessonAssetCacheSlugMaxBytes" in header
    assert "kLessonAssetCacheVersionDigitsMax" in header


def test_helper_uses_only_target_supported_fat_path_apis():
    source = HELPER_SOURCE.read_text(encoding="utf-8")

    for unsupported in (
        "lstat(",
        "openat(",
        "fstatat(",
        "fdopendir(",
        "unlinkat(",
        "st_ino",
        "st_dev",
        "S_ISLNK",
    ):
        assert unsupported not in source


def test_storage_coordinator_is_move_only_non_waiting_and_privacy_safe():
    header = COORDINATOR_HEADER.read_text(encoding="utf-8")
    source = COORDINATOR_SOURCE.read_text(encoding="utf-8")
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")

    assert '"lesson_asset_storage_coordinator.cc"' in cmake
    assert "LessonAssetMutationLease(const LessonAssetMutationLease&) = delete;" in header
    assert "operator=(const LessonAssetMutationLease&) = delete;" in header
    assert "LessonAssetMutationLease(LessonAssetMutationLease&& other) noexcept;" in header
    assert "operator=(LessonAssetMutationLease&& other) noexcept;" in header
    assert "std::lock_guard<std::mutex>" in source
    assert "condition_variable" not in header + source
    assert "sleep" not in header + source
    assert "MutationOperationLabel mutation_operation_" in header
    assert "ClassifyOperationLabel(operation)" in source
    mutation = source[
        source.index("LessonAssetStorageCoordinator::TryBeginMutation(") :
        source.index("LessonAssetSessionResult", source.index("TryBeginMutation("))
    ]
    assert mutation.index("ClassifyOperationLabel(operation)") < mutation.index(
        "std::lock_guard<std::mutex>"
    )
    assert "mutation_operation_ = operation;" not in source
    assert "std::string mutation_operation_" not in header
    assert "kLessonAssetIdentityMaxBytes" in header
    assert "kInvalidIdentity" in header
    assert "std::uint64_t generation" in header
    assert "last_generation_" in header
    assert "assignment_id_.swap(validated_assignment_id)" in source
    assert "session_id_.swap(validated_session_id)" in source


def test_eviction_acquires_coordinator_before_any_filesystem_scan():
    source = HELPER_SOURCE.read_text(encoding="utf-8")
    body = source[source.index("LessonAssetCacheEvictResult EvictLessonAssetCacheKey(") :]

    lease = body.index('TryBeginMutation("evict")')
    assert lease < body.index("InspectRequiredDirectory(root)")
    assert lease < body.index("stat(leaf_path.c_str()")
    assert "kPartialEvictRecoveryRequired" in source
    assert "deleted_count" in body
    assert "second_pass_names != validated_names" in body
    assert "TBOT_TEST_LESSON_CACHE_EVICT_FAIL_FINAL_STAT" in source
    assert "#ifndef ESP_PLATFORM" in source
    assert "defined(ESP_PLATFORM)" in source
    runner = (ROOT / "scripts" / "run_host_native_lesson_asset_cache_evict_test.sh").read_text(
        encoding="utf-8"
    )
    assert "TBOT_LESSON_ASSET_CACHE_EVICT_TESTING" in runner


def test_eviction_tool_has_exact_six_field_envelope_and_runtime_bypass():
    source = MCP_SOURCE.read_text(encoding="utf-8")
    registration = source[source.index('AddUserOnlyTool("self.lesson_assets.evict_cache_key"') :]
    registration = registration[: registration.index("\n    AddUserOnlyTool(", 1)]

    response = registration[registration.index("auto json = MakeCheckedCJsonObject();") :]
    expected_fields = ("cacheKey", "status", "evicted", "notFound", "fileCount", "reason")
    for field in expected_fields:
        assert response.count(f'"{field}"') == 1
    assert "LessonAssetCacheEvictCodeName(result.code)" in registration
    assert "result.evicted" in registration
    assert "result.not_found" in registration
    assert "result.file_count" in registration

    dispatch = source[source.index("void McpServer::DoToolCall(") :]
    assert 'tool_name == "self.lesson_assets.evict_cache_key"' in dispatch
    assert dispatch.count("is_lesson_cache_evict") >= 3
    immediate_guard = dispatch.index("Application::GetInstance().IsLessonRuntimeActive()")
    assert dispatch.rfind("is_lesson_cache_evict", 0, immediate_guard) != -1
    scheduled_guard = dispatch.index(
        "Application::GetInstance().IsLessonRuntimeActive()", immediate_guard + 1
    )
    assert dispatch.rfind("is_lesson_cache_evict", 0, scheduled_guard) != -1
