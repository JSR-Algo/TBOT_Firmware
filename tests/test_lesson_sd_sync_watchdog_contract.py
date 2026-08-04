from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def test_sd_sync_worker_owns_and_releases_its_watchdog_subscription():
    source = read("main/mcp_server.cc")
    worker = function_body(source, "void McpServer::LessonAssetSyncTaskBody")

    add = worker.index("esp_task_wdt_add(nullptr)")
    tool_call = worker.index("context->tool->Call")
    delete = worker.index("esp_task_wdt_delete(nullptr)")
    task_delete = worker.index("vTaskDeleteWithCaps(nullptr)")

    assert add < tool_call < delete < task_delete
    assert "watchdog_registered" in worker


def test_sd_sync_resets_only_subscribed_tasks():
    mcp_source = read("main/mcp_server.cc")
    transfer_source = read("main/lesson_asset_http_transfer.cc")
    staging_source = read("main/lesson_asset_download_staging.cc")

    for source in (mcp_source, transfer_source, staging_source):
        helper = function_body(
            source, "void ResetCurrentTaskWatchdogIfSubscribed"
        )
        assert "esp_task_wdt_status(nullptr) == ESP_OK" in helper
        assert "esp_task_wdt_reset()" in helper

    assert "ResetCurrentTaskWatchdogIfSubscribed();" in mcp_source
    assert "ResetCurrentTaskWatchdogIfSubscribed();" in transfer_source
    sha256 = function_body(staging_source, "std::string Sha256FileHex")
    assert "ResetCurrentTaskWatchdogIfSubscribed();" in sha256
    assert sha256.index("mbedtls_sha256_update") < sha256.index(
        "ResetCurrentTaskWatchdogIfSubscribed();"
    )
