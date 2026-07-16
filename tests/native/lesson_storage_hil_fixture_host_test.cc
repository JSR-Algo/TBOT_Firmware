#include "lesson_storage_hil_fixture.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_mkdir_calls = 0;
int g_unlink_calls = 0;
int g_rmdir_calls = 0;
int g_fail_mkdir_call = 0;
int g_fail_unlink_call = 0;
int g_fail_rmdir_call = 0;

int InjectedMkdir(const char* path) {
    ++g_mkdir_calls;
    if (g_mkdir_calls == g_fail_mkdir_call) {
        errno = EIO;
        return -1;
    }
    return mkdir(path, 0755);
}

int InjectedUnlink(const char* path) {
    ++g_unlink_calls;
    if (g_unlink_calls == g_fail_unlink_call) {
        errno = EIO;
        return -1;
    }
    return unlink(path);
}

int InjectedRmdir(const char* path) {
    ++g_rmdir_calls;
    if (g_rmdir_calls == g_fail_rmdir_call) {
        errno = EIO;
        return -1;
    }
    return rmdir(path);
}

void ResetMutationInjection() {
    g_mkdir_calls = 0;
    g_unlink_calls = 0;
    g_rmdir_calls = 0;
    g_fail_mkdir_call = 0;
    g_fail_unlink_call = 0;
    g_fail_rmdir_call = 0;
    SetLessonStorageHilFixtureMkdirCallbackForTest(nullptr);
    SetLessonStorageHilFixtureUnlinkCallbackForTest(nullptr);
    SetLessonStorageHilFixtureRmdirCallbackForTest(nullptr);
}

void Expect(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string Key(const std::string& slug, int version, char digest) {
    return slug + "/v" + std::to_string(version) + "-" + std::string(64, digest);
}

fs::path Root() { return TBOT_LESSON_STORAGE_HIL_ROOT; }
fs::path Leaf(const std::string& key) { return Root() / fs::path(key); }

void ResetRoot() {
    std::error_code error;
    fs::remove_all(Root(), error);
    Expect(!error, "test root reset failed");
}

void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    Expect(output.good(), "test file write failed");
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

LessonAssetMutationLease Lease() {
    return LessonAssetStorageCoordinator::GetInstance().TryBeginMutation(
        "lesson-storage-hil-fixture-test"
    );
}

const LessonStorageHilInspectionEntry* FindEntry(
    const LessonStorageHilInspection& inspection,
    const std::string& label
) {
    for (const auto& entry : inspection.entries) {
        if (entry.label == label) {
            return &entry;
        }
    }
    return nullptr;
}

void ExpectNoAbsolutePaths(const LessonStorageHilFixtureResult& result) {
    Expect(result.cache_key.find("/sdcard") == std::string::npos,
           "result leaked fixed root");
    Expect(result.sibling_cache_key.find("/sdcard") == std::string::npos,
           "sibling result leaked fixed root");
}

void TestValidationAndLeaseRefusalPrecedeFilesystemAccess() {
    ResetRoot();
    const std::string valid = Key("hil-child", 1, 'a');
    auto lease = Lease();
    Expect(static_cast<bool>(lease), "fixture lease was not acquired");

    const std::vector<std::string> invalid = {
        Key("child", 1, 'a'),
        "hil-child/../v1-" + std::string(64, 'a'),
        std::string("hil-child/v1-") + std::string(32, 'a') + '\0' +
            std::string(31, 'a'),
        Key("hil-" + std::string(128, 'a'), 1, 'a'),
    };
    for (const auto& key : invalid) {
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kInvalidCacheKey,
               "invalid or non-HIL key was accepted");
        Expect(result.cache_key.empty() && result.sibling_cache_key.empty(),
               "invalid input was echoed");
        Expect(!fs::exists(Root()), "validation touched the filesystem");
    }

    const auto bad_extra = StageLessonStorageHilFixture(
        lease,
        valid,
        LessonStorageHilFixture::kNestedDirectory,
        Key("hil-child", 2, 'b')
    );
    Expect(bad_extra.code == LessonStorageHilFixtureCode::kInvalidSibling,
           "non-preservation fixture accepted sibling");
    const auto bad_slug = StageLessonStorageHilFixture(
        lease,
        valid,
        LessonStorageHilFixture::kPreservationSet,
        Key("hil-other", 2, 'b')
    );
    Expect(bad_slug.code == LessonStorageHilFixtureCode::kInvalidSibling,
           "preservation sibling with another slug was accepted");
    Expect(bad_slug.sibling_cache_key.empty(), "invalid sibling was echoed");
    const auto bad_same = StageLessonStorageHilFixture(
        lease, valid, LessonStorageHilFixture::kPreservationSet, valid
    );
    Expect(bad_same.code == LessonStorageHilFixtureCode::kInvalidSibling,
           "identical preservation sibling was accepted");
    const std::string same_version = Key("hil-child", 1, 'b');
    const auto bad_version = StageLessonStorageHilFixture(
        lease, valid, LessonStorageHilFixture::kPreservationSet, same_version
    );
    Expect(bad_version.code == LessonStorageHilFixtureCode::kInvalidSibling,
           "same-version different-SHA sibling was accepted by stage");
    const auto bad_cleanup_version = CleanupLessonStorageHilFixture(
        lease, valid, LessonStorageHilFixture::kPreservationSet, same_version
    );
    Expect(bad_cleanup_version.code == LessonStorageHilFixtureCode::kInvalidSibling,
           "same-version different-SHA sibling was accepted by cleanup");
    const auto bad_inspect_version =
        InspectLessonStorageHilStorage(valid, same_version);
    Expect(bad_inspect_version.cache_key.empty() &&
               bad_inspect_version.sibling_cache_key.empty() &&
               bad_inspect_version.entries.empty(),
           "same-version different-SHA sibling was accepted by inspect");
    Expect(!fs::exists(Root()), "sibling validation touched filesystem");

    auto refused = Lease();
    Expect(!static_cast<bool>(refused), "second mutation lease was acquired");
    const auto lease_result = StageLessonStorageHilFixture(
        refused, valid, LessonStorageHilFixture::kNestedDirectory, ""
    );
    Expect(lease_result.code == LessonStorageHilFixtureCode::kLeaseRefused,
           "missing lease was not refused first");
    Expect(!fs::exists(Root()), "lease refusal touched filesystem");

    fs::create_directories(Root().parent_path());
    Write(Root(), "not-a-directory");
    const auto root_type = InspectLessonStorageHilStorage(valid, "");
    const auto* root_leaf = FindEntry(root_type, "lesson-assets/" + valid);
    Expect(root_leaf != nullptr && root_leaf->node_type == "unexpected",
           "inspection followed invalid root node");
}

