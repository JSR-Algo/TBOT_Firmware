from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "mcp_server.cc"
ATTESTATION_SOURCE = ROOT / "main" / "lesson_asset_sync_attestation.cc"
MAIN_CMAKE = ROOT / "main" / "CMakeLists.txt"


def function_body(text: str, signature: str, offset: int = 0) -> str:
    start = text.index(signature, offset)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def sync_body() -> str:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
    end = source.index('\n\n    AddUserOnlyTool("self.assets.set_download_url"', start)
    return source[start:end]


def test_ready_attestation_echoes_exact_verified_manifest_checksum():
    body = sync_body()
    attestation = ATTESTATION_SOURCE.read_text(encoding="utf-8")

    assert 'JsonStringField(pack.get(), "manifestChecksum")' in body
    assert "AddLessonAssetSyncAttestation(" in body
    assert '"lesson_asset_sync_attestation.cc"' in MAIN_CMAKE.read_text(encoding="utf-8")
    assert "IsSha256Hex(manifest_checksum_value)" in attestation
    assert "manifest_checksum_value == manifest_checksum" in attestation
    assert "cache_key_value.find(manifest_checksum_value)" in attestation
    assert 'CheckedCJsonAddStringToObject(response, "manifestChecksum", manifest_checksum)' in attestation

def test_generic_sync_normalizes_alias_pairs_and_rejects_mismatches():
    source = SOURCE.read_text(encoding="utf-8")
    body = sync_body()
    validation = source[
        source.index("ValidateLessonAssetSyncPackOrThrow(") :
        source.index("void DownloadLessonAssetToVerifiedFile(")
    ]

    assert 'const char* local_path = NormalizedAliasOrThrow(asset, "sdPath", "localPath");' in validation
    assert 'const char* url = NormalizedAliasOrThrow(asset, "onlineUrl", "url");' in validation
    assert 'throw std::runtime_error("lesson asset sync request invalid")' in validation
    assert '"sdPath"' in validation
    assert '"onlineUrl"' in validation
    assert "asset.destination" in body
    assert '"localPath", asset.destination' not in body

def test_generic_sync_aliases_must_be_strings_when_present():
    source = SOURCE.read_text(encoding="utf-8")
    validation = source[
        source.index("NormalizedAliasOrThrow(") :
        source.index("bool IsOptionalNonNegativeInteger(")
    ]

    assert "cJSON_GetObjectItem(object, preferred_field)" in validation
    assert "cJSON_GetObjectItem(object, fallback_field)" in validation
    assert "!cJSON_IsString(preferred)" in validation
    assert "!cJSON_IsString(fallback)" in validation


def test_generic_sync_accepts_only_valid_renderer_v4_cue_metadata():
    source = SOURCE.read_text(encoding="utf-8")
    validation = source[
        source.index("ValidateLessonAssetSyncPackOrThrow(") :
        source.index("void DownloadLessonAssetToVerifiedFile(")
    ]

    for field in ("cueId", "effect", "stepKey", "playbackMode"):
        assert f'"{field}"' in validation
    assert "IsOptionalRendererV4AssetMetadata(asset)" in validation
    assert "IsOptionalRendererV4CueMetadata(asset)" in source
    assert "IsOptionalSafeCueToken" in source
    assert "IsOptionalLessonPlaybackMode" in source
    assert 'std::strcmp(value->valuestring, "once") == 0' in source
    assert 'std::strcmp(value->valuestring, "loop") == 0' in source


def test_generic_sync_strictly_validates_renderer_v4_derivative_attestation():
    source = SOURCE.read_text(encoding="utf-8")
    validation = source[
        source.index("ValidateLessonAssetSyncPackOrThrow(") :
        source.index("void DownloadLessonAssetToVerifiedFile(")
    ]

    for field in ("derivativeId", "phaseId", "compatibilityMetadata"):
        assert f'"{field}"' in validation
    assert "IsOptionalRendererV4AssetMetadata(asset)" in validation
    assert "IsExactRendererV4CompatibilityMetadata" in source
    assert "HasOnlyAllowedJsonFields(metadata, kCompatibilityFields)" in source
    assert 'std::strcmp(codec->valuestring, "mjpeg") == 0' in source
    assert "cJSON_IsFalse(has_audio)" in source
    assert "IsBoundedPositiveIntegerField(metadata, \"frameCount\", 900)" in source

