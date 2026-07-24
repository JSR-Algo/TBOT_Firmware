#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "lesson_asset_pack_activation.h"
#include "lesson_asset_storage_coordinator.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kRoot = "/tmp/tbot-lesson-asset-pack-activation-host";
const std::string kChecksumA(64, 'a');
const std::string kChecksumB(64, 'b');
const std::string kLessonId = "pip-farm";
const std::string kKeyV1 = kLessonId + "/v1-" + kChecksumA;
const std::string kKeyV2 = kLessonId + "/v2-" + kChecksumB;
const std::string kForeignKey = "other-lesson/v1-" + kChecksumA;
int checks = 0;

void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void ResetRoot() {
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR");
    SetLessonAssetPackActivationFsTestMode(
        LessonAssetPackActivationFsTestMode::kNone);
    SetLessonAssetPackActivationFsTestFailure(
        LessonAssetPackActivationFsTestFailure::kNone);
    fs::remove_all(kRoot);
    fs::create_directories(kRoot);
}

fs::path LessonRoot(const std::string& lesson_id = kLessonId) {
    return fs::path(kRoot) / lesson_id;
}

fs::path ActivePath(const std::string& lesson_id = kLessonId) {
    return LessonRoot(lesson_id) / "active.json";
}

fs::path CacheLeaf(const std::string& cache_key) {
    return fs::path(kRoot) / cache_key;
}

void WriteFile(const fs::path& path, const std::string& body = "asset") {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << body;
    output.close();
    Expect(output.good(), "fixture file write must succeed");
}

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void ExpectActivePointer(
    const std::string& cache_key,
    const std::string& checksum,
    const char* message
) {
    const std::string body = ReadFile(ActivePath());
    Expect(body.find("\"lessonId\":\"" + kLessonId + "\"") != std::string::npos,
           message);
    Expect(body.find("\"cacheKey\":\"" + cache_key + "\"") != std::string::npos,
           message);
    Expect(body.find("\"manifestChecksum\":\"" + checksum + "\"") !=
               std::string::npos,
           message);
    Expect(body.find(".tmp") == std::string::npos,
           "active pointer must contain only stable pointer fields");
}

void TestFirstActivationWritesPointerWithoutEviction() {
    ResetRoot();
    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV1, kChecksumA, true);
    Expect(result.activated, "verified pack must activate");
    Expect(!result.previous_evicted, "first activation has no previous pack");
    Expect(result.previous_cache_key.empty(), "first activation has no previous key");
    Expect(result.error_code.empty(), "successful first activation has no error code");
    ExpectActivePointer(kKeyV1, kChecksumA, "active pointer must be written");
    Expect(!fs::exists(ActivePath().string() + ".tmp"),
           "activation temp file must be removed after rename");
}

void TestCriticalFailureBlocksActivation() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, false);
    Expect(!result.activated, "critical verification failure must block activation");
    Expect(!result.previous_evicted, "blocked activation must not evict");
    Expect(result.previous_cache_key.empty(), "blocked activation need not expose previous");
    Expect(result.error_code == "critical_assets_unverified",
           "blocked activation must be machine-readable");
    ExpectActivePointer(kKeyV1, kChecksumA, "blocked activation must preserve active file");
}

void TestActivationEvictsOnlyPreviousSameLessonAfterPointerSwap() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    WriteFile(CacheLeaf(kKeyV2) / "new.bin");
    WriteFile(CacheLeaf(kForeignKey) / "foreign.bin");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated, "replacement pack must activate");
    Expect(result.previous_evicted, "previous same-lesson key must be evicted");
    Expect(result.previous_cache_key == kKeyV1,
           "previous cache key must be reported exactly");
    Expect(result.error_code.empty(), "successful eviction has no error code");
    ExpectActivePointer(kKeyV2, kChecksumB, "pointer must update to replacement");
    Expect(!fs::exists(CacheLeaf(kKeyV1)), "only previous same-lesson leaf is removed");
    Expect(fs::exists(CacheLeaf(kKeyV2) / "new.bin"), "new cache leaf must remain");
    Expect(fs::exists(CacheLeaf(kForeignKey) / "foreign.bin"),
           "foreign lesson leaf must never be evicted");
}