void TestNestedDirectoryFixtureIsExactAndNonRecursive() {
    ResetRoot();
    const std::string key = Key("hil-nested", 1, 'a');
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && result.changed,
               "nested fixture did not stage");
        ExpectNoAbsolutePaths(result);
    }
    const fs::path sentinel = Leaf(key) / ".tbot-hil-nested";
    Expect(fs::is_directory(sentinel) && fs::is_empty(sentinel),
           "nested sentinel was not an exact empty directory");
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && !result.changed,
               "nested stage was not idempotent");
    }

    Write(sentinel / "deep.txt", "must survive");
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "non-empty nested sentinel was mutated");
    }
    Expect(Read(sentinel / "deep.txt") == "must survive",
           "cleanup recursed into sentinel");
    fs::remove(sentinel / "deep.txt");
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
               "nested cleanup failed");
    }
    Expect(!fs::exists(Leaf(key)) && fs::is_directory(Root() / "hil-nested"),
           "nested cleanup removed wrong scope");
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && !result.changed,
               "nested cleanup was not idempotent");
    }

    fs::create_directories(Leaf(key));
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "partial nested stage was not fail-closed");
    }
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
               "known empty interrupted cleanup was not recovered");
    }
}

void TestLeafRegularFileFixtureRequiresExactMagic() {
    ResetRoot();
    const std::string key = Key("hil-leaf", 7, 'b');
    std::string magic;
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && result.changed,
               "leaf file fixture did not stage");
        magic = Read(Leaf(key));
    }
    Expect(magic.find("TBOT-HIL-LEAF-FIXTURE-V1") != std::string::npos,
           "leaf magic was not fixed and versioned");
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && !result.changed,
               "leaf stage was not idempotent");
    }
    Write(Leaf(key), magic + "foreign");
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kSentinelMismatch &&
                   !result.changed && Read(Leaf(key)) == magic + "foreign",
               "wrong leaf magic was mutated");
    }
    Write(Leaf(key), magic);
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
               "leaf cleanup failed");
    }
    Expect(!fs::exists(Leaf(key)), "leaf fixture survived cleanup");

    fs::create_directories(Leaf(key));
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode,
               "directory at regular-file leaf was accepted");
    }
}