def test_generic_sync_counts_critical_failures_and_activates_only_verified_critical_pack():
    source = SOURCE.read_text(encoding="utf-8")
    body = sync_body()
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")

    assert '#include "lesson_asset_pack_activation.h"' in source
    assert '"lesson_asset_pack_activation.cc"' in cmake
    assert "bool critical;" in source
    assert "critical_failed += 1" in body
    assert "asset.critical" in body
    assert "const bool all_critical_verified = critical_failed == 0" in body
    assert "ActivateLessonAssetPack(" in body
    activation = body.index("ActivateLessonAssetPack(")
    counts = body.index('CheckedCJsonAddNumberToObject(json.get(), "criticalFailedCount", critical_failed)')
    assert activation < counts
    assert 'CheckedCJsonAddBoolToObject(json.get(), "activated", activation.activated)' in body
    assert '"previousEvicted", activation.previous_evicted' in body
    assert '"errorCode", activation.error_code.c_str()' in body

def test_generic_sync_optional_failure_activates_but_attestation_ready_requires_all_assets():
    source = SOURCE.read_text(encoding="utf-8")
    body = sync_body()

    assert "cJSON_IsTrue(critical) != 0" in source
    assert "ActivateLessonAssetPack(\n                    mutation," in body
    activation = body.index("ActivateLessonAssetPack(\n                    mutation,")
    lease_start = body.index('TryBeginMutation("sync")')
    lease_end = body.index("EvictPreviousLessonAssetPackAfterActivation(")
    assert lease_start < activation < lease_end
    assert "AddLessonAssetSyncAttestation(\n                json.get(), cache_key, manifest_checksum, asset_count,\n                verified, failed);" in body
    assert "all_critical_verified ? asset_count : verified" not in body


def test_missing_whitespace_or_cache_key_mismatch_cannot_claim_ready_or_checksum():
    attestation = ATTESTATION_SOURCE.read_text(encoding="utf-8")

    assert "manifest_checksum_valid" in attestation
    assert "cache_key_matches_manifest" in attestation
    assert "pack_verified" in attestation
    assert 'CheckedCJsonAddBoolToObject(response, "ready", pack_verified)' in attestation
    checksum_add = attestation.index(
        'CheckedCJsonAddStringToObject(response, "manifestChecksum", manifest_checksum)'
    )
    verified_guard = attestation.rindex("if (pack_verified)", 0, checksum_add)
    assert verified_guard < checksum_add


def test_failed_or_empty_asset_pack_cannot_claim_ready_checksum():
    body = sync_body()
    attestation = ATTESTATION_SOURCE.read_text(encoding="utf-8")

    assert "asset_count > 0" in attestation
    assert "verified_count == asset_count" in attestation
    assert "failed_count == 0" in attestation
    assert "pack_verified" in attestation
    assert "asset_count" in body
    assert "critical_failed" in body


def test_generic_sync_validates_the_whole_pack_before_mutation_or_filesystem_access():
    body = sync_body()

    validate = body.index("ValidateLessonAssetSyncPackOrThrow(")
    lease = body.index('TryBeginMutation("sync")')
    first_read = body.index("VerifyLessonAssetSha256(")
    first_write = body.index("DownloadLessonAssetToVerifiedFile(")
    assert validate < lease < first_read < first_write
    source = SOURCE.read_text(encoding="utf-8")
    validate_body = source[
        source.index("ValidateLessonAssetSyncPackOrThrow(") :
        source.index("void DownloadLessonAssetToVerifiedFile(")
    ]
    assert "LessonAssetSyncDestinationsCollide(" in validate_body
    assert 'throw std::runtime_error("lesson asset sync request invalid")' in validate_body
    assert "kLessonAssetSyncMaxAssets = 64" in source
    assert "IsExactLowerLessonAssetSha256(manifest_checksum)" in validate_body
    assert "CacheKeyMatchesManifestChecksum(" in validate_body
    assert "IsExactLowerLessonAssetSha256(sha256)" in validate_body
    assert "IsAllowedLessonAssetSyncUrl(url)" in validate_body
    assert "ValidateLessonAssetSyncPath(cache_key, local_path, key)" in validate_body
    assert "HasOnlyAllowedJsonFields(pack," in validate_body
    assert "HasOnlyAllowedJsonFields(asset," in validate_body


def test_backup_asset_namespace_is_reserved_before_pack_mutation():
    source = SOURCE.read_text(encoding="utf-8")
    policy = (ROOT / "main" / "lesson_asset_sync_path_policy.cc").read_text(
        encoding="utf-8"
    )
    body = sync_body()

    assert 'EndsWith(lowered, ".backup")' in policy
    assert "ValidateLessonAssetSyncPackOrThrow(" in body
    assert body.index("ValidateLessonAssetSyncPackOrThrow(") < body.index(
        'TryBeginMutation("sync")'
    )
    assert "LessonAssetSyncDestinationsCollide(" in source


