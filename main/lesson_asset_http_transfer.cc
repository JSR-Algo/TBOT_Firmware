#include "lesson_asset_http_transfer.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "lesson_asset_download_raii.h"
#include "lesson_trgb_size_policy.h"
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

void ResetCurrentTaskWatchdogIfSubscribed() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
}

bool IsTrgbMediaType(const char* media_type) {
    return media_type != nullptr &&
        std::strcmp(media_type, "application/vnd.tbot.rgb565-indexed") == 0;
}

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

bool FlushDownloadCheckpoint(FILE* file) {
    ResetCurrentTaskWatchdogIfSubscribed();
    const int flush_result = std::fflush(file);
    ResetCurrentTaskWatchdogIfSubscribed();
    if (flush_result != 0) return false;
    const int descriptor = fileno(file);
    if (descriptor < 0) return false;
    ResetCurrentTaskWatchdogIfSubscribed();
    const int sync_result = fsync(descriptor);
    ResetCurrentTaskWatchdogIfSubscribed();
    return sync_result == 0;
}

bool ParseUnsignedToken(
    const std::string& text,
    std::size_t begin,
    std::size_t end,
    std::size_t& value
) {
    if (begin >= end) return false;
    std::size_t parsed = 0;
    for (std::size_t index = begin; index < end; ++index) {
        const char ch = text[index];
        if (ch < '0' || ch > '9') return false;
        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

struct ParsedContentRange {
    std::size_t first = 0;
    std::size_t last = 0;
    bool has_total = false;
    std::size_t total = 0;
};

bool ParseContentRange(const std::string& header, ParsedContentRange& parsed) {
    constexpr const char* kPrefix = "bytes ";
    constexpr std::size_t kPrefixLength = 6;
    if (header.compare(0, kPrefixLength, kPrefix) != 0) return false;

    const std::size_t dash = header.find('-', kPrefixLength);
    if (dash == std::string::npos) return false;
    const std::size_t slash = header.find('/', dash + 1);
    if (slash == std::string::npos || slash + 1 >= header.size()) return false;
    if (!ParseUnsignedToken(header, kPrefixLength, dash, parsed.first) ||
        !ParseUnsignedToken(header, dash + 1, slash, parsed.last) ||
        parsed.last < parsed.first) {
        return false;
    }

    if (header[slash + 1] == '*') return false;

    parsed.has_total = true;
    if (!ParseUnsignedToken(header, slash + 1, header.size(), parsed.total)) {
        return false;
    }
    return parsed.total > parsed.last;
}

}  // namespace

void DownloadLessonAssetHttpBodyToFile(
    Http& http,
    const char* cache_key,
    bool has_declared_size,
    std::size_t declared_size,
    const std::string& url,
    const std::string& destination,
    std::size_t& bytes_out,
    const char* media_type
) {
    bytes_out = 0;
    const bool trgb = IsTrgbMediaType(media_type);
    const std::size_t max_bytes = trgb
        ? std::min(LessonAssetMaxFileBytes(), tbot::kLessonTrgbMaxFileBytes)
        : LessonAssetMaxFileBytes();
    if (trgb && (!has_declared_size ||
                 !tbot::LessonTrgbPlausibleContainerBytes(declared_size))) {
        throw std::runtime_error("invalid TRGB size: " + url);
    }
    if (has_declared_size && declared_size > max_bytes) {
        throw std::runtime_error("asset too large: " + url);
    }

    ScopedTempPath tmp_path(destination + ".tmp");
    tmp_path.RemoveIfPresent();

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
    std::size_t expected_total = has_declared_size ? declared_size : 0;
    for (int attempt = 0; attempt <= kLessonAssetMaxResumeAttempts; ++attempt) {
        const bool resumed = bytes_out > 0;
        if (resumed) {
            http.SetHeader("Range", "bytes=" + std::to_string(bytes_out) + "-");
        }

        ResetCurrentTaskWatchdogIfSubscribed();
        if (!http.Open("GET", url)) {
            ResetCurrentTaskWatchdogIfSubscribed();
            failed = true;
            error = "failed to open URL: " + url;
            if (resumed && attempt < kLessonAssetMaxResumeAttempts) {
                failed = false;
                continue;
            }
            break;
        }
        ScopedHttpClose<Http> http_close(&http);
        ResetCurrentTaskWatchdogIfSubscribed();

        ResetCurrentTaskWatchdogIfSubscribed();
        const int status = http.GetStatusCode();
        ResetCurrentTaskWatchdogIfSubscribed();
        if ((!resumed && status != 200) || (resumed && status != 206)) {
            failed = true;
            error = "unexpected status " + std::to_string(status) + " for " + url;
            break;
        }

        const std::size_t content_length = http.GetBodyLength();
        if (!resumed && expected_total == 0 && content_length > 0) {
            expected_total = content_length;
        }
        if (content_length > max_bytes ||
            content_length > max_bytes - bytes_out ||
            (has_declared_size && content_length > declared_size - bytes_out) ||
            (trgb && content_length > 0 && !resumed && content_length != declared_size)) {
            failed = true;
            error = "asset too large: " + url;
            break;
        }

        if (resumed) {
            ParsedContentRange range;
            if (!ParseContentRange(http.GetResponseHeader("Content-Range"), range) ||
                range.first != bytes_out ||
                content_length == 0 ||
                range.last - range.first + 1 != content_length ||
                !range.has_total || range.total > max_bytes ||
                (expected_total > 0 && range.total != expected_total)) {
                failed = true;
                error = "invalid resume range for " + url;
                break;
            }
        }

        FILE* raw_file = std::fopen(
            tmp_path.path().c_str(),
            resumed ? "ab" : "wb");
        if (raw_file == nullptr) {
            failed = true;
            error = "failed to open SD file: " + tmp_path.path();
            break;
        }
        ScopedCFile file(raw_file);
        bool retry = false;
        const std::size_t attempt_start = bytes_out;

        while (true) {
            ResetCurrentTaskWatchdogIfSubscribed();
            std::size_t want = kLessonAssetDownloadBufferBytes;
            if (content_length > 0) {
                const std::size_t attempt_bytes = bytes_out - attempt_start;
                if (attempt_bytes >= content_length) break;
                want = std::min(want, content_length - attempt_bytes);
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
                        want = std::min(want, content_length - (bytes_out - attempt_start));
                    }
                }
            }
#endif
            const int ret = http.Read(buffer, want);
            if (ret < 0) {
                retry = bytes_out > 0 && attempt < kLessonAssetMaxResumeAttempts;
                if (!retry) {
                    failed = true;
                    error = "read error for " + url;
                }
                break;
            }
            if (ret > static_cast<int>(want)) {
                failed = true;
                error = "invalid read size for " + url;
                break;
            }
            if (ret == 0) break;
            const std::size_t read_bytes = static_cast<std::size_t>(ret);
            if (read_bytes > max_bytes - bytes_out ||
                (has_declared_size && read_bytes > declared_size - bytes_out)) {
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

        const std::size_t attempt_bytes = bytes_out - attempt_start;
        const bool response_short = content_length > 0 && attempt_bytes != content_length;
        const bool declared_short = has_declared_size && bytes_out < declared_size;
        const bool continue_transfer =
            (retry || response_short || declared_short) && bytes_out > 0 &&
            attempt < kLessonAssetMaxResumeAttempts;
        if (continue_transfer && !failed && !FlushDownloadCheckpoint(file.get())) {
            failed = true;
            error = "write error for " + tmp_path.path();
        }
        ResetCurrentTaskWatchdogIfSubscribed();
        if (file.Close() != 0 && !failed) {
            failed = true;
            error = "write error for " + tmp_path.path();
        }
        ResetCurrentTaskWatchdogIfSubscribed();
        ResetCurrentTaskWatchdogIfSubscribed();
        http_close.Close();
        ResetCurrentTaskWatchdogIfSubscribed();
        if (failed) {
            break;
        }

        if (continue_transfer) {
            continue;
        }
        if (retry || response_short) {
            failed = true;
            error = "short read for " + url;
        }
        break;
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
