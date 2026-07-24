#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "lesson_asset_pack_activation.h"

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

}  // namespace

int main() {
    TestFirstActivationWritesPointerWithoutEviction();
    TestCriticalFailureBlocksActivation();
    TestActivationEvictsOnlyPreviousSameLessonAfterPointerSwap();
    TestForeignPreviousPointerDoesNotEvict();
    TestDeletionFailureIsRetryableWithoutRollback();

    std::cout << "lesson asset pack activation host test OK (" << checks
              << " checks)" << std::endl;
    return 0;
}
