#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lesson_asset_cache_evict.h"
#include "lesson_asset_storage_coordinator.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kRoot = "/tmp/tbot-lesson-asset-cache-evict-host";
const std::string kChecksum(64, 'a');
const std::string kKey = "pip-farm-3m/v1-" + kChecksum;
int checks = 0;

void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void ResetRoot() {
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_UNLINK");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_FINAL_STAT");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_LEAF_STAT");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SLUG_RECHECK");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN_ALLOCATION");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_PREDELETE_ALLOCATION");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_ALLOCATION_AFTER_UNLINK");
    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    fs::remove_all(kRoot);
    fs::create_directories(kRoot);
}

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

fs::path Leaf(const std::string& key = kKey) {
    return fs::path(kRoot) / key;
}

void WriteFile(const fs::path& path, const std::string& body = "asset") {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << body;
    output.close();
    Expect(output.good(), "fixture file write must succeed");
}

void ExpectCode(
    const LessonAssetCacheEvictResult& result,
    LessonAssetCacheEvictCode code,
    const char* message
) {
    Expect(result.code == code, message);
}

void TestCanonicalGrammar() {
    const std::vector<std::string> valid = {
        "a/v1-" + kChecksum,
        "pip-farm-3m/v9-" + kChecksum,
        "abc123-def456/v123456789-" + kChecksum,
        std::string(kLessonAssetCacheSlugMaxBytes, 'a') + "/v1-" + kChecksum,
        "a/v" + std::string(kLessonAssetCacheVersionDigitsMax, '1') + "-" + kChecksum,
    };
    for (const auto& key : valid) {
        Expect(IsCanonicalLessonCacheKey(key), "valid canonical key rejected");
    }

    const std::vector<std::string> invalid = {
        "", "a", "/v1-" + kChecksum, "a-/v1-" + kChecksum,
        "-a/v1-" + kChecksum, "a--b/v1-" + kChecksum,
        "a_b/v1-" + kChecksum, "A/v1-" + kChecksum,
        "a/b/v1-" + kChecksum, "a/v0-" + kChecksum,
        "a/v01-" + kChecksum, "a/v-" + kChecksum,
        "a/v1-" + std::string(63, 'a'),
        "a/v1-" + std::string(65, 'a'),
        "a/v1-" + std::string(64, 'A'),
        "a/v1-" + std::string(63, 'a') + "g",
        "a/v1-" + kChecksum + "/x", "a/v1-" + kChecksum + " ",
        " a/v1-" + kChecksum, "a\\v1-" + kChecksum,
        "a/../v1-" + kChecksum, "a/v1-" + kChecksum + "\n",
        "a/v+1-" + kChecksum, "../a/v1-" + kChecksum,
        "file://a/v1-" + kChecksum, "https://a/v1-" + kChecksum,
        "a%2fb/v1-" + kChecksum, "a//v1-" + kChecksum,
        "a.b/v1-" + kChecksum,
        std::string(kLessonAssetCacheSlugMaxBytes + 1, 'a') + "/v1-" + kChecksum,
        "a/v" + std::string(kLessonAssetCacheVersionDigitsMax + 1, '1') + "-" + kChecksum,
    };
    for (const auto& key : invalid) {
        Expect(!IsCanonicalLessonCacheKey(key), "invalid canonical key accepted");
        const auto result = EvictLessonAssetCacheKey(key, false);
        ExpectCode(result, LessonAssetCacheEvictCode::kInvalidCacheKey,
                   "invalid key must return invalid_cache_key");
        Expect(result.cache_key.empty(), "invalid key must not be echoed");
    }

    std::string embedded_nul = "a/v1-" + kChecksum;
    embedded_nul.insert(1, 1, '\0');
    Expect(!IsCanonicalLessonCacheKey(embedded_nul),
           "embedded NUL canonical key must be rejected");
    const auto result = EvictLessonAssetCacheKey(embedded_nul, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kInvalidCacheKey,
               "embedded NUL key must return invalid_cache_key");
    Expect(result.cache_key.empty(), "embedded NUL key must not be echoed");
}

