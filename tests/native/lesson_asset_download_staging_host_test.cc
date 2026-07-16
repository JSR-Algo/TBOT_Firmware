#include "lesson_asset_download_staging.h"
#include "lesson_storage_hil_controller.h"
#include "lesson_storage_hil_hooks.h"

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

constexpr const char* kRoot = "/tmp/tbot-lesson-asset-staging-host";
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

bool SimulateDownloadToStaging(
    LessonAssetDownloadStagingFile& staging,
    const char* cache_key,
    bool has_declared_size,
    std::size_t declared_size,
    const std::string& source,
    std::size_t& bytes_out,
    std::vector<std::size_t>& read_wants
) {
    const std::string tmp = staging.path() + ".tmp";
    fs::remove(tmp);
    bytes_out = 0;
    bool wrote = false;
    try {
        std::ofstream stream(tmp, std::ios::binary);
        while (bytes_out < source.size()) {
            std::size_t want = source.size() - bytes_out;
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
                        throw std::runtime_error("injected local write failure");
                    }
                    continue;
                }
            }
            read_wants.push_back(want);
            const std::size_t count = std::min(want, source.size() - bytes_out);
            if (!wrote) {
                const auto outcome = RunLessonStorageHilCheckpoint(
                    cache_key,
                    LessonStorageHilOperation::kSync,
                    LessonStorageHilCheckpoint::kBeforeDownloadWrite,
                    0,
                    has_declared_size ? static_cast<std::uint32_t>(declared_size) : 0);
                if (outcome != LessonStorageHilHookOutcome::kContinue) {
                    throw std::runtime_error("injected local write failure");
                }
                wrote = true;
            }
            stream.write(source.data() + bytes_out, static_cast<std::streamsize>(count));
            bytes_out += count;
            if (has_declared_size) {
                const auto outcome = RunLessonStorageHilCheckpoint(
                    cache_key,
                    LessonStorageHilOperation::kSync,
                    LessonStorageHilCheckpoint::kAfterDownloadBytes,
                    static_cast<std::uint32_t>(bytes_out),
                    static_cast<std::uint32_t>(declared_size));
                if (outcome != LessonStorageHilHookOutcome::kContinue) {
                    throw std::runtime_error("injected local write failure");
                }
            }
        }
        stream.close();
        fs::rename(tmp, staging.path());
        return true;
    } catch (...) {
        fs::remove(tmp);
        return false;
    }
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
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
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
        CommitVerifiedLessonAssetDownload(
            staging, nullptr, destination, HostSha256("replacement"));
    }
    Expect(Read(destination) == "replacement", "replacement was not committed");
    Expect(!fs::exists(destination + ".download"),
           "successful replacement left staging file");
    Expect(!fs::exists(destination + ".backup"),
           "successful replacement left backup file");
}

void TestBeforeFirstWriteFailuresLeaveNoPartialState() {
    for (const auto action : {
             LessonStorageHilAction::kFail,
             LessonStorageHilAction::kNoSpace,
         }) {
        const std::string destination = std::string(kRoot) + "/before-write.png";
        Write(destination, "known-good");
        ArmSync(LessonStorageHilCheckpoint::kBeforeDownloadWrite, action);
        std::size_t bytes = 999;
        std::vector<std::size_t> wants;
        {
            LessonAssetDownloadStagingFile staging(destination);
            Expect(!SimulateDownloadToStaging(
                       staging, kHilKey, true, 11, "replacement", bytes, wants),
                   "before-write fault did not stop download");
            Expect(bytes == 0, "before-write fault wrote destination bytes");
            Expect(!fs::exists(staging.path()), "before-write fault left .download");
            Expect(!fs::exists(staging.path() + ".tmp"), "before-write fault left .tmp");
        }
        Expect(Read(destination) == "known-good", "before-write fault replaced destination");
        ExpectArmConsumed("before-write fault did not consume arm");
    }
}

void TestAfterBytesCapsReadExactlyAndNoSpaceCleans() {
    const std::string destination = std::string(kRoot) + "/after-bytes.png";
    Write(destination, "known-good");
    ArmSync(LessonStorageHilCheckpoint::kAfterDownloadBytes,
            LessonStorageHilAction::kNoSpace, 5, 11);
    std::size_t bytes = 0;
    std::vector<std::size_t> wants;
    {
        LessonAssetDownloadStagingFile staging(destination);
        Expect(!SimulateDownloadToStaging(
                   staging, kHilKey, true, 11, "replacement", bytes, wants),
               "after-bytes no-space did not stop download");
        Expect(bytes == 5, "after-bytes progress overshot exact threshold");
        Expect(wants.size() == 1 && wants.front() == 5,
               "HTTP read was not capped to exact threshold");
        Expect(!fs::exists(staging.path()), "after-bytes fault left .download");
        Expect(!fs::exists(staging.path() + ".tmp"), "after-bytes fault left .tmp");
    }
    Expect(Read(destination) == "known-good", "after-bytes fault replaced destination");
    ExpectArmConsumed("after-bytes fault did not consume arm");
}

void TestAfterBytesRequiresExactDeclaredSize() {
    for (const auto declared : {std::size_t{0}, std::size_t{12}}) {
        ArmSync(LessonStorageHilCheckpoint::kAfterDownloadBytes,
                LessonStorageHilAction::kFail, 5, 11);
        const bool has_declared_size = declared != 0;
        const std::string destination = std::string(kRoot) + "/declared-" +
                                        std::to_string(declared) + ".png";
        std::size_t bytes = 0;
        std::vector<std::size_t> wants;
        {
            LessonAssetDownloadStagingFile staging(destination);
            Expect(SimulateDownloadToStaging(
                       staging, kHilKey, has_declared_size, declared,
                       "replacement", bytes, wants),
                   "missing/mismatched declared size affected download");
        }
        const auto status = LessonStorageHilController::GetInstance().Status();
        Expect(status.armed && !status.reached && !status.consumed,
               "missing/mismatched declared size consumed arm");
        LessonStorageHilController::GetInstance().Reset();
    }
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
    TestSuccessfulReplacementCleansBackupAndStaging();
    TestBeforeFirstWriteFailuresLeaveNoPartialState();
    TestAfterBytesCapsReadExactlyAndNoSpaceCleans();
    TestAfterBytesRequiresExactDeclaredSize();
    TestCorruptStagingUsesNormalChecksumFailure();
    TestPreCommitFaultRestoresOldDestination();
    TestSampleContextCannotConsumeCanonicalArm();
    fs::remove_all(kRoot);
    std::cout << "lesson asset staging host tests passed\n";
    return 0;
}
