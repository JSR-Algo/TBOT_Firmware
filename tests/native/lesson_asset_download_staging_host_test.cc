#include "lesson_asset_download_staging.h"
#include "lesson_storage_hil_controller.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef TBOT_LESSON_ASSET_STAGING_TEST_ROOT
#define TBOT_LESSON_ASSET_STAGING_TEST_ROOT "/tmp/tbot-lesson-asset-staging-host"
#endif
constexpr const char* kRoot = TBOT_LESSON_ASSET_STAGING_TEST_ROOT;
constexpr const char* kZeroSha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kHilKey =
    "hil-task14/v1-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

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

std::string HostSha256(const std::string& bytes) {
    unsigned char digest[32] = {};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        digest[index % 32] ^= static_cast<unsigned char>(bytes[index]);
    }
    char hex[65];
    for (std::size_t index = 0; index < 32; ++index) {
        std::snprintf(hex + index * 2, sizeof(hex) - index * 2, "%02x", digest[index]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

void ArmSync(
    LessonStorageHilCheckpoint checkpoint,
    LessonStorageHilAction action,
    std::uint32_t threshold = 0,
    std::uint32_t declared_asset_bytes = 0
) {
    auto& controller = LessonStorageHilController::GetInstance();
    controller.Reset();
    const auto result = controller.Arm({
        kHilKey,
        LessonStorageHilOperation::kSync,
        checkpoint,
        action,
        threshold,
        declared_asset_bytes,
        0,
    });
    Expect(result.code == LessonStorageHilArmCode::kArmed, "sync fault did not arm");
}

void ExpectArmConsumed(const char* message) {
    const auto status = LessonStorageHilController::GetInstance().Status();
    Expect(status.reached && status.consumed && !status.armed, message);
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
            CommitVerifiedLessonAssetDownload(
                staging, nullptr, destination, HostSha256("asset"));
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
            staging, nullptr, mismatch_destination, std::string(64, 'f'));
    } catch (const std::runtime_error&) {
    }
    Expect(!fs::exists(mismatch_destination + ".download"),
           "checksum mismatch left staging file");
    Expect(!fs::exists(mismatch_destination), "checksum mismatch committed destination");

    const std::string destination = std::string(kRoot) + "/committed.png";
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "committed");
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("committed"));
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
            CommitVerifiedLessonAssetDownload(
                staging, nullptr, destination, HostSha256("replacement"));
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
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
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
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("interrupted-replacement"));
    } catch (const std::runtime_error&) {
    }
    SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure::kNone);
    // A crash right after the destination was moved aside to `.backup` (but
    // before the verified staging file was promoted) leaves the destination
    // path vacated and the last-known-good bytes parked in `.backup`. That is
    // the truthful on-disk state after a real power loss at that point.
    Expect(!fs::exists(destination),
           "interruption seam left destination present alongside backup");
    Expect(fs::exists(backup_path),
           "interruption seam did not preserve last-known-good as backup");
    Expect(Read(backup_path) == "known-good",
           "interruption seam corrupted preserved backup content");
    Expect(!fs::exists(staging_path), "interruption seam left staged file");

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
    const std::string backup_path = destination + ".backup";
    Write(destination, "known-good");
    bool threw = false;
    try {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        SetLessonAssetStagingFsTestFailure(
            LessonAssetStagingFsTestFailure::kRestoreRename);
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure::kNone);
    Expect(threw, "failed restore did not throw");
    // When both promotion and the best-effort restore fail, the code must not
    // fabricate success: destination stays vacated and the last-known-good
    // bytes remain recoverable from `.backup` on the next attempt.
    Expect(!fs::exists(destination),
           "failed restore falsely reported a readable destination");
    Expect(fs::exists(backup_path),
           "failed restore lost the last-known-good backup");
    Expect(Read(backup_path) == "known-good",
           "failed restore corrupted the last-known-good backup");
    Expect(!fs::exists(destination + ".download"),
           "failed restore left unverified staging state");

    LessonAssetDownloadStagingFile next_attempt(destination);
    Expect(Read(destination) == "known-good",
           "next attempt did not recover after a failed restore");
    Expect(!fs::exists(backup_path),
           "successful recovery left backup file");
}

void TestExistingDestinationReplacedUnderFatFsNoOverwriteRename() {
    const std::string destination = std::string(kRoot) + "/fatfs-no-overwrite.png";
    Write(destination, "corrupt-or-stale");
    SetLessonAssetStagingFsTestMode(
        LessonAssetStagingFsTestMode::kFatFsNoOverwriteRename);
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
    }
    SetLessonAssetStagingFsTestMode(LessonAssetStagingFsTestMode::kNone);
    Expect(Read(destination) == "replacement",
           "FATFS no-overwrite rename must still replace an existing destination");
    Expect(!fs::exists(destination + ".download"),
           "FATFS no-overwrite replacement left staging file");
    Expect(!fs::exists(destination + ".backup"),
           "FATFS no-overwrite replacement left backup file");
}

void TestMissingDestinationCommitsUnderFatFsNoOverwriteRename() {
    const std::string destination = std::string(kRoot) + "/fatfs-missing.png";
    SetLessonAssetStagingFsTestMode(
        LessonAssetStagingFsTestMode::kFatFsNoOverwriteRename);
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "first-download");
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("first-download"));
    }
    SetLessonAssetStagingFsTestMode(LessonAssetStagingFsTestMode::kNone);
    Expect(Read(destination) == "first-download",
           "first download must commit under FATFS no-overwrite rename");
    Expect(!fs::exists(destination + ".backup"),
           "first download under FATFS no-overwrite rename created a backup");
}