void TestExistingActiveUpgradeWorksWithFatFsNoOverwriteRename() {
    ResetRoot();
    SetLessonAssetPackActivationFsTestMode(
        LessonAssetPackActivationFsTestMode::kFatFsNoOverwriteRename);
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    WriteFile(CacheLeaf(kKeyV2) / "new.bin");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated,
           "FatFS no-overwrite active upgrade must still activate");
    ExpectActivePointer(kKeyV2, kChecksumB,
                        "FatFS no-overwrite upgrade must promote new active");
    Expect(result.previous_evicted,
           "successful FatFS upgrade must evict previous pack after promotion");
    Expect(!fs::exists(ActivePath().string() + ".tmp"),
           "successful FatFS upgrade left active tmp");
    Expect(!fs::exists(ActivePath().string() + ".backup"),
           "successful FatFS upgrade left active backup");
}

void TestForeignPreviousPointerDoesNotEvict() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"other-lesson\",\"cacheKey\":\"" +
                                kForeignKey + "\",\"manifestChecksum\":\"" +
                                kChecksumA + "\"}");
    WriteFile(CacheLeaf(kForeignKey) / "foreign.bin");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated, "pack should activate over foreign stale pointer");
    Expect(!result.previous_evicted, "foreign previous pack must not be evicted");
    Expect(result.previous_cache_key == kForeignKey,
           "foreign previous key can be reported for retry context");
    Expect(result.error_code.empty(), "foreign non-eviction is not an error");
    Expect(fs::exists(CacheLeaf(kForeignKey) / "foreign.bin"),
           "foreign previous cache leaf must remain");
    ExpectActivePointer(kKeyV2, kChecksumB, "pointer must still update");
}

void TestInterruptedAfterActiveBackupRecoversOldPointerOnRestart() {
    ResetRoot();
    SetLessonAssetPackActivationFsTestMode(
        LessonAssetPackActivationFsTestMode::kFatFsNoOverwriteRename);
    SetLessonAssetPackActivationFsTestFailure(
        LessonAssetPackActivationFsTestFailure::kInterruptAfterActiveBackup);
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    WriteFile(CacheLeaf(kKeyV2) / "new.bin");

    const auto interrupted =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(!interrupted.activated,
           "interruption before tmp promotion must not report activation");
    Expect(fs::exists(ActivePath().string() + ".backup"),
           "interruption after active backup must preserve old active backup");
    Expect(fs::exists(ActivePath().string() + ".tmp"),
           "interruption after active backup must preserve verified tmp");
    Expect(fs::exists(CacheLeaf(kKeyV1) / "old.bin"),
           "interrupted activation must not evict previous pack");

    SetLessonAssetPackActivationFsTestFailure(
        LessonAssetPackActivationFsTestFailure::kNone);
    SetLessonAssetPackActivationFsTestMode(
        LessonAssetPackActivationFsTestMode::kFatFsNoOverwriteRename);
    const auto recovered =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(recovered.activated, "restart must recover and activate new pointer");
    ExpectActivePointer(kKeyV2, kChecksumB,
                        "restart after backup interruption must activate new pointer");
    Expect(!fs::exists(ActivePath().string() + ".backup"),
           "restart recovery left backup");
    Expect(!fs::exists(ActivePath().string() + ".tmp"),
           "restart recovery left tmp");
}

void TestTmpPromotionFailureRestoresOldPointerAndDefersEviction() {
    ResetRoot();
    SetLessonAssetPackActivationFsTestMode(
        LessonAssetPackActivationFsTestMode::kFatFsNoOverwriteRename);
    SetLessonAssetPackActivationFsTestFailure(
        LessonAssetPackActivationFsTestFailure::kTmpToActiveRename);
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    WriteFile(CacheLeaf(kKeyV2) / "new.bin");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(!result.activated, "failed tmp promotion must not report activation");
    ExpectActivePointer(kKeyV1, kChecksumA,
                        "failed tmp promotion must restore old active pointer");
    Expect(fs::exists(CacheLeaf(kKeyV1) / "old.bin"),
           "failed tmp promotion must not evict previous pack");
}