void TestPreservationSetUsesTwoPhaseCleanup() {
    ResetRoot();
    const std::string key = Key("hil-pair", 1, 'c');
    const std::string sibling = Key("hil-pair", 2, 'd');
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && result.changed,
               "preservation set did not stage");
    }
    const fs::path first = Leaf(key) / ".tbot-hil-sentinel";
    const fs::path second = Leaf(sibling) / ".tbot-hil-sentinel";
    const std::string first_magic = Read(first);
    const std::string second_magic = Read(second);
    Expect(first_magic != second_magic, "preservation role magic was not distinct");
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && !result.changed,
               "preservation stage was not idempotent");
    }
    Write(second, second_magic + "foreign");
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kSentinelMismatch,
               "stage did not report actual sibling sentinel mismatch");
    }
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kSentinelMismatch &&
                   !result.changed,
               "preservation mismatch was not fail-closed");
    }
    Expect(Read(first) == first_magic, "two-phase cleanup mutated primary first");
    Write(second, second_magic);
    Write(Leaf(sibling) / "foreign.bin", "foreign");
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "stage did not report actual unknown sibling state");
    }
    Expect(Read(first) == first_magic && Read(second) == second_magic,
           "stage mutated pair while reporting unknown sibling");
    fs::remove(Leaf(sibling) / "foreign.bin");
    Write(Leaf(key) / "unknown.bin", "preserve");
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "preservation extra entry was mutated");
    }
    Expect(fs::exists(first) && fs::exists(second),
           "extra-entry refusal partially cleaned pair");
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "preservation stage accepted an extra entry");
    }
    fs::remove(Leaf(key) / "unknown.bin");

    Write(Root() / "current.json", "current");
    Write(Root() / "pvg" / "protected.bin", "pvg");
    Write(Root() / "shared" / "protected.bin", "shared");
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
               "preservation cleanup failed");
    }
    Expect(!fs::exists(Leaf(key)) && !fs::exists(Leaf(sibling)),
           "preservation leaves survived cleanup");
    Expect(fs::is_directory(Root() / "hil-pair"), "cleanup removed slug");
    Expect(Read(Root() / "current.json") == "current" &&
               Read(Root() / "pvg" / "protected.bin") == "pvg" &&
               Read(Root() / "shared" / "protected.bin") == "shared",
           "cleanup mutated protected storage");
}

void TestPreservationCleanupResumesOwnedPartialStates() {
    ResetRoot();
    const std::string key = Key("hil-retry", 1, 'a');
    const std::string sibling = Key("hil-retry", 2, 'b');
    const auto stage = [&]() {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && result.changed,
               "retry fixture did not stage");
    };
    const auto cleanup = [&]() {
        auto lease = Lease();
        return CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
    };

    stage();
    g_fail_unlink_call = 1;
    SetLessonStorageHilFixtureUnlinkCallbackForTest(InjectedUnlink);
    auto result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && !result.changed,
           "first unlink failure was not truthful");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
           "cleanup did not retry after first unlink failure");

    stage();
    g_fail_unlink_call = 2;
    SetLessonStorageHilFixtureUnlinkCallbackForTest(InjectedUnlink);
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "second unlink failure did not report partial mutation");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
           "cleanup did not resume empty-primary plus complete-sibling state");

    stage();
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "first rmdir failure did not report deleted sentinels");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
           "cleanup did not resume two empty fixture leaves");

    stage();
    g_fail_rmdir_call = 2;
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "second rmdir failure did not report partial removal");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
           "cleanup did not resume missing-primary plus empty-sibling state");
    Expect(!fs::exists(Leaf(key)) && !fs::exists(Leaf(sibling)),
           "retry cleanup left fixture-owned leaves");
}

