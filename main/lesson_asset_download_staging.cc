#include "lesson_asset_download_staging.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

namespace {

#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
LessonAssetSha256TestFailure g_sha256_failure = LessonAssetSha256TestFailure::kNone;
LessonAssetStagingFsTestFailure g_fs_failure = LessonAssetStagingFsTestFailure::kNone;

bool ShouldInject(LessonAssetSha256TestFailure failure) {
    return g_sha256_failure == failure;
}

bool ShouldInject(LessonAssetStagingFsTestFailure failure) {
    return g_fs_failure == failure;
}
#endif

struct FileClose {
    void operator()(FILE* file) const {
        if (file != nullptr) std::fclose(file);
    }
};

class Sha256Context {
public:
    Sha256Context() { mbedtls_sha256_init(&context_); }
    ~Sha256Context() { mbedtls_sha256_free(&context_); }
    mbedtls_sha256_context* get() { return &context_; }

private:
    mbedtls_sha256_context context_;
};

bool IsExactLowerSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (char byte : value) {
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool IsRegularFile(const std::string& path) {
    struct stat file_stat {};
    return stat(path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode);
}

bool PathIsMissing(const std::string& path) {
    struct stat file_stat {};
    return stat(path.c_str(), &file_stat) != 0 && errno == ENOENT;
}

void RemoveFileIfPresent(const std::string& path, const char* error_message) {
    if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
        throw std::runtime_error(error_message);
    }
}

void RecoverInterruptedReplacement(const std::string& destination) {
    const std::string backup = destination + ".backup";
    if (PathIsMissing(backup)) return;
    if (!IsRegularFile(backup)) {
        throw std::runtime_error("lesson asset backup is not a regular file");
    }

    if (IsRegularFile(destination)) {
        RemoveFileIfPresent(backup, "failed to clean stale lesson asset backup");
        return;
    }
    if (!PathIsMissing(destination)) {
        throw std::runtime_error("lesson asset destination blocks backup recovery");
    }
    if (std::rename(backup.c_str(), destination.c_str()) != 0) {
        throw std::runtime_error("failed to recover lesson asset backup");
    }
}

void FlushAndSyncFile(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb+");
    if (file == nullptr) {
        throw std::runtime_error("failed to open verified lesson asset for sync");
    }

#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
    const bool flush_failed = ShouldInject(LessonAssetStagingFsTestFailure::kFlush);
#else
    const bool flush_failed = false;
#endif
    if (flush_failed || std::fflush(file) != 0) {
        std::fclose(file);
        throw std::runtime_error("failed to flush verified lesson asset");
    }

    const int descriptor = fileno(file);
#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
    const bool fsync_failed = ShouldInject(LessonAssetStagingFsTestFailure::kFsync);
#else
    const bool fsync_failed = false;
#endif
    if (descriptor < 0 || fsync_failed || fsync(descriptor) != 0) {
        std::fclose(file);
        throw std::runtime_error("failed to sync verified lesson asset");
    }
    if (std::fclose(file) != 0) {
        throw std::runtime_error("failed to close verified lesson asset");
    }
}

std::string Sha256FileHex(const std::string& path) {
#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
    if (ShouldInject(LessonAssetSha256TestFailure::kOpen)) {
        throw std::runtime_error("failed to open asset for sha256");
    }
#endif
    std::unique_ptr<FILE, FileClose> file(std::fopen(path.c_str(), "rb"));
    if (!file) {
        throw std::runtime_error("failed to open asset for sha256");
    }

    Sha256Context context;
#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
    int rc = ShouldInject(LessonAssetSha256TestFailure::kMbedtlsStart) ? -1 :
#else
    int rc =
#endif
#if MBEDTLS_VERSION_MAJOR >= 3
        mbedtls_sha256_starts(context.get(), 0);
#else
        mbedtls_sha256_starts_ret(context.get(), 0);
#endif
    if (rc != 0) {
        throw std::runtime_error("failed to start sha256");
    }

    unsigned char buffer[4096];
    while (true) {
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file.get());
#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
        if (ShouldInject(LessonAssetSha256TestFailure::kRead)) {
            throw std::runtime_error("failed to read asset for sha256");
        }
#endif
        if (read > 0) {
#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
            rc = ShouldInject(LessonAssetSha256TestFailure::kMbedtlsUpdate) ? -1 :
#else
            rc =
#endif
#if MBEDTLS_VERSION_MAJOR >= 3
                mbedtls_sha256_update(context.get(), buffer, read);
#else
                mbedtls_sha256_update_ret(context.get(), buffer, read);
#endif
            if (rc != 0) {
                throw std::runtime_error("failed to update sha256");
            }
        }
        if (read < sizeof(buffer)) {
            if (std::ferror(file.get())) {
                throw std::runtime_error("failed to read asset for sha256");
            }
            break;
        }
    }