void TestCodeNames() {
    const std::vector<std::pair<LessonAssetCacheEvictCode, const char*>> cases = {
        {LessonAssetCacheEvictCode::kEvicted, "evicted"},
        {LessonAssetCacheEvictCode::kNotFound, "not_found"},
        {LessonAssetCacheEvictCode::kInvalidCacheKey, "invalid_cache_key"},
        {LessonAssetCacheEvictCode::kLessonSessionActive, "lesson_session_active"},
        {LessonAssetCacheEvictCode::kPathMismatch, "path_mismatch"},
        {LessonAssetCacheEvictCode::kNestedDirectory, "nested_directory"},
        {LessonAssetCacheEvictCode::kUnexpectedNodeType, "unexpected_node_type"},
        {LessonAssetCacheEvictCode::kScanFailed, "scan_failed"},
        {LessonAssetCacheEvictCode::kUnlinkFailed, "unlink_failed"},
        {LessonAssetCacheEvictCode::kRmdirFailed, "rmdir_failed"},
        {LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired,
         "partial_evict_recovery_required"},
    };
    for (const auto& [code, name] : cases) {
        Expect(std::string(LessonAssetCacheEvictCodeName(code)) == name,
               "stable code name mismatch");
    }
}

void TestActiveAndAbsent() {
    ResetRoot();
    WriteFile(Leaf() / "one.bin");
    auto result = EvictLessonAssetCacheKey(kKey, true);
    ExpectCode(result, LessonAssetCacheEvictCode::kLessonSessionActive,
               "active lesson must be refused");
    Expect(fs::exists(Leaf() / "one.bin"), "active refusal must not mutate cache");
    Expect(!result.evicted && !result.not_found && result.file_count == 0,
           "active refusal flags must be false and count zero");

    ResetRoot();
    fs::create_directories(Leaf().parent_path());
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kNotFound,
               "absent cache leaf must be idempotent");
    Expect(!result.evicted && result.not_found && result.file_count == 0,
           "not-found flags mismatch");
}

void TestCoordinatorRefusesBeforeScan() {
    ResetRoot();
    WriteFile(Leaf() / "one.bin", "prepared");
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    const auto session =
        coordinator.TryBeginLessonSession("assignment-a", "session-a");
    Expect(session.acquired, "lesson fixture reservation must acquire");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN", "1", 1);
    auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kLessonSessionActive,
               "prepared lesson must refuse before any scan");
    Expect(ReadFile(Leaf() / "one.bin") == "prepared",
           "prepared refusal must preserve the exact leaf");
    Expect(coordinator.EndLessonSession(
               "assignment-a", "session-a", session.generation),
           "lesson fixture reservation must release");

    ResetRoot();
    WriteFile(Leaf() / "one.bin", "syncing");
    auto sync = coordinator.TryBeginMutation("sync");
    Expect(static_cast<bool>(sync), "sync fixture lease must acquire");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN", "1", 1);
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kLessonSessionActive,
               "concurrent mutation must use the stable public refusal");
    Expect(ReadFile(Leaf() / "one.bin") == "syncing",
           "mutation conflict must refuse before any scan");
}

void TestExactFlatLeafDeletionAndPreservation() {
    ResetRoot();
    WriteFile(Leaf() / "one.bin");
    WriteFile(Leaf() / "two.json");
    const fs::path sibling = fs::path(kRoot) / "pip-farm-3m" / ("v2-" + kChecksum);
    WriteFile(sibling / "keep.bin");
    WriteFile(fs::path(kRoot) / "current.json", "current");
    WriteFile(fs::path(kRoot) / "pvg" / "manifest.json", "pvg");
    WriteFile(fs::path(kRoot) / "shared" / "keep.bin", "shared");

    const auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kEvicted, "flat leaf must evict");
    Expect(result.evicted && !result.not_found && result.file_count == 2,
           "evicted flags or file count mismatch");
    Expect(!fs::exists(Leaf()), "exact leaf must be removed");
    Expect(fs::exists(fs::path(kRoot) / "pip-farm-3m"), "slug parent must remain");
    Expect(fs::exists(sibling / "keep.bin"), "sibling version must remain");
    Expect(ReadFile(sibling / "keep.bin") == "asset",
           "sibling version bytes must remain");
    Expect(ReadFile(fs::path(kRoot) / "current.json") == "current",
           "current metadata bytes must remain");
    Expect(ReadFile(fs::path(kRoot) / "pvg" / "manifest.json") == "pvg",
           "PVG directory bytes must remain");
    Expect(ReadFile(fs::path(kRoot) / "shared" / "keep.bin") == "shared",
           "shared asset bytes must remain");
}