void TestPreservationStageRollbackReportsResidualTruthAndCleanupConverges() {
    const std::string key = Key("hil-stage-rollback", 1, 'a');
    const std::string sibling = Key("hil-stage-rollback", 2, 'b');
    const fs::path first_leaf = Leaf(key);
    const fs::path first_sentinel = first_leaf / ".tbot-hil-sentinel";
    const auto stage_with_faults = [&]() {
        auto lease = Lease();
        return StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
    };
    const auto cleanup = [&]() {
        auto lease = Lease();
        return CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
    };
    const auto prepare_existing_parents = [&]() {
        ResetRoot();
        ResetMutationInjection();
        fs::create_directories(first_leaf.parent_path());
        g_fail_mkdir_call = 2;
        SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    };
    const auto expect_cleanup_converges = [&]() {
        ResetMutationInjection();
        auto result = cleanup();
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
               "cleanup did not remove a verified staging rollback residual");
        result = cleanup();
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && !result.changed,
               "repeated rollback cleanup did not converge");
    };

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 4;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    auto result = stage_with_faults();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && !result.changed,
           "complete staging rollback reported a residual mutation");
    Expect(!fs::exists(Root()),
           "complete staging rollback left created parent storage");

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 4;
    g_fail_rmdir_call = 2;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage_with_faults();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "failed slug rollback hid created parent storage");
    Expect(fs::is_directory(first_leaf.parent_path()) &&
               fs::is_empty(first_leaf.parent_path()),
           "failed slug rollback did not leave the expected empty slug");

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 4;
    g_fail_rmdir_call = 3;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage_with_faults();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "failed root rollback hid created root storage");
    Expect(fs::is_directory(Root()) && fs::is_empty(Root()),
           "failed root rollback did not leave the expected empty root");

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 3;
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage_with_faults();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "failed parent rollback after primary creation failure hid storage");
    Expect(fs::is_directory(first_leaf.parent_path()) &&
               fs::is_empty(first_leaf.parent_path()),
           "primary creation rollback did not leave the expected empty slug");

    prepare_existing_parents();
    g_fail_unlink_call = 1;
    SetLessonStorageHilFixtureUnlinkCallbackForTest(InjectedUnlink);
    result = stage_with_faults();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "failed primary rollback unlink hid the staged residual");
    Expect(Read(first_sentinel) == "TBOT-HIL-PRESERVATION-PRIMARY-V1\n" &&
               !fs::exists(Leaf(sibling)),
           "failed unlink did not leave the expected verified primary residual");
    expect_cleanup_converges();

    prepare_existing_parents();
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage_with_faults();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "failed primary rollback rmdir hid the staged residual");
    Expect(fs::is_directory(first_leaf) && fs::is_empty(first_leaf) &&
               !fs::exists(Leaf(sibling)),
           "failed rmdir did not leave the expected empty primary residual");
    expect_cleanup_converges();
}

void TestPreservationCleanupAcceptsAnExactlyEvictedPrimary() {
    ResetRoot();
    const std::string key = Key("hil-evicted", 1, 'a');
    const std::string sibling = Key("hil-evicted", 2, 'b');
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && result.changed,
               "eviction preservation fixture did not stage");
    }

    Write(Root() / "current.json", "current");
    Write(Root() / "pvg" / "protected.bin", "pvg");
    Write(Root() / "shared" / "protected.bin", "shared");
    Expect(fs::remove(Leaf(key) / ".tbot-hil-sentinel"),
           "primary sentinel was not removed by exact eviction setup");
    Expect(fs::remove(Leaf(key)),
           "primary leaf was not removed by exact eviction setup");

    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
               "cleanup rejected missing-primary/complete-sibling eviction state");
    }
    Expect(!fs::exists(Leaf(key)) && !fs::exists(Leaf(sibling)),
           "cleanup left an exact preservation leaf after eviction");
    Expect(fs::is_directory(Root()) && fs::is_directory(Root() / "hil-evicted"),
           "cleanup removed the root or preservation slug");
    Expect(Read(Root() / "current.json") == "current" &&
               Read(Root() / "pvg" / "protected.bin") == "pvg" &&
               Read(Root() / "shared" / "protected.bin") == "shared",
           "post-eviction cleanup mutated protected storage");
}