def test_prelease_validation_keeps_cjson_fields_borrowed_and_bounded():
    source = SOURCE.read_text(encoding="utf-8")
    struct_body = source[
        source.index("struct ValidatedLessonAsset") :
        source.index("};", source.index("struct ValidatedLessonAsset"))
    ]
    assert "const char* key;" in struct_body
    assert "const char* path;" in struct_body
    assert "const char* url;" in struct_body
    assert "const char* sha256;" in struct_body
    assert "const char* destination;" in struct_body
    assert "bool has_declared_size;" in struct_body
    assert "size_t declared_size;" in struct_body
    assert "std::string" not in struct_body
    assert "kLessonAssetSyncMaxAssets = 64" in source
    policy_header = (ROOT / "main" / "lesson_asset_sync_path_policy.h").read_text(
        encoding="utf-8"
    )
    assert "kLessonAssetSyncUrlMaxBytes" in policy_header
    assert "kLessonAssetSyncKeyMaxBytes" in source
    assert "kLessonAssetSyncMetadataPathMaxBytes" in source
    assert "kLessonAssetSyncSha256HexBytes" in source
    assert "kLessonAssetCacheChecksumHexBytes" not in source


def test_every_invalid_pack_branch_precedes_lease_filesystem_and_network_calls():
    body = sync_body()
    validation = body.index("ValidateLessonAssetSyncPackOrThrow(")
    lease = body.index('TryBeginMutation("sync")')
    filesystem_read = body.index("VerifyLessonAssetSha256(")
    network_write = body.index("DownloadLessonAssetToVerifiedFile(")
    assert validation < lease < filesystem_read < network_write
    assert body.count("ValidateLessonAssetSyncPackOrThrow(") == 1


def test_both_sync_tools_hold_nonwaiting_mutation_lease_before_any_writer_call():
    source = SOURCE.read_text(encoding="utf-8")
    sample_start = source.index('AddUserOnlyTool("self.lesson_assets.sync_sample_to_sd"')
    generic_start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"', sample_start)
    sample = source[sample_start:generic_start]
    generic = sync_body()

    for body, first_writer in (
        (sample, "EnsureSampleLessonAssetDir(mutation)"),
        (generic, "DownloadLessonAssetToVerifiedFile("),
    ):
        lease = body.index('TryBeginMutation("sync")')
        refusal = body.index("ThrowLessonAssetMutationRefusal(mutation.code())")
        writer = body.index(first_writer, refusal)
        assert lease < refusal < writer
        assert "auto mutation =" in body


def test_writer_helpers_require_a_live_mutation_lease_and_keep_errors_private():
    source = SOURCE.read_text(encoding="utf-8")

    assert source.count("RequireLessonAssetMutationLease(mutation);") >= 4
    assert "const LessonAssetMutationLease& mutation" in source
    assert 'throw std::runtime_error("lesson asset storage busy")' in source
    assert 'throw std::runtime_error("lesson asset session active")' in source
    assert 'throw std::runtime_error("lesson asset mutation lease required")' in source
    for public_error in (
        "lesson asset storage busy",
        "lesson asset session active",
        "lesson asset mutation lease required",
        "lesson asset sync request invalid",
    ):
        assert "/sdcard" not in public_error
        assert "sync" not in public_error or public_error == "lesson asset sync request invalid"


def test_sync_response_is_checked_and_staging_cleanup_is_scope_bound():
    source = SOURCE.read_text(encoding="utf-8")
    body = sync_body()
    checked = (ROOT / "main" / "checked_cjson.h").read_text(encoding="utf-8")
    staging_header = (
        ROOT / "main" / "lesson_asset_download_staging.h"
    ).read_text(encoding="utf-8")
    staging_source = (
        ROOT / "main" / "lesson_asset_download_staging.cc"
    ).read_text(encoding="utf-8")
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")

    assert '"lesson_asset_download_staging.cc"' in cmake
    assert "LessonAssetDownloadStagingFile staging(dest_path);" in source
    assert "DownloadLessonAssetToFile(" in source
    assert "CommitVerifiedLessonAssetDownload(staging, cache_key, dest_path, sha256);" in source
    assert "staging.Disarm();" in staging_source
    assert "~LessonAssetDownloadStagingFile" in staging_header
    assert "TBOT_LESSON_ASSET_STAGING_TESTING" in staging_header + staging_source
    assert "!defined(ESP_PLATFORM)" in staging_header + staging_source
    assert "MakeCheckedCJsonObject()" in body
    assert "CheckedCJsonAddStringToObject" in body
    assert "MakeCheckedCJsonArray()" not in body
    assert '"files"' not in body
    assert '"MCP response allocation failed"' in checked
    assert "std::string(printed.get())" in checked
    assert "argument.set_value<std::string>(CheckedCJsonPrint(value));" in source


