#include "lesson_asset_http_transfer.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "lesson_asset_download_raii.h"
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
#include "lesson_storage_hil_controller.h"
#include "lesson_storage_hil_hooks.h"
#endif

namespace {

constexpr std::size_t kLessonAssetDownloadBufferBytes = 4096;
constexpr int kLessonAssetMaxResumeAttempts = 3;
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
constexpr const char* kLessonAssetStorageWriteError =
    "lesson asset storage write failed";
#endif

void* AllocateLessonAssetDownloadBuffer() {
#if defined(ESP_PLATFORM) && CONFIG_SPIRAM
    void* buffer = heap_caps_malloc(
        kLessonAssetDownloadBufferBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer != nullptr) {
        return buffer;
    }
#endif
    return heap_caps_malloc(
        kLessonAssetDownloadBufferBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool ParseDecimalSize(
    const std::string& value,
    std::size_t begin,
    std::size_t end,
    std::size_t& parsed
) {
    if (begin >= end) return false;
    std::size_t result = 0;
    for (std::size_t index = begin; index < end; ++index) {
        const char byte = value[index];
        if (byte < '0' || byte > '9') return false;
        const std::size_t digit = static_cast<std::size_t>(byte - '0');
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    parsed = result;
    return true;
}

bool ParseContentRange(
    const std::string& value,
    std::size_t& start,
    std::size_t& end,
    std::size_t& total
) {
    constexpr const char* kPrefix = "bytes ";
    constexpr std::size_t kPrefixLength = 6;
    if (value.compare(0, kPrefixLength, kPrefix) != 0) return false;
    const std::size_t dash = value.find('-', kPrefixLength);
    const std::size_t slash = value.find('/', dash == std::string::npos ? 0 : dash + 1);
    if (dash == std::string::npos || slash == std::string::npos ||
        value.find_first_not_of("0123456789", slash + 1) != std::string::npos) {
        return false;
    }
    return ParseDecimalSize(value, kPrefixLength, dash, start) &&
           ParseDecimalSize(value, dash + 1, slash, end) &&
           ParseDecimalSize(value, slash + 1, value.size(), total);
}

bool FlushDownloadCheckpoint(FILE* file) {
    esp_task_wdt_reset();
    const int flush_result = std::fflush(file);
    esp_task_wdt_reset();
    if (flush_result != 0) return false;
    const int descriptor = fileno(file);
    if (descriptor < 0) return false;
    esp_task_wdt_reset();
    const int sync_result = fsync(descriptor);
    esp_task_wdt_reset();
    return sync_result == 0;
}

bool ResumeLessonAssetHttp(
    Http& http,
    const std::string& url,
    std::size_t offset,
    bool has_declared_size,
    std::size_t declared_size,
    std::size_t& expected_total
) {
    esp_task_wdt_reset();
    http.Close();
    esp_task_wdt_reset();
    http.SetHeader("Range", "bytes=" + std::to_string(offset) + "-");
    esp_task_wdt_reset();
    const bool opened = http.Open("GET", url);
    esp_task_wdt_reset();
    if (!opened) return false;
    esp_task_wdt_reset();
    const int status = http.GetStatusCode();
    esp_task_wdt_reset();
    if (status != 206) return false;

    std::size_t range_start = 0;
    std::size_t range_end = 0;
    std::size_t range_total = 0;
    if (!ParseContentRange(
            http.GetResponseHeader("Content-Range"),
            range_start,
            range_end,
            range_total) ||
        range_start != offset || range_end < range_start || range_end >= range_total ||
        range_total > LessonAssetMaxFileBytes() ||
        (expected_total > 0 && range_total != expected_total) ||
        (has_declared_size && range_total != declared_size)) {
        return false;
    }
    const std::size_t response_length = http.GetBodyLength();
    if (response_length > 0 && response_length != range_end - range_start + 1) {
        return false;
    }
    expected_total = range_total;
    return true;
}

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
    if (content_length > LessonAssetMaxFileBytes() ||
        (has_declared_size &&
         !IsLessonAssetDeclaredFileSizeAllowed(declared_size))) {
        throw std::runtime_error("asset too large: " + url);
    }
    std::size_t expected_total = content_length;

    ScopedTempPath tmp_path(destination + ".tmp");
    tmp_path.RemoveIfPresent();
    FILE* raw_file = std::fopen(tmp_path.path().c_str(), "wb");
    if (raw_file == nullptr) {
        throw std::runtime_error("failed to open SD file: " + tmp_path.path());
    }
    ScopedCFile file(raw_file);

    void* raw_buffer = AllocateLessonAssetDownloadBuffer();
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
#endif
    std::string error;
    int resume_attempts = 0;
    while (true) {
        esp_task_wdt_reset();
        std::size_t want = kLessonAssetDownloadBufferBytes;
        if (expected_total > 0) {
            if (bytes_out >= expected_total) break;
            want = std::min(want, expected_total - bytes_out);
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
                if (expected_total > 0) {
                    want = std::min(want, expected_total - bytes_out);
                }
            }
        }
#endif
        const int ret = http.Read(buffer, want);
        if (ret < 0) {
            if (bytes_out == 0 || resume_attempts >= kLessonAssetMaxResumeAttempts ||
                !FlushDownloadCheckpoint(file.get()) ||
                !ResumeLessonAssetHttp(
                    http,
                    url,
                    bytes_out,
                    has_declared_size,
                    declared_size,
                    expected_total)) {
                failed = true;
                error = "read error for " + url;
                break;
            }
            ++resume_attempts;
            continue;
        }
        if (ret > static_cast<int>(want)) {
            failed = true;
            error = "invalid read size for " + url;
            break;
        }
        if (ret == 0) break;
        const std::size_t read_bytes = static_cast<std::size_t>(ret);
        if (bytes_out > LessonAssetMaxFileBytes() ||
            read_bytes > LessonAssetMaxFileBytes() - bytes_out) {
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
    if (!failed && expected_total > 0 && bytes_out != expected_total) {
        failed = true;
        error = "short read for " + url;
    }
    if (!failed && has_declared_size && bytes_out != declared_size) {
        failed = true;
        error = "asset declared size mismatch for " + url;
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
