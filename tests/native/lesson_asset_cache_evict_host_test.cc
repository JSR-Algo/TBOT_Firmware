#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lesson_asset_cache_evict.h"

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
    fs::remove_all(kRoot);
    fs::create_directories(kRoot);
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
    result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kNotFound,
               "absent cache leaf must be idempotent");
    Expect(!result.evicted && result.not_found && result.file_count == 0,
           "not-found flags mismatch");
}

void TestExactFlatLeafDeletionAndPreservation() {
    ResetRoot();
    WriteFile(Leaf() / "one.bin");
    WriteFile(Leaf() / "two.json");
    const fs::path sibling = fs::path(kRoot) / "pip-farm-3m" / ("v2-" + kChecksum);
    WriteFile(sibling / "keep.bin");
    WriteFile(fs::path(kRoot) / "current.json", "current");
    WriteFile(fs::path(kRoot) / "previous-good-version.json", "pvg");
    WriteFile(fs::path(kRoot) / "shared" / "keep.bin", "shared");

    const auto result = EvictLessonAssetCacheKey(kKey, false);
    ExpectCode(result, LessonAssetCacheEvictCode::kEvicted, "flat leaf must evict");
    Expect(result.evicted && !result.not_found && result.file_count == 2,
           "evicted flags or file count mismatch");
    Expect(!fs::exists(Leaf()), "exact leaf must be removed");
    Expect(fs::exists(fs::path(kRoot) / "pip-farm-3m"), "slug parent must remain");
    Expect(fs::exists(sibling / "keep.bin"), "sibling version must remain");
    Expect(fs::exists(fs::path(kRoot) / "current.json"), "current metadata must remain");
    Expect(fs::exists(fs::path(kRoot) / "previous-good-version.json"),
           "PVG metadata must remain");
    Expect(fs::exists(fs::path(kRoot) / "shared" / "keep.bin"),
           "shared assets must remain");
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
    TestExactFlatLeafDeletionAndPreservation();
    TestFirstPassHazards();
    TestDeterministicSecondPassFailures();
    TestLeafTypeRefusals();
    ResetRoot();
    std::cout << "lesson asset cache eviction host test OK (" << checks << " checks)"
              << std::endl;
    return 0;
}