void TestPreservationCleanupAcceptsEveryExactPartialOrder() {
    const std::string key = Key("hil-order", 1, 'c');
    const std::string sibling = Key("hil-order", 2, 'd');
    const fs::path first_leaf = Leaf(key);
    const fs::path second_leaf = Leaf(sibling);
    const auto arrange = [&](const fs::path& leaf, int state, const char* magic) {
        if (state == 1) {
            fs::create_directories(leaf);
        } else if (state == 2) {
            Write(leaf / ".tbot-hil-sentinel", magic);
        }
    };
    for (int first_state = 0; first_state < 3; ++first_state) {
        for (int second_state = 0; second_state < 3; ++second_state) {
            ResetRoot();
            fs::create_directories(first_leaf.parent_path());
            arrange(
                first_leaf,
                first_state,
                "TBOT-HIL-PRESERVATION-PRIMARY-V1\n"
            );
            arrange(
                second_leaf,
                second_state,
                "TBOT-HIL-PRESERVATION-SIBLING-V1\n"
            );

            const bool expected_change = first_state != 0 || second_state != 0;
            {
                auto lease = Lease();
                const auto result = CleanupLessonStorageHilFixture(
                    lease, key, LessonStorageHilFixture::kPreservationSet, sibling
                );
                Expect(result.code == LessonStorageHilFixtureCode::kCleaned &&
                           result.changed == expected_change,
                       "cleanup rejected an exact bounded preservation partial pair");
            }
            Expect(!fs::exists(first_leaf) && !fs::exists(second_leaf),
                   "cleanup left an exact bounded preservation partial pair");
            auto retry_lease = Lease();
            const auto retry_result = CleanupLessonStorageHilFixture(
                retry_lease,
                key,
                LessonStorageHilFixture::kPreservationSet,
                sibling
            );
            Expect(retry_result.code == LessonStorageHilFixtureCode::kCleaned &&
                       !retry_result.changed,
                   "repeated exact partial cleanup did not converge");
        }
    }
}

void TestPreservationCleanupRejectsUnownedPartialStates() {
    const std::string key = Key("hil-unowned", 1, 'c');
    const std::string sibling = Key("hil-unowned", 2, 'd');
    const fs::path first_leaf = Leaf(key);
    const fs::path second_leaf = Leaf(sibling);
    const auto reject = [&]() {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kPreservationSet, sibling
        );
        Expect((result.code == LessonStorageHilFixtureCode::kSentinelMismatch ||
                result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode) &&
                   !result.changed,
               "cleanup accepted an unowned preservation partial state");
    };

    ResetRoot();
    Write(first_leaf / ".tbot-hil-sentinel", "foreign");
    reject();
    Expect(Read(first_leaf / ".tbot-hil-sentinel") == "foreign",
           "sentinel mismatch refusal mutated foreign bytes");

    ResetRoot();
    Write(
        first_leaf / ".tbot-hil-sentinel",
        "TBOT-HIL-PRESERVATION-PRIMARY-V1\n"
    );
    Write(first_leaf / "foreign.bin", "foreign");
    fs::create_directories(second_leaf);
    reject();
    Expect(fs::exists(first_leaf / "foreign.bin") && fs::is_directory(second_leaf),
           "unexpected-entry refusal partially cleaned the pair");

    ResetRoot();
    Write(
        first_leaf / ".tbot-hil-sentinel",
        "TBOT-HIL-PRESERVATION-SIBLING-V1\n"
    );
    reject();
    Expect(fs::exists(first_leaf / ".tbot-hil-sentinel"),
           "role-mismatched sentinel refusal mutated the primary");
}