void TestRepeatedRetriesEventuallySucceedUnderFatFsNoOverwriteRename() {
    const std::string destination = std::string(kRoot) + "/fatfs-retry.png";
    const std::string backup_path = destination + ".backup";
    Write(destination, "corrupt-attempt-0");
    SetLessonAssetStagingFsTestMode(
        LessonAssetStagingFsTestMode::kFatFsNoOverwriteRename);

    try {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        SetLessonAssetStagingFsTestFailure(
            LessonAssetStagingFsTestFailure::kInterruptAfterBackupRename);
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
    } catch (const std::runtime_error&) {
    }
    SetLessonAssetStagingFsTestFailure(LessonAssetStagingFsTestFailure::kNone);
    Expect(!fs::exists(destination),
           "retry fixture: crash must leave destination vacated");
    Expect(fs::exists(backup_path), "retry fixture: crash must preserve backup");

    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(Read(destination) == "corrupt-attempt-0",
               "retry did not recover last-known-good before replacing");
        Write(staging.path(), "replacement");
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
    }
    SetLessonAssetStagingFsTestMode(LessonAssetStagingFsTestMode::kNone);
    Expect(Read(destination) == "replacement",
           "retry under FATFS no-overwrite rename did not eventually replace destination");
    Expect(!fs::exists(backup_path), "retry left backup file");
    Expect(!fs::exists(destination + ".download"), "retry left staging file");
}

void TestSuccessfulReplacementCleansBackupAndStaging() {
    const std::string destination = std::string(kRoot) + "/replace-success.png";
    Write(destination, "known-good");
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
    }
    Expect(Read(destination) == "replacement", "replacement was not committed");
    Expect(!fs::exists(destination + ".download"),
           "successful replacement left staging file");
    Expect(!fs::exists(destination + ".backup"),
           "successful replacement left backup file");
}

void TestCorruptStagingUsesNormalChecksumFailure() {
    const std::string destination = std::string(kRoot) + "/corrupt.png";
    Write(destination, "known-good");
    ArmSync(LessonStorageHilCheckpoint::kBeforeChecksumVerify,
            LessonStorageHilAction::kCorruptStaging);
    bool checksum_mismatch = false;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "replacement");
        try {
            CommitVerifiedLessonAssetDownload(
                staging, kHilKey, destination, HostSha256("replacement"));
        } catch (const std::runtime_error& error) {
            checksum_mismatch =
                std::string(error.what()) == "lesson asset checksum mismatch";
        }
        const std::string corrupted = Read(staging.path());
        Expect(corrupted.size() == std::string("replacement").size(),
               "corrupt staging changed file length");
        std::size_t differences = 0;
        for (std::size_t index = 0; index < corrupted.size(); ++index) {
            differences += corrupted[index] != std::string("replacement")[index];
        }
        Expect(differences == 1 &&
                   static_cast<unsigned char>(corrupted.front()) ==
                       (static_cast<unsigned char>('r') ^ 0x01U),
               "corrupt staging did not flip exactly one bounded byte");
    }
    Expect(checksum_mismatch, "corrupt staging did not use normal checksum mismatch");
    Expect(Read(destination) == "known-good", "corrupt staging replaced destination");
    Expect(!fs::exists(destination + ".download"), "corrupt staging left .download");
    ExpectArmConsumed("corrupt staging did not consume arm");
}

void TestPreCommitFaultRestoresOldDestination() {
    for (const auto action : {
             LessonStorageHilAction::kFail,
             LessonStorageHilAction::kNoSpace,
         }) {
        const std::string destination = std::string(kRoot) + "/pre-commit.png";
        Write(destination, "known-good");
        ArmSync(LessonStorageHilCheckpoint::kBeforeCommitRename, action);
        bool threw = false;
        try {
            LessonAssetDownloadStagingFile staging(destination);
            Write(staging.path(), "replacement");
            CommitVerifiedLessonAssetDownload(
                staging, kHilKey, destination, HostSha256("replacement"));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        Expect(threw, "pre-commit fault did not fail");
        Expect(Read(destination) == "known-good", "pre-commit fault did not restore old file");
        Expect(!fs::exists(destination + ".backup"), "pre-commit fault left backup");
        Expect(!fs::exists(destination + ".download"), "pre-commit fault left staging");
        ExpectArmConsumed("pre-commit fault did not consume arm");
    }
}

void TestSampleContextCannotConsumeCanonicalArm() {
    const std::string destination = std::string(kRoot) + "/sample.png";
    ArmSync(LessonStorageHilCheckpoint::kBeforeChecksumVerify,
            LessonStorageHilAction::kFail);
    {
        LessonAssetDownloadStagingFile staging(destination);
        Write(staging.path(), "sample");
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("sample"));
    }
    const auto status = LessonStorageHilController::GetInstance().Status();
    Expect(status.armed && !status.reached && !status.consumed,
           "sample empty HIL context consumed canonical arm");
    LessonStorageHilController::GetInstance().Reset();
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
    TestExistingDestinationReplacedUnderFatFsNoOverwriteRename();
    TestMissingDestinationCommitsUnderFatFsNoOverwriteRename();
    TestRepeatedRetriesEventuallySucceedUnderFatFsNoOverwriteRename();
    TestSuccessfulReplacementCleansBackupAndStaging();
    TestCorruptStagingUsesNormalChecksumFailure();
    TestPreCommitFaultRestoresOldDestination();
    TestSampleContextCannotConsumeCanonicalArm();
    fs::remove_all(kRoot);
    std::cout << "lesson asset staging host tests passed\n";
    return 0;
}