void ExpectFirstPassRefusal(
    LessonAssetCacheEvictCode expected,
    const char* message
) {
    const auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, expected, message);
    Expect(!result.evicted && !result.not_found && result.file_count == 0,
           "refusal must not claim success or file count");
    Expect(fs::exists(Leaf() / "safe.bin"), "first-pass refusal must have zero mutation");
}

void TestFirstPassHazards() {
    ResetRoot();
    WriteFile(Leaf() / "safe.bin");
    fs::create_directory(Leaf() / "nested");
    ExpectFirstPassRefusal(LessonAssetCacheEvictCode::kNestedDirectory,
                           "nested directory must be refused");

    ResetRoot();
    WriteFile(Leaf() / "safe.bin");
    Expect(mkfifo((Leaf() / "pipe").c_str(), 0600) == 0, "FIFO fixture must be created");
    ExpectFirstPassRefusal(LessonAssetCacheEvictCode::kUnexpectedNodeType,
                           "FIFO must be refused");

    ResetRoot();
    WriteFile(Leaf() / "safe.bin");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN", "1", 1);
    ExpectFirstPassRefusal(LessonAssetCacheEvictCode::kScanFailed,
                           "deterministic scan failure must be refused");

    ResetRoot();
    WriteFile(Leaf() / "safe.bin");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN_ALLOCATION", "1", 1);
    ExpectFirstPassRefusal(LessonAssetCacheEvictCode::kScanFailed,
                           "scan allocation failure must be stable");
    Expect(LessonAssetCacheEvictOpenDirectoryCountForTest() == 0,
           "scan allocation failure must close its DIR handle");
}

void TestDeterministicSecondPassFailures() {
    ResetRoot();
    WriteFile(Leaf() / "safe.bin");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_UNLINK", "safe.bin", 1);
    auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kUnlinkFailed,
               "deterministic unlink failure must be reported");
    Expect(!result.evicted && result.file_count == 0 && fs::exists(Leaf() / "safe.bin"),
           "unlink failure must not claim success or count");

    ResetRoot();
    fs::create_directories(Leaf());
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR", "1", 1);
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kRmdirFailed,
               "deterministic rmdir failure must be reported");
    Expect(!result.evicted && result.file_count == 0 && fs::exists(Leaf()),
           "rmdir failure must not claim success or count");
}

void TestPartialMutationTruthAndRetry() {
    ResetRoot();
    WriteFile(Leaf() / "a.bin", "a");
    WriteFile(Leaf() / "b.bin", "b");
    WriteFile(Leaf() / "c.bin", "c");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_UNLINK", "b.bin", 1);
    auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired,
               "later unlink failure must report partial recovery");
    Expect(!result.evicted && !result.not_found && result.file_count == 1,
           "later unlink failure must report exact deleted count");
    Expect(!fs::exists(Leaf() / "a.bin") && fs::exists(Leaf() / "b.bin") &&
               fs::exists(Leaf() / "c.bin"),
           "partial unlink must expose the exact remaining files");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_UNLINK");
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kEvicted,
               "retry must finish a partial exact leaf");
    Expect(result.file_count == 2 && !fs::exists(Leaf()),
           "retry must report only files deleted by that attempt");

    ResetRoot();
    WriteFile(Leaf() / "a.bin", "a");
    WriteFile(Leaf() / "b.bin", "b");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR", "1", 1);
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired,
               "rmdir after file deletion must report partial recovery");
    Expect(result.file_count == 2 && fs::exists(Leaf()) && fs::is_empty(Leaf()),
           "rmdir partial must preserve exact deleted count and empty leaf");
    unsetenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR");
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kEvicted,
               "retry must remove an empty partial leaf");
    Expect(result.file_count == 0 && !fs::exists(Leaf()),
           "empty-leaf retry must report zero newly deleted files");
}