    unsigned char digest[32];
#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
    rc = ShouldInject(LessonAssetSha256TestFailure::kMbedtlsFinish) ? -1 :
#else
    rc =
#endif
#if MBEDTLS_VERSION_MAJOR >= 3
        mbedtls_sha256_finish(context.get(), digest);
#else
        mbedtls_sha256_finish_ret(context.get(), digest);
#endif
    if (rc != 0) {
        throw std::runtime_error("failed to finish sha256");
    }

    char hex[65];
    for (int index = 0; index < 32; ++index) {
        std::snprintf(hex + index * 2, sizeof(hex) - index * 2, "%02x", digest[index]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

}  // namespace

LessonAssetDownloadStagingFile::LessonAssetDownloadStagingFile(
    const std::string& destination
) : path_(destination + ".download") {
    RecoverInterruptedReplacement(destination);
    std::remove(path_.c_str());
}

LessonAssetDownloadStagingFile::~LessonAssetDownloadStagingFile() {
    if (armed_) {
        std::remove(path_.c_str());
    }
}

bool VerifyLessonAssetSha256(
    const std::string& path,
    const std::string& expected_sha256
) {
    if (!IsExactLowerSha256(expected_sha256)) {
        return false;
    }
    if (!IsRegularFile(path)) {
        return false;
    }
    return Sha256FileHex(path) == expected_sha256;
}

void CommitVerifiedLessonAssetDownload(
    LessonAssetDownloadStagingFile& staging,
    const std::string& destination,
    const std::string& expected_sha256
) {
    if (!VerifyLessonAssetSha256(staging.path(), expected_sha256)) {
        throw std::runtime_error("lesson asset checksum mismatch");
    }
    FlushAndSyncFile(staging.path());

    const std::string backup = destination + ".backup";
    const bool destination_exists = IsRegularFile(destination);
    if (!destination_exists && !PathIsMissing(destination)) {
        throw std::runtime_error("lesson asset destination is not a regular file");
    }
    RemoveFileIfPresent(backup, "failed to clean lesson asset backup");
    if (destination_exists && std::rename(destination.c_str(), backup.c_str()) != 0) {
        throw std::runtime_error("failed to preserve existing lesson asset");
    }

#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
    if (destination_exists &&
        ShouldInject(LessonAssetStagingFsTestFailure::kInterruptAfterBackupRename)) {
        staging.Disarm();
        throw std::runtime_error("interrupted lesson asset replacement");
    }
    const bool replace_failed =
        ShouldInject(LessonAssetStagingFsTestFailure::kReplaceRename) ||
        ShouldInject(LessonAssetStagingFsTestFailure::kRestoreRename);
#else
    const bool replace_failed = false;
#endif
    if (replace_failed || std::rename(staging.path().c_str(), destination.c_str()) != 0) {
        if (destination_exists) {
#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
            const bool restore_failed =
                ShouldInject(LessonAssetStagingFsTestFailure::kRestoreRename);
#else
            const bool restore_failed = false;
#endif
            if (restore_failed || std::rename(backup.c_str(), destination.c_str()) != 0) {
                throw std::runtime_error(
                    "lesson asset commit failed; previous asset remains in backup");
            }
        }
        throw std::runtime_error("lesson asset commit failed");
    }
    staging.Disarm();
    if (destination_exists) {
        RemoveFileIfPresent(backup, "lesson asset committed but backup cleanup failed");
    }
}

#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
void SetLessonAssetSha256TestFailure(LessonAssetSha256TestFailure failure) {
    g_sha256_failure = failure;
}


void SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure failure) {
    g_fs_failure = failure;
}
#endif