void TestStaleTmpAndBackupCleanupKeepsTruthfulActivePointer() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(ActivePath().string() + ".backup",
              "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                  "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(ActivePath().string() + ".tmp",
              "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV2 +
                  "\",\"manifestChecksum\":\"" + kChecksumB + "\"}");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated, "stale tmp/backup cleanup must permit activation");
    ExpectActivePointer(kKeyV2, kChecksumB,
                        "stale cleanup must leave the new truthful active pointer");
    Expect(!fs::exists(ActivePath().string() + ".backup"),
           "stale cleanup left backup");
    Expect(!fs::exists(ActivePath().string() + ".tmp"),
           "stale cleanup left tmp");
}

void TestDeletionFailureIsRetryableWithoutRollback() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR", "1", 1);

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated,
           "eviction failure must not roll back successful activation");
    Expect(!result.previous_evicted, "failed deletion is not evicted");
    Expect(result.previous_cache_key == kKeyV1,
           "retryable failure must return the previous cache key");
    Expect(result.error_code == "previous_evict_retryable",
           "deletion failure must be retryable");
    ExpectActivePointer(kKeyV2, kChecksumB,
                        "pointer must update before old-pack deletion");
    Expect(fs::exists(CacheLeaf(kKeyV1)), "failed deletion leaves retry target");
}

void TestPostPromotionBackupRetryEvictsPreviousAndRemovesMarker() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV2 +
                                "\",\"manifestChecksum\":\"" + kChecksumB + "\"}");
    WriteFile(ActivePath().string() + ".backup",
              "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                  "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    WriteFile(CacheLeaf(kKeyV2) / "new.bin");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated, "post-promotion retry must keep active new pointer");
    Expect(result.previous_cache_key == kKeyV1,
           "post-promotion retry must read previous key from backup marker");
    Expect(result.previous_evicted,
           "post-promotion retry must evict pending previous pack");
    Expect(!fs::exists(ActivePath().string() + ".backup"),
           "successful pending eviction must remove backup marker");
    Expect(!fs::exists(CacheLeaf(kKeyV1)),
           "post-promotion retry left previous pack");
    ExpectActivePointer(kKeyV2, kChecksumB,
                        "post-promotion retry changed active pointer");

    const auto idempotent =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(idempotent.activated, "idempotent retry must remain activated");
    Expect(idempotent.error_code.empty(), "idempotent retry must be clean");
    Expect(!fs::exists(ActivePath().string() + ".backup"),
           "idempotent retry recreated backup marker");
}

void TestPendingEvictionFailureKeepsBackupMarkerForRetry() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV2 +
                                "\",\"manifestChecksum\":\"" + kChecksumB + "\"}");
    WriteFile(ActivePath().string() + ".backup",
              "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                  "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR", "1", 1);

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated,
           "pending eviction failure must not undo active new pointer");
    Expect(!result.previous_evicted, "failed pending eviction cannot report evicted");
    Expect(result.error_code == "previous_evict_retryable",
           "pending eviction failure must be retryable");
    Expect(fs::exists(ActivePath().string() + ".backup"),
           "pending eviction failure must keep backup marker");
    Expect(fs::exists(CacheLeaf(kKeyV1)),
           "pending eviction failure must keep previous pack for retry");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR");

    const auto retry =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(retry.previous_evicted, "retry must finish pending eviction");
    Expect(!fs::exists(ActivePath().string() + ".backup"),
           "retry did not remove backup marker");
}

void TestBackupCleanupFailureKeepsMarkerWithoutWriteFailure() {
    ResetRoot();
    SetLessonAssetPackActivationFsTestFailure(
        LessonAssetPackActivationFsTestFailure::kBackupCleanupRemove);
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV2 +
                                "\",\"manifestChecksum\":\"" + kChecksumB + "\"}");
    WriteFile(ActivePath().string() + ".backup",
              "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV2 +
                  "\",\"manifestChecksum\":\"" + kChecksumB + "\"}");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated,
           "backup cleanup failure must not report activation write failure");
    Expect(result.error_code == "previous_evict_retryable",
           "backup cleanup failure must be retryable cleanup state");
    Expect(fs::exists(ActivePath().string() + ".backup"),
           "backup cleanup failure must keep marker");
}

