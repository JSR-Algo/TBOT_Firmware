from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "mcp_server.cc"


def sync_body() -> str:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
    end = source.index('\n\n    AddUserOnlyTool("self.assets.set_download_url"', start)
    return source[start:end]


def test_ready_attestation_echoes_exact_verified_manifest_checksum():
    body = sync_body()

    assert 'JsonStringField(pack, "manifestChecksum")' in body
    assert "IsSha256Hex(manifest_checksum_value)" in body
    assert "manifest_checksum_value == manifest_checksum" in body
    assert "cache_key_value.find(manifest_checksum_value)" in body
    assert 'cJSON_AddStringToObject(json, "manifestChecksum", manifest_checksum)' in body


def test_missing_whitespace_or_cache_key_mismatch_cannot_claim_ready_or_checksum():
    body = sync_body()

    assert "manifest_checksum_valid" in body
    assert "cache_key_matches_manifest" in body
    assert "pack_verified" in body
    assert 'cJSON_AddBoolToObject(json, "ready", pack_verified)' in body
    checksum_add = body.index(
        'cJSON_AddStringToObject(json, "manifestChecksum", manifest_checksum)'
    )
    verified_guard = body.rindex("if (pack_verified)", 0, checksum_add)
    assert verified_guard < checksum_add


def test_failed_or_empty_asset_pack_cannot_claim_ready_checksum():
    body = sync_body()

    assert "asset_count > 0" in body
    assert "verified == asset_count" in body
    assert "failed == 0" in body
    assert "pack_verified" in body
