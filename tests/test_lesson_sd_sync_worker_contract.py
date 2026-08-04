"""Contracts keeping long lesson SD sync work off the Application task."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_sync_to_sd_dispatches_to_single_flight_low_priority_worker():
    header = read("main/mcp_server.h")
    source = read("main/mcp_server.cc")
    dispatch = function_body(source, "void McpServer::DoToolCall")

    worker_branch = dispatch.index('tool_name == "self.lesson_assets.sync_to_sd"')
    generic_schedule = dispatch.index("app.Schedule", worker_branch)
    assert "StartLessonAssetSyncTask" in dispatch[worker_branch:generic_schedule]
    assert "return;" in dispatch[worker_branch:generic_schedule]

    assert "std::atomic<bool> lesson_asset_sync_in_flight_" in header
    assert "static void LessonAssetSyncTask(void* arg)" in header

    starter = function_body(source, "bool McpServer::StartLessonAssetSyncTask")
    assert "lesson_asset_sync_in_flight_.exchange(true)" in starter
    assert "xTaskCreateWithCaps" in starter
    assert "tskIDLE_PRIORITY + 1" in starter
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in starter
    assert "lesson_asset_sync_in_flight_.store(false)" in starter

    worker = function_body(source, "void McpServer::LessonAssetSyncTask")
    assert "tool->Call" in worker
    assert "ReplyResult" in worker
    assert "ReplyError" in worker
    assert "lesson_asset_sync_in_flight_.store(false)" in worker
    assert "vTaskDeleteWithCaps(nullptr)" in worker


def test_generic_sync_keeps_response_and_verified_file_tracking_bounded():
    source = read("main/mcp_server.cc")
    sync_start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
    sync_end = source.index(
        '\n\n    AddUserOnlyTool("self.assets.set_download_url"', sync_start
    )
    sync_body = source[sync_start:sync_end]
    record_start = source.index("struct VerifiedLessonAssetFile")
    record_end = source.index("};", record_start)
    record = source[record_start:record_end]

    assert "MakeCheckedCJsonArray" not in sync_body
    assert '"files"' not in sync_body
    assert "const char* sha256" in record
    assert "const char* destination" in record
    compact = " ".join(sync_body.split())
    assert "std::strcmp( verified_file.sha256, asset.sha256) == 0" in compact


def test_download_buffer_prefers_psram_with_internal_fallback():
    source = read("main/lesson_asset_http_transfer.cc")
    allocation = function_body(source, "void* AllocateLessonAssetDownloadBuffer")

    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in allocation
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in allocation
    assert source.index("AllocateLessonAssetDownloadBuffer()") < source.index(
        "ScopedHeapAllocation buffer_allocation"
    )