void TestSymlinksAreRefusedWithoutFollowingTargets() {
    ResetRoot();
    const fs::path outside = Root().parent_path() / "outside-hil-fixture";
    std::error_code error;
    fs::remove_all(outside, error);
    Expect(!error, "outside fixture reset failed");
    fs::create_directories(outside);

    const std::string leaf_key = Key("hil-link-file", 1, 'a');
    fs::create_directories(Leaf(leaf_key).parent_path());
    Write(outside / "leaf-magic", "TBOT-HIL-LEAF-FIXTURE-V1\n");
    fs::create_symlink(outside / "leaf-magic", Leaf(leaf_key), error);
    if (error) {
        fs::remove_all(outside, error);
        ResetRoot();
        return;
    }
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, leaf_key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "regular-file symlink was followed or removed");
    }
    Expect(fs::is_symlink(Leaf(leaf_key)) &&
               Read(outside / "leaf-magic") == "TBOT-HIL-LEAF-FIXTURE-V1\n",
           "regular-file symlink cleanup mutated the link or target");

    ResetRoot();
    const std::string nested_key = Key("hil-link-dir", 1, 'b');
    fs::create_directories(Leaf(nested_key).parent_path());
    fs::create_directories(outside / "nested-target" / ".tbot-hil-nested");
    error.clear();
    fs::create_directory_symlink(outside / "nested-target", Leaf(nested_key), error);
    Expect(!error, "directory symlink setup failed after symlink support probe");
    {
        auto lease = Lease();
        const auto result = CleanupLessonStorageHilFixture(
            lease, nested_key, LessonStorageHilFixture::kNestedDirectory, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "directory symlink was followed during cleanup");
    }
    Expect(fs::is_symlink(Leaf(nested_key)) &&
               fs::is_directory(outside / "nested-target" / ".tbot-hil-nested"),
           "directory symlink cleanup mutated the link or target");

    ResetRoot();
    const std::string root_key = Key("hil-link-root", 1, 'c');
    error.clear();
    fs::create_directory_symlink(outside, Root(), error);
    Expect(!error, "root symlink setup failed after symlink support probe");
    {
        auto lease = Lease();
        const auto result = StageLessonStorageHilFixture(
            lease, root_key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
        Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed,
               "root symlink was followed during staging");
    }
    Expect(!fs::exists(outside / "hil-link-root"),
           "root symlink staging mutated its external target");

    ResetRoot();
    fs::remove_all(outside, error);
    Expect(!error, "outside fixture cleanup failed");
}

