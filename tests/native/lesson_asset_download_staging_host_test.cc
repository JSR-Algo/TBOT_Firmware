#include "lesson_asset_download_staging.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kRoot = "/tmp/tbot-lesson-asset-staging-host";
constexpr const char* kZeroSha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void Write(const std::string& path, const std::string& bytes = "asset") {
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
}

std::string Read(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void TestScopeCleanupOnDownloadException() {
    const std::string destination = std::string(kRoot) + "/network.png";
    const std::string staging_path = destination + ".download";
    try {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path());
        throw std::runtime_error("injected download failure");
    } catch (const std::runtime_error&) {
    }
    Expect(!fs::exists(staging_path), "download exception left staging file");
}

void TestMissingDestinationIsACacheMissNotAHashFailure() {
    const std::string missing = std::string(kRoot) + "/missing.png";
    Expect(!VerifyLessonAssetSha256(missing, kZeroSha256),
           "missing destination did not remain a cache miss");
}

void TestScopeCleanupOnEveryShaFailure() {
    const std::vector<LessonAssetSha256TestFailure> failures = {
        LessonAssetSha256TestFailure::kOpen,
        LessonAssetSha256TestFailure::kRead,
        LessonAssetSha256TestFailure::kMbedtlsStart,
        LessonAssetSha256TestFailure::kMbedtlsUpdate,
        LessonAssetSha256TestFailure::kMbedtlsFinish,
    };
    int index = 0;
    for (const auto failure : failures) {
        const std::string destination =
            std::string(kRoot) + "/sha-" + std::to_string(index++) + ".png";
        const std::string staging_path = destination + ".download";
        bool threw = false;
        try {
            LessonAssetDownloadStagingFile staging(destination);
            Write(staging.path());
            SetLessonAssetSha256TestFailure(failure);
            CommitVerifiedLessonAssetDownload(staging, destination, kZeroSha256);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        SetLessonAssetSha256TestFailure(LessonAssetSha256TestFailure::kNone);
        Expect(threw, "injected sha failure did not throw");
        Expect(!fs::exists(staging_path), "sha failure left staging file");
        Expect(!fs::exists(destination), "sha failure committed destination");
    }
}

void TestMismatchCleansAndSuccessfulRenameDisarms() {
    const std::string mismatch_destination = std::string(kRoot) + "/mismatch.png";
    try {
        LessonAssetDownloadStagingFile staging(mismatch_destination);
        Write(staging.path());
        CommitVerifiedLessonAssetDownload(
            staging, mismatch_destination, std::string(64, 'f'));
    } catch (const std::runtime_error&) {
    }
    Expect(!fs::exists(mismatch_destination + ".download"),
           "checksum mismatch left staging file");
    Expect(!fs::exists(mismatch_destination), "checksum mismatch committed destination");

    const std::string destination = std::string(kRoot) + "/committed.png";
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "committed");
        CommitVerifiedLessonAssetDownload(staging, destination, kZeroSha256);
        Expect(fs::exists(destination), "verified asset was not renamed");
        Expect(!fs::exists(staging.path()), "staging path survived rename");
    }
    Expect(fs::exists(destination), "armed guard deleted committed destination");
}

void TestFlushAndFsyncCompleteBeforeReplacement() {
    const std::vector<LessonAssetStagingFsTestFailure> failures = {
        LessonAssetStagingFsTestFailure::kFlush,
        LessonAssetStagingFsTestFailure::kFsync,
    };
    int index = 0;
    for (const auto failure : failures) {
        const std::string destination =
            std::string(kRoot) + "/durable-" + std::to_string(index++) + ".png";
        Write(destination, "known-good");
        bool threw = false;
        try {
            LessonAssetDownloadStagingFile staging(destination);
            Write(staging.path(), "replacement");
            SetLessonAssetStagingFsTestFailure(failure);
            CommitVerifiedLessonAssetDownload(staging, destination, kZeroSha256);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure::kNone);
        Expect(threw, "durability failure did not abort commit");
        Expect(Read(destination) == "known-good",
               "durability failure replaced last-known-good destination");
        Expect(!fs::exists(destination + ".download"),
               "durability failure left staging file");
        Expect(!fs::exists(destination + ".backup"),
               "durability failure created backup before sync");
    }
}

