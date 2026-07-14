from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "mcp_server.cc"
ATTESTATION_SOURCE = ROOT / "main" / "lesson_asset_sync_attestation.cc"
MAIN_CMAKE = ROOT / "main" / "CMakeLists.txt"


def sync_body() -> str:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
    end = source.index('\n\n    AddUserOnlyTool("self.assets.set_download_url"', start)
    return source[start:end]


def test_ready_attestation_echoes_exact_verified_manifest_checksum():
    body = sync_body()
    attestation = ATTESTATION_SOURCE.read_text(encoding="utf-8")

    assert 'JsonStringField(pack, "manifestChecksum")' in body
    assert "AddLessonAssetSyncAttestation(" in body
    assert '"lesson_asset_sync_attestation.cc"' in MAIN_CMAKE.read_text(encoding="utf-8")
    assert "IsSha256Hex(manifest_checksum_value)" in attestation
    assert "manifest_checksum_value == manifest_checksum" in attestation
    assert "cache_key_value.find(manifest_checksum_value)" in attestation
    assert 'cJSON_AddStringToObject(response, "manifestChecksum", manifest_checksum)' in attestation


def test_missing_whitespace_or_cache_key_mismatch_cannot_claim_ready_or_checksum():
    attestation = ATTESTATION_SOURCE.read_text(encoding="utf-8")

    assert "manifest_checksum_valid" in attestation
    assert "cache_key_matches_manifest" in attestation
    assert "pack_verified" in attestation
    assert 'cJSON_AddBoolToObject(response, "ready", pack_verified)' in attestation
    checksum_add = attestation.index(
        'cJSON_AddStringToObject(response, "manifestChecksum", manifest_checksum)'
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
    assert "asset_count, verified, failed" in body