void TestUnrelatedBackupMarkerDoesNotEvictForeignPack() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV2 +
                                "\",\"manifestChecksum\":\"" + kChecksumB + "\"}");
    WriteFile(ActivePath().string() + ".backup",
              "{\"lessonId\":\"other-lesson\",\"cacheKey\":\"" + kForeignKey +
                  "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kForeignKey) / "foreign.bin");

    const auto result =
        ActivateLessonAssetPack(kLessonId, kKeyV2, kChecksumB, true);
    Expect(result.activated, "unrelated backup marker must not block active pointer");
    Expect(!result.previous_evicted, "unrelated backup marker must not evict");
    Expect(fs::exists(CacheLeaf(kForeignKey) / "foreign.bin"),
           "unrelated backup marker evicted foreign pack");
    Expect(!fs::exists(ActivePath().string() + ".backup"),
           "unrelated backup marker should be cleaned after no-eviction decision");
}

void TestSyncMutationLeaseCoversActivePointerSwap() {
    ResetRoot();
    WriteFile(ActivePath(), "{\"lessonId\":\"pip-farm\",\"cacheKey\":\"" + kKeyV1 +
                                "\",\"manifestChecksum\":\"" + kChecksumA + "\"}");
    WriteFile(CacheLeaf(kKeyV1) / "old.bin");
    WriteFile(CacheLeaf(kKeyV2) / "new.bin");

    LessonAssetPackActivationResult activation{
        false, false, std::string(), std::string()};
    {
        auto mutation =
            LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        Expect(static_cast<bool>(mutation), "sync mutation fixture must acquire");
        const auto interleaved =
            LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
                "assignment-during-sync", "session-during-sync");
        Expect(interleaved.code == LessonAssetReservationCode::kMutationActive,
               "lesson session interleaved before active pointer swap");

        activation =
            ActivateLessonAssetPack(mutation, kLessonId, kKeyV2, kChecksumB, true);
        Expect(activation.activated, "lease-aware activation must swap active pointer");
        ExpectActivePointer(kKeyV2, kChecksumB,
                            "lease-aware activation must write active pointer");
        Expect(!activation.previous_evicted,
               "lease-aware activation must not evict while sync lease is held");
    }

    EvictPreviousLessonAssetPackAfterActivation(activation, kLessonId, kKeyV2);
    Expect(activation.previous_evicted,
           "post-activation eviction must run after sync lease release");
    Expect(!fs::exists(CacheLeaf(kKeyV1)),
           "post-activation eviction must remove previous pack");
    Expect(fs::exists(CacheLeaf(kKeyV2) / "new.bin"),
           "post-activation eviction must preserve active pack");
}

}  // namespace

int main() {
    TestFirstActivationWritesPointerWithoutEviction();
    TestCriticalFailureBlocksActivation();
    TestActivationEvictsOnlyPreviousSameLessonAfterPointerSwap();
    TestExistingActiveUpgradeWorksWithFatFsNoOverwriteRename();
    TestForeignPreviousPointerDoesNotEvict();
    TestInterruptedAfterActiveBackupRecoversOldPointerOnRestart();
    TestTmpPromotionFailureRestoresOldPointerAndDefersEviction();
    TestStaleTmpAndBackupCleanupKeepsTruthfulActivePointer();
    TestDeletionFailureIsRetryableWithoutRollback();
    TestPostPromotionBackupRetryEvictsPreviousAndRemovesMarker();
    TestPendingEvictionFailureKeepsBackupMarkerForRetry();
    TestBackupCleanupFailureKeepsMarkerWithoutWriteFailure();
    TestUnrelatedBackupMarkerDoesNotEvictForeignPack();
    TestSyncMutationLeaseCoversActivePointerSwap();

    std::cout << "lesson asset pack activation host test OK (" << checks
              << " checks)" << std::endl;
    return 0;
}