void TestInspectionIsReadOnlyBoundedSortedAndDataSensitive() {
    ResetRoot();
    const std::string key = Key("hil-inspect", 1, 'e');
    const std::string sibling = Key("hil-inspect", 2, 'f');
    Write(Leaf(key) / "z.txt", "z");
    Write(Leaf(key) / "a.txt", "alpha");
    Write(Leaf(key) / "nested" / "too-deep" / "secret.txt", "secret");
    const std::string overlong_name(49, 'q');
    Write(Leaf(key) / overlong_name, "overlong");
    Write(Leaf(sibling) / "sibling.bin", "sibling");
    Write(Root() / "current.json", "current-v1");
    Write(Root() / "pvg" / "b.bin", "pvg-b");
    Write(Root() / "pvg" / "a.bin", "pvg-a");
    Write(Root() / "shared" / "shared.bin", "shared");
    Write(Root() / "pvg" / "line\nbreak", "newline");
    Write(Root() / "pvg" / "percent%name", "percent");
    Write(Root() / "pvg" / (std::string("utf8-") + "\xC3\xA9"), "utf8");
    for (int index = 0; index < 40; ++index) {
        Write(Root() / "shared" / ("cap-" + std::to_string(index)), "x");
    }

    const auto first = InspectLessonStorageHilStorage(key, sibling);
    Expect(first.cache_key == key && first.sibling_cache_key == sibling,
           "inspection did not retain validated keys");
    Expect(first.truncated, "inspection did not report truncation");
    Expect(!first.entries.empty() && first.entries.size() < 80,
           "inspection was not hard bounded");
    Expect(std::is_sorted(
               first.entries.begin(),
               first.entries.end(),
               [](const auto& left, const auto& right) {
                   return left.label < right.label;
               }
           ),
           "inspection labels were not sorted");
    const auto repeated = InspectLessonStorageHilStorage(key, sibling);
    Expect(repeated.entries.size() == first.entries.size(),
           "repeated inspection changed bounded entry count");
    for (std::size_t index = 0; index < first.entries.size(); ++index) {
        Expect(repeated.entries[index].label == first.entries[index].label,
               "repeated inspection changed label ordering");
    }
    for (const auto& entry : first.entries) {
        Expect(entry.label.rfind("lesson-assets", 0) == 0,
               "inspection label was not stable-relative");
        Expect(entry.label.empty() || entry.label[0] != '/',
               "inspection label began with slash");
        Expect(entry.label.find("/sdcard") == std::string::npos,
               "inspection leaked absolute root");
        Expect(entry.label.find('\n') == std::string::npos &&
                   entry.label.find('\r') == std::string::npos,
               "inspection label leaked raw control bytes");
        Expect(entry.label.size() <= 384, "inspection label exceeded byte cap");
        Expect(entry.node_type == "missing" || entry.node_type == "regular_file" ||
                   entry.node_type == "directory" || entry.node_type == "unexpected",
               "inspection emitted unstable node type");
        Expect(entry.label.find("too-deep/secret.txt") == std::string::npos,
               "inspection recursed below direct children");
    }

    Expect(FindEntry(first, "lesson-assets/pvg/line%0Abreak") != nullptr,
           "newline filename was not safely encoded");
    Expect(FindEntry(first, "lesson-assets/pvg/percent%25name") != nullptr,
           "percent filename was not safely encoded");
    Expect(FindEntry(first, "lesson-assets/pvg/utf8-%C3%A9") != nullptr,
           "non-ASCII filename was not safely encoded");
    Expect(FindEntry(first, "lesson-assets/" + key + "/" + overlong_name) == nullptr,
           "overlong raw filename was emitted");

    const std::string label = "lesson-assets/" + key + "/a.txt";
    const auto* before = FindEntry(first, label);
    Expect(before != nullptr && before->node_type == "regular_file" &&
               before->bytes == 5 && before->sha256.size() == 64,
           "regular fingerprint was incomplete");
    Expect(std::all_of(before->sha256.begin(), before->sha256.end(), [](char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           }),
           "fingerprint was not lowercase hex");
    Expect(FindEntry(first, "lesson-assets/current.json") != nullptr &&
               FindEntry(first, "lesson-assets/pvg") != nullptr &&
               FindEntry(first, "lesson-assets/shared") != nullptr,
           "inspection omitted a protected location");
    const std::string old_sha = before->sha256;
    Write(Leaf(key) / "a.txt", "bravo");
    const auto changed = InspectLessonStorageHilStorage(key, sibling);
    const auto* after = FindEntry(changed, label);
    Expect(after != nullptr && after->sha256 != old_sha,
           "SHA did not change with bytes");
    Expect(Read(Root() / "current.json") == "current-v1",
           "inspection mutated protected metadata");

    ResetRoot();
    Write(Leaf(key) / overlong_name, "overlong-only");
    const auto overlong = InspectLessonStorageHilStorage(key, "");
    Expect(overlong.truncated, "raw filename cap did not set truncated");
    Expect(FindEntry(overlong, "lesson-assets/" + key + "/" + overlong_name) ==
               nullptr,
           "raw filename cap emitted overlong label");

    ResetRoot();
    const auto missing = InspectLessonStorageHilStorage(key, "");
    Expect(!fs::exists(Root()), "inspection created root");
    const auto* missing_leaf = FindEntry(missing, "lesson-assets/" + key);
    const auto* missing_current = FindEntry(missing, "lesson-assets/current.json");
    Expect(missing_leaf != nullptr && missing_leaf->node_type == "missing",
           "missing leaf was not reported");
    Expect(missing_current != nullptr && missing_current->node_type == "missing",
           "missing current.json was not reported");

    const auto invalid = InspectLessonStorageHilStorage(Key("child", 1, 'a'), sibling);
    Expect(invalid.cache_key.empty() && invalid.sibling_cache_key.empty() &&
               invalid.entries.empty(),
           "invalid inspection echoed values");
}

}  // namespace

int main() {
    TestValidationAndLeaseRefusalPrecedeFilesystemAccess();
    TestNestedDirectoryFixtureIsExactAndNonRecursive();
    TestLeafRegularFileFixtureRequiresExactMagic();
    TestPreservationSetUsesTwoPhaseCleanup();
    TestPreservationCleanupResumesOwnedPartialStates();
    TestPreservationStageRollbackReportsResidualTruthAndCleanupConverges();
    TestPreservationCleanupAcceptsAnExactlyEvictedPrimary();
    TestPreservationCleanupAcceptsEveryExactPartialOrder();
    TestPreservationCleanupRejectsUnownedPartialStates();
    TestSymlinksAreRefusedWithoutFollowingTargets();
    TestInspectionIsReadOnlyBoundedSortedAndDataSensitive();
    ResetRoot();
    std::cout << "lesson storage HIL fixture host checks: " << g_checks << '\n';
    return 0;
}
