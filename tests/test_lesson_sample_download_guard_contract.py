from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "mcp_server.cc"


def sample_sync_body() -> str:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index('AddUserOnlyTool("self.lesson_assets.sync_sample_to_sd"')
    end = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"', start)
    return source[start:end]


def test_sample_base_url_is_strictly_validated_before_mutation_and_io():
    body = sample_sync_body()

    validate = body.index("IsAllowedSampleLessonAssetBaseUrl(base_url)")
    lease = body.index('TryBeginMutation("sync")')
    filesystem = body.index("EnsureSampleLessonAssetDir(mutation)")
    network = body.index("DownloadLessonAssetToVerifiedFile(")
    assert validate < lease < filesystem < network

    validator = (ROOT / "main" / "lesson_asset_sample_url_policy.cc").read_text(
        encoding="utf-8"
    )
    assert "IsAllowedLessonAssetSyncUrl(value)" in validator
    assert "value.find('?')" in validator
    assert "value.find('#')" in validator
    assert "IsValidSampleLessonAssetAuthority(" in validator


def test_sample_manifest_is_fixed_to_server_filename_digest_pairs():
    source = SOURCE.read_text(encoding="utf-8")
    expected = {
        "barn-round-field-poster.jpg": "d5cdaba9f9086ef56a5f41c5fddf2e32b91ecfe141cc346f3221c7b221a3a357",
        "barn.png": "bf3d88d17867f02872e3e6aff31b5d4d0a94977a5efa4214b7e831122938511b",
        "bright-teach.png": "576d86a75686f6eab606295529593da14b01554e21e0601c8f29aedbc1ba4965",
        "bright-listening.png": "572a61f140eca17968a85f61704967d03a1a3311222335e32b94b1ab370e2419",
        "bright-thinking.png": "3d3d3c3a7c6993ad2346e11cfee72a15750d0b5f35642b8d949902a8745352c8",
        "bright-celebrate.png": "8392fb31c53030147d27fbd96c5b2dd1a4e5c33efd35f8727bee6dabda62605d",
    }

    assert "struct SampleLessonAsset" in source
    assert "kSampleLessonAssets" in source
    for filename, digest in expected.items():
        assert f'{{"{filename}", "{digest}"}}' in source

    body = sample_sync_body()
    assert 'Property("base_url", kPropertyTypeString' in body
    assert body.count("Property(") == 1
    assert "assetPack" not in body
    assert "properties[\"sha256\"]" not in body
    assert "DownloadLessonAssetToVerifiedFile(" in body
    assert 'CheckedCJsonAddStringToObject(item.get(), "sha256", asset.sha256)' in body


def test_download_resources_and_temp_commit_are_scope_owned():
    source = SOURCE.read_text(encoding="utf-8")
    body = source[
        source.index("bool DownloadLessonAssetToFile(") :
        source.index("\n}\n}\n\nMcpServer::McpServer", source.index("bool DownloadLessonAssetToFile("))
    ]

    assert '#include "lesson_asset_download_raii.h"' in source
    assert "ScopedHttpClose" in body
    assert "ScopedCFile" in body
    assert "ScopedHeapAllocation" in body
    assert "ScopedTempPath" in body
    guards = (ROOT / "main" / "lesson_asset_download_raii.h").read_text(encoding="utf-8")
    assert "#if !defined(ESP_PLATFORM)" in guards
    assert "http->Close()" not in body
    assert "fclose(fp)" not in body
    assert "heap_caps_free(buffer)" not in body
    assert "remove(tmp_path.c_str())" not in body


def test_host_raii_runner_is_portable_and_sanitized():
    runner = (ROOT / "scripts" / "run_host_native_lesson_asset_download_raii_test.sh").read_text(
        encoding="utf-8"
    )
    assert "-fsanitize=address,undefined" in runner
    assert "/Users/" not in runner


def test_mcp_cjson_runner_does_not_hardcode_a_developer_idf_checkout():
    runner = (ROOT / "scripts" / "run_host_native_mcp_cjson_oom_test.sh").read_text(
        encoding="utf-8"
    )
    assert "/Users/" not in runner
    assert "IDF_PATH" in runner