void TestReplacementRenameFailureRestoresLastKnownGood() {
    const std::string destination = std::string(kRoot) + "/replace-failure.png";
    Write(destination, "known-good");
    bool threw = false;
    try {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        SetLessonAssetStagingFsTestFailure(
            LessonAssetStagingFsTestFailure::kReplaceRename);
        CommitVerifiedLessonAssetDownload(staging, destination, kZeroSha256);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure::kNone);
    Expect(threw, "replacement rename failure did not throw");
    Expect(Read(destination) == "known-good",
           "replacement rename failure lost last-known-good destination");
    Expect(!fs::exists(destination + ".download"),
           "replacement rename failure left staging file");
    Expect(!fs::exists(destination + ".backup"),
           "replacement rename failure left restored backup");
}

void TestInterruptedReplacementRecoversOnNextAttempt() {
    const std::string destination = std::string(kRoot) + "/interrupted.png";
    const std::string staging_path = destination + ".download";
    const std::string backup_path = destination + ".backup";
    Write(destination, "known-good");
    try {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "interrupted-replacement");
        SetLessonAssetStagingFsTestFailure(
            LessonAssetStagingFsTestFailure::kInterruptAfterBackupRename);
        CommitVerifiedLessonAssetDownload(staging, destination, kZeroSha256);
    } catch (const std::runtime_error&) {
    }
    SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure::kNone);
    Expect(!fs::exists(destination), "interruption seam unexpectedly restored destination");
    Expect(fs::exists(staging_path), "interruption seam did not preserve staged file");
    Expect(Read(backup_path) == "known-good",
           "interruption seam did not preserve last-known-good backup");

    {
        LessonAssetDownloadStagingFile next_attempt(destination);
        Expect(Read(destination) == "known-good",
               "next attempt did not recover interrupted replacement");
        Expect(!fs::exists(backup_path), "recovery left backup file");
        Expect(!fs::exists(staging_path), "recovery left interrupted staging file");
    }
}

void TestFailedRestoreLeavesTruthfulRecoverableState() {
    const std::string destination = std::string(kRoot) + "/restore-failure.png";
    Write(destination, "known-good");
    bool threw = false;
    try {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        SetLessonAssetStagingFsTestFailure(
            LessonAssetStagingFsTestFailure::kRestoreRename);
        CommitVerifiedLessonAssetDownload(staging, destination, kZeroSha256);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure::kNone);
    Expect(threw, "failed restore did not throw");
    Expect(!fs::exists(destination), "failed restore reported a false destination");
    Expect(Read(destination + ".backup") == "known-good",
           "failed restore lost recoverable last-known-good backup");
    Expect(!fs::exists(destination + ".download"),
           "failed restore left unverified staging state");

    LessonAssetDownloadStagingFile next_attempt(destination);
    Expect(Read(destination) == "known-good",
           "next attempt did not recover after a failed restore");
    Expect(!fs::exists(destination + ".backup"),
           "successful recovery left backup file");
}

void TestSuccessfulReplacementCleansBackupAndStaging() {
    const std::string destination = std::string(kRoot) + "/replace-success.png";
    Write(destination, "known-good");
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        CommitVerifiedLessonAssetDownload(staging, destination, kZeroSha256);
    }
    Expect(Read(destination) == "replacement", "replacement was not committed");
    Expect(!fs::exists(destination + ".download"),
           "successful replacement left staging file");
    Expect(!fs::exists(destination + ".backup"),
           "successful replacement left backup file");
}

}  // namespace

int main() {
    fs::remove_all(kRoot);
    fs::create_directories(kRoot);
    TestScopeCleanupOnDownloadException();
    TestMissingDestinationIsACacheMissNotAHashFailure();
    TestScopeCleanupOnEveryShaFailure();
    TestMismatchCleansAndSuccessfulRenameDisarms();
    TestFlushAndFsyncCompleteBeforeReplacement();
    TestReplacementRenameFailureRestoresLastKnownGood();
    TestInterruptedReplacementRecoversOnNextAttempt();
    TestFailedRestoreLeavesTruthfulRecoverableState();
    TestSuccessfulReplacementCleansBackupAndStaging();
    fs::remove_all(kRoot);
    std::cout << "lesson asset staging host tests passed\n";
    return 0;
}
