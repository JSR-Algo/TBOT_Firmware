#include "lesson_asset_http_transfer.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "lesson_asset_download_raii.h"
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
#include "lesson_storage_hil_controller.h"
#include "lesson_storage_hil_hooks.h"
#endif

namespace {

constexpr std::size_t kLessonAssetSyncMaxBytes = 512 * 1024;
constexpr std::size_t kLessonAssetDownloadBufferBytes = 4096;
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
constexpr const char* kLessonAssetStorageWriteError =
    "lesson asset storage write failed";
#endif

}  // namespace

void DownloadLessonAssetHttpBodyToFile(
    Http& http,
    const char* cache_key,
    bool has_declared_size,
    std::size_t declared_size,
    const std::string& url,
    const std::string& destination,
    std::size_t& bytes_out
) {
    bytes_out = 0;
    const std::size_t content_length = http.GetBodyLength();
    if (content_length > kLessonAssetSyncMaxBytes) {
        throw std::runtime_error("asset too large: " + url);
    }

    ScopedTempPath tmp_path(destination + ".tmp");
    tmp_path.RemoveIfPresent();
    FILE* raw_file = std::fopen(tmp_path.path().c_str(), "wb");
    if (raw_file == nullptr) {
        throw std::runtime_error("failed to open SD file: " + tmp_path.path());
    }
    ScopedCFile file(raw_file);

    void* raw_buffer =
        heap_caps_malloc(kLessonAssetDownloadBufferBytes, MALLOC_CAP_8BIT);
    if (raw_buffer == nullptr) {
        throw std::runtime_error("failed to allocate download buffer");
    }
    ScopedHeapAllocation buffer_allocation(raw_buffer, heap_caps_free);
    char* buffer = static_cast<char*>(buffer_allocation.get());

    bool failed = false;
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
    bool first_write = true;
#else
    (void)cache_key;
    (void)has_declared_size;
    (void)declared_size;
#endif
    std::string error;
    while (true) {
        esp_task_wdt_reset();
        std::size_t want = kLessonAssetDownloadBufferBytes;
        if (content_length > 0) {
            if (bytes_out >= content_length) break;
            want = std::min(want, content_length - bytes_out);
        }
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
        if (has_declared_size) {
            want = LessonStorageHilController::GetInstance().LimitDownloadRead(
                cache_key, bytes_out, want, declared_size);
            if (want == 0) {
                const auto outcome = RunLessonStorageHilCheckpoint(
                    cache_key,
                    LessonStorageHilOperation::kSync,
                    LessonStorageHilCheckpoint::kAfterDownloadBytes,
                    static_cast<std::uint32_t>(bytes_out),
                    static_cast<std::uint32_t>(declared_size));
                if (outcome != LessonStorageHilHookOutcome::kContinue) {
                    failed = true;
                    error = kLessonAssetStorageWriteError;
                    break;
                }
                const std::size_t second_limit =
                    LessonStorageHilController::GetInstance().LimitDownloadRead(
                        cache_key,
                        bytes_out,
                        kLessonAssetDownloadBufferBytes,
                        declared_size);
                if (second_limit == 0) {
                    failed = true;
                    error = kLessonAssetStorageWriteError;
                    break;
                }
                want = second_limit;
                if (content_length > 0) {
                    want = std::min(want, content_length - bytes_out);
                }
            }
        }
#endif
        const int ret = http.Read(buffer, want);
        if (ret < 0) {
            failed = true;
            error = "read error for " + url;
            break;
        }
        if (ret > static_cast<int>(want)) {
            failed = true;
            error = "invalid read size for " + url;
            break;
        }
        if (ret == 0) break;
        const std::size_t read_bytes = static_cast<std::size_t>(ret);
        if (bytes_out + read_bytes > kLessonAssetSyncMaxBytes) {
            failed = true;
            error = "asset too large: " + url;
            break;
        }
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
        if (first_write && RunLessonStorageHilCheckpoint(
                cache_key,
                LessonStorageHilOperation::kSync,
                LessonStorageHilCheckpoint::kBeforeDownloadWrite,
                0,
                has_declared_size ? static_cast<std::uint32_t>(declared_size) : 0) !=
            LessonStorageHilHookOutcome::kContinue) {
            failed = true;
            error = kLessonAssetStorageWriteError;
            break;
        }
        first_write = false;
#endif
        if (std::fwrite(buffer, 1, read_bytes, file.get()) != read_bytes) {
            failed = true;
            error = "write error for " + tmp_path.path();
            break;
        }
        bytes_out += read_bytes;
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
        if (has_declared_size && RunLessonStorageHilCheckpoint(
                cache_key,
                LessonStorageHilOperation::kSync,
                LessonStorageHilCheckpoint::kAfterDownloadBytes,
                static_cast<std::uint32_t>(bytes_out),
                static_cast<std::uint32_t>(declared_size)) !=
            LessonStorageHilHookOutcome::kContinue) {
            failed = true;
            error = kLessonAssetStorageWriteError;
            break;
        }
#endif
    }

    if (file.Close() != 0 && !failed) {
        failed = true;
        error = "write error for " + tmp_path.path();
    }
    if (!failed && content_length > 0 && bytes_out != content_length) {
        failed = true;
        error = "short read for " + url;
    }
    if (!failed && bytes_out == 0) {
        failed = true;
        error = "empty asset: " + url;
    }
    if (failed) {
        throw std::runtime_error(error);
    }
    tmp_path.CommitTo(destination);
}
