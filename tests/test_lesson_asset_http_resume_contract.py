from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/lesson_asset_http_transfer.cc").read_text(encoding="utf-8")
HEADER = (ROOT / "main/lesson_asset_http_transfer.h").read_text(encoding="utf-8")
KCONFIG = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")


def test_transfer_budget_covers_full_farm_v8_without_buffering_it_in_ram():
    assert "CONFIG_TBOT_LESSON_ASSET_MAX_FILE_BYTES" in HEADER
    assert "CONFIG_TBOT_LESSON_ASSET_MAX_PACK_BYTES" in HEADER
    assert "128U * 1024U * 1024U" in HEADER
    assert "256U * 1024U * 1024U" in HEADER
    assert "IsLessonAssetDeclaredFileSizeAllowed" in HEADER
    assert "AccumulateLessonAssetDeclaredSize" in HEADER
    assert "LessonAssetMaxFileBytes()" in SOURCE
    assert "kLessonAssetDownloadBufferBytes = 4096" in SOURCE
    assert "heap_caps_malloc(LessonAssetMaxFileBytes" not in SOURCE
    assert "std::string(content_length" not in SOURCE
    assert "config TBOT_LESSON_ASSET_MAX_FILE_BYTES" in KCONFIG
    assert "default 134217728" in KCONFIG
    assert "config TBOT_LESSON_ASSET_MAX_PACK_BYTES" in KCONFIG
    assert "default 268435456" in KCONFIG


def test_partial_receive_reconnects_with_validated_http_range():
    assert 'http.SetHeader("Range", "bytes=" + std::to_string(offset) + "-")' in SOURCE
    assert 'http.Open("GET", url)' in SOURCE
    assert "const int status = http.GetStatusCode()" in SOURCE
    assert "status != 206" in SOURCE
    assert 'http.GetResponseHeader("Content-Range")' in SOURCE
    assert "range_start != offset" in SOURCE
    assert "range_total != expected_total" in SOURCE


def test_resume_checkpoint_is_synced_and_attempts_are_bounded():
    assert "std::fflush(file)" in SOURCE
    assert "fsync(descriptor)" in SOURCE
    assert "kLessonAssetMaxResumeAttempts = 3" in SOURCE
    assert "resume_attempts >= kLessonAssetMaxResumeAttempts" in SOURCE


def test_watchdog_is_fed_around_checkpoint_and_reconnect_waits():
    flush = SOURCE[SOURCE.index("bool FlushDownloadCheckpoint") : SOURCE.index(
        "bool ResumeLessonAssetHttp"
    )]
    resume = SOURCE[SOURCE.index("bool ResumeLessonAssetHttp") : SOURCE.index(
        "}  // namespace"
    )]

    assert flush.count("esp_task_wdt_reset()") >= 4
    assert flush.index("esp_task_wdt_reset()") < flush.index("std::fflush(file)")
    assert flush.rindex("esp_task_wdt_reset()") > flush.index("fsync(descriptor)")
    assert resume.count("esp_task_wdt_reset()") >= 6
    assert resume.index("esp_task_wdt_reset()") < resume.index('http.Open("GET", url)')
    assert resume.rindex("esp_task_wdt_reset()") > resume.index("http.GetStatusCode()")