void TestAllocationFailureTruth() {
    ResetRoot();
    WriteFile(Leaf() / "a.bin", "a");
    WriteFile(Leaf() / "b.bin", "b");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_PREDELETE_ALLOCATION", "1", 1);
    auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kScanFailed,
               "pre-delete allocation failure must refuse before mutation");
    Expect(result.file_count == 0 && !result.evicted && !result.not_found,
           "pre-delete allocation failure flags must stay cold");
    Expect(ReadFile(Leaf() / "a.bin") == "a" &&
               ReadFile(Leaf() / "b.bin") == "b",
           "pre-delete allocation failure must preserve every file");

    ResetRoot();
    WriteFile(Leaf() / "a.bin", "a");
    WriteFile(Leaf() / "b.bin", "b");
    WriteFile(Leaf() / "c.bin", "c");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_ALLOCATION_AFTER_UNLINK", "1", 1);
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired,
               "post-unlink allocation failure must report partial recovery");
    Expect(result.file_count == 1 && !result.evicted && !result.not_found,
           "post-unlink allocation failure must preserve exact deleted count");
    Expect(!fs::exists(Leaf() / "a.bin") && fs::exists(Leaf() / "b.bin") &&
               fs::exists(Leaf() / "c.bin"),
           "post-unlink allocation failure must expose exact remaining files");
}

void TestFinalAbsenceVerification() {
    ResetRoot();
    WriteFile(Leaf() / "one.bin");
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_FINAL_STAT", "1", 1);
    const auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired,
               "failed final absence proof after deletion must report partial");
    Expect(!result.evicted && !result.not_found && result.file_count == 1,
           "failed final absence proof must never claim eviction");
}

void TestAuthoritativeNotFoundOnly() {
    ResetRoot();
    fs::remove_all(kRoot);
    auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kScanFailed,
               "missing root must not become false not_found");
    Expect(!result.not_found, "missing root must not set notFound");

    ResetRoot();
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kScanFailed,
               "missing slug must not become false not_found");
    Expect(!result.not_found, "missing slug must not set notFound");

    ResetRoot();
    fs::create_directories(Leaf().parent_path());
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kNotFound,
               "exact leaf ENOENT under valid topology must be idempotent");
    Expect(result.not_found, "authoritative exact-leaf ENOENT must set notFound");

    ResetRoot();
    fs::create_directories(Leaf().parent_path());
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SLUG_RECHECK", "1", 1);
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kScanFailed,
               "slug loss around exact-leaf ENOENT must not become not_found");
    Expect(!result.not_found, "slug recheck failure must not set notFound");

    ResetRoot();
    fs::create_directories(Leaf().parent_path());
    setenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_LEAF_STAT", "1", 1);
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kScanFailed,
               "exact leaf SD error must not become false not_found");
    Expect(!result.not_found, "exact leaf SD error must not set notFound");

}

void TestLeafTypeRefusals() {
    ResetRoot();
    WriteFile(Leaf(), "not a directory");
    auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kUnexpectedNodeType,
               "regular leaf must be refused");
    Expect(fs::exists(Leaf()), "regular leaf must remain");

}

}  // namespace

int main() {
    TestCanonicalGrammar();
    TestCodeNames();
    TestActiveAndAbsent();
    TestCoordinatorRefusesBeforeScan();
    TestExactFlatLeafDeletionAndPreservation();
    TestFirstPassHazards();
    TestDeterministicSecondPassFailures();
    TestPartialMutationTruthAndRetry();
    TestAllocationFailureTruth();
    TestFinalAbsenceVerification();
    TestAuthoritativeNotFoundOnly();
    TestLeafTypeRefusals();
    ResetRoot();
    std::cout << "lesson asset cache eviction host test OK (" << checks << " checks)"
              << std::endl;
    return 0;
}
