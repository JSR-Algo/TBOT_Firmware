#ifndef LESSON_ASSET_DOWNLOAD_STAGING_H
#define LESSON_ASSET_DOWNLOAD_STAGING_H

#include <string>

class LessonAssetDownloadStagingFile {
public:
    explicit LessonAssetDownloadStagingFile(const std::string& destination);
    ~LessonAssetDownloadStagingFile();

    LessonAssetDownloadStagingFile(const LessonAssetDownloadStagingFile&) = delete;
    LessonAssetDownloadStagingFile& operator=(const LessonAssetDownloadStagingFile&) = delete;

    const std::string& path() const { return path_; }
    void Disarm() { armed_ = false; }

private:
    std::string path_;
    bool armed_ = true;
};

bool VerifyLessonAssetSha256(
    const std::string& path,
    const std::string& expected_sha256
);

void CommitVerifiedLessonAssetDownload(
    LessonAssetDownloadStagingFile& staging,
    const std::string& destination,
    const std::string& expected_sha256
);

#if defined(TBOT_LESSON_ASSET_STAGING_TESTING) && !defined(ESP_PLATFORM)
enum class LessonAssetSha256TestFailure {
    kNone,
    kOpen,
    kRead,
    kMbedtlsStart,
    kMbedtlsUpdate,
    kMbedtlsFinish,
};

void SetLessonAssetSha256TestFailure(LessonAssetSha256TestFailure failure);

enum class LessonAssetStagingFsTestFailure {
    kNone,
    kFlush,
    kFsync,
    kReplaceRename,
    kRestoreRename,
    kInterruptAfterBackupRename,
};

void SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure failure);
#endif

#endif  // LESSON_ASSET_DOWNLOAD_STAGING_H