def test_generic_sync_propagates_exact_declared_size_without_making_size_required():
    source = SOURCE.read_text(encoding="utf-8")
    validation = source[
        source.index("ValidateLessonAssetSyncPackOrThrow(") :
        source.index("void DownloadLessonAssetToVerifiedFile(")
    ]
    helper_start = source.index("void DownloadLessonAssetToVerifiedFile(")
    helper = source[
        helper_start : source.index("void EnsureDirOrThrow(", helper_start)
    ]
    body = sync_body()

    assert 'const cJSON* declared_size = cJSON_GetObjectItem(asset, "size");' in validation
    assert "declared_size != nullptr" in validation
    assert "static_cast<size_t>(declared_size->valuedouble)" in validation
    assert "asset.has_declared_size" in body
    assert "asset.declared_size" in body
    assert "const char* cache_key" in helper
    assert "bool has_declared_size" in helper
    assert "size_t declared_size" in helper
    assert "IsOptionalNonNegativeInteger(asset, \"size\")" in validation


def test_sync_size_policy_rejects_each_file_and_aggregate_before_worker_creation():
    source = SOURCE.read_text(encoding="utf-8")
    validation = function_body(source, "ValidateLessonAssetSyncPackOrThrow(")
    dispatch = function_body(source, "void McpServer::DoToolCall(")

    assert "IsLessonAssetDeclaredFileSizeAllowed" in validation
    assert "AccumulateLessonAssetDeclaredSize" in validation
    assert "declared_pack_bytes" in validation
    preflight = dispatch.index("ValidateLessonAssetSyncPackOrThrow(")
    worker = dispatch.index("StartLessonAssetSyncTask(")
    assert preflight < worker

def test_sync_response_has_task10_activation_envelope():
    body = sync_body()
    expected_fields = (
        "ready",
        "cacheKey",
        "manifestChecksum",
        "downloadedCount",
        "skippedCount",
        "failedCount",
        "criticalFailedCount",
        "activated",
        "previousEvicted",
        "errorCode",
    )
    attestation = ATTESTATION_SOURCE.read_text(encoding="utf-8")
    response_source = body + attestation
    for field in expected_fields:
        assert f'"{field}"' in response_source


def test_sync_hil_read_and_write_checkpoints_are_exact_and_cleanup_owned():
    body = (ROOT / "main" / "lesson_asset_http_transfer.cc").read_text(
        encoding="utf-8"
    )
    limit = body.index("LimitDownloadRead(")
    read = body.index("http.Read(buffer, want)")
    before_write = body.index("kBeforeDownloadWrite")
    fwrite = body.index("fwrite(buffer")
    increment = body.index("bytes_out +=")
    after_bytes = body.index("kAfterDownloadBytes", increment)

    assert limit < read < before_write < fwrite < increment < after_bytes
    assert "if (want == 0)" in body[limit:read]
    assert body.rindex("if (has_declared_size)", 0, limit) < limit
    assert "lesson asset storage write failed" in body
    assert 'ScopedTempPath tmp_path(destination + ".tmp")' in body
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS" in body
    assert "TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING" in body


def test_mcp_delegates_the_real_transfer_loop_to_the_host_tested_unit():
    source = SOURCE.read_text(encoding="utf-8")
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    transfer = (ROOT / "main" / "lesson_asset_http_transfer.cc").read_text(
        encoding="utf-8"
    )
    start = source.index(
        "bool DownloadLessonAssetToFile(", source.index("void EnsureSampleLessonAssetDir")
    )
    body = function_body(source, "bool DownloadLessonAssetToFile(", start)

    assert '"lesson_asset_http_transfer.cc"' in cmake
    assert "DownloadLessonAssetHttpBodyToFile(" in body
    assert "http->Read(" not in body
    assert "fwrite(" not in body
    assert "ScopedTempPath" not in body
    assert "http.Read(buffer, want)" in transfer
    assert "ret > static_cast<int>(want)" in transfer
    assert "LimitDownloadRead(" in transfer
    assert "if (want == 0)" in transfer
    assert "second_limit == 0" in transfer


def test_sample_sync_passes_empty_hil_context_but_generic_uses_canonical_key():
    source = SOURCE.read_text(encoding="utf-8")
    sample_start = source.index('AddUserOnlyTool("self.lesson_assets.sync_sample_to_sd"')
    generic_start = source.index(
        'AddUserOnlyTool("self.lesson_assets.sync_to_sd"', sample_start
    )
    sample = source[sample_start:generic_start]
    generic = sync_body()

    assert "mutation, nullptr, false, 0, url, dest" in sample
    compact_generic = " ".join(generic.split())
    assert (
        "mutation, cache_key, asset.has_declared_size, asset.declared_size"
        in compact_generic
    )
