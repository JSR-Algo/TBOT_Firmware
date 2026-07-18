#include "lesson_storage_hil_fixture.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
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
int g_fsync_calls = 0;
int g_write_calls = 0;
int g_open_calls = 0;
int g_unlink_calls = 0;
int g_rmdir_calls = 0;
int g_fail_mkdir_call = 0;
int g_fail_fsync_call = 0;
int g_fail_write_call = 0;
int g_fail_open_call = 0;
int g_create_then_fail_mkdir_call = 0;
int g_create_then_fail_open_call = 0;
int g_fail_unlink_call = 0;
int g_fail_rmdir_call = 0;
int g_inspect_path_calls = 0;
int g_fail_inspect_path_call = 0;
std::string g_inspect_failure_path;
std::size_t g_read_bytes = 0;
std::size_t g_largest_read_request = 0;
std::size_t g_directory_read_calls = 0;

int InjectedMkdir(const char* path) {
    ++g_mkdir_calls;
    if (g_mkdir_calls == g_create_then_fail_mkdir_call) {
        const int result = mkdir(path, 0755);
        if (result == 0) {
            errno = EIO;
            return -1;
        }
        return result;
    }
    if (g_mkdir_calls == g_fail_mkdir_call) {
        errno = EIO;
        return -1;
    }
    return mkdir(path, 0755);
}

int InjectedFsync(int descriptor) {
    ++g_fsync_calls;
    if (g_fsync_calls == g_fail_fsync_call) {
        errno = EIO;
        return -1;
    }
    return fsync(descriptor);
}

ssize_t InjectedWrite(int descriptor, const void* bytes, std::size_t length) {
    ++g_write_calls;
    if (g_write_calls == g_fail_write_call) {
        errno = EIO;
        return -1;
    }
    return write(descriptor, bytes, length);
}

int InjectedOpen(const char* path, int flags, mode_t mode) {
    ++g_open_calls;
    if (g_open_calls == g_create_then_fail_open_call) {
        const int descriptor = open(path, flags, mode);
        if (descriptor >= 0) {
            close(descriptor);
            errno = EIO;
            return -1;
        }
        return descriptor;
    }
    if (g_open_calls == g_fail_open_call) {
        errno = EIO;
        return -1;
    }
    return open(path, flags, mode);
}

std::size_t ObservedRead(void* bytes, std::size_t length, FILE* file) {
    g_read_bytes += length;
    g_largest_read_request = std::max(g_largest_read_request, length);
    return std::fread(bytes, 1, length, file);
}

void ObserveDirectoryRead() { ++g_directory_read_calls; }

bool InjectedInspectFailure(const char* path) {
    if (g_inspect_failure_path != path) {
        return false;
    }
    ++g_inspect_path_calls;
    return g_inspect_path_calls == g_fail_inspect_path_call;
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
    g_fsync_calls = 0;
    g_write_calls = 0;
    g_open_calls = 0;
    g_unlink_calls = 0;
    g_rmdir_calls = 0;
    g_fail_mkdir_call = 0;
    g_fail_fsync_call = 0;
    g_fail_write_call = 0;
    g_fail_open_call = 0;
    g_create_then_fail_mkdir_call = 0;
    g_create_then_fail_open_call = 0;
    g_fail_unlink_call = 0;
    g_fail_rmdir_call = 0;
    g_inspect_path_calls = 0;
    g_fail_inspect_path_call = 0;
    g_inspect_failure_path.clear();
    g_read_bytes = 0;
    g_largest_read_request = 0;
    g_directory_read_calls = 0;
    SetLessonStorageHilFixtureMkdirCallbackForTest(nullptr);
    SetLessonStorageHilFixtureFsyncCallbackForTest(nullptr);
    SetLessonStorageHilFixtureWriteCallbackForTest(nullptr);
    SetLessonStorageHilFixtureOpenCallbackForTest(nullptr);
    SetLessonStorageHilFixtureReadCallbackForTest(nullptr);
    SetLessonStorageHilFixtureDirectoryReadCallbackForTest(nullptr);
    SetLessonStorageHilFixtureInspectFailureCallbackForTest(nullptr);
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
fs::path NamespaceRoot() { return Root().parent_path(); }
fs::path MountPoint() { return NamespaceRoot().parent_path(); }
fs::path Leaf(const std::string& key) { return Root() / fs::path(key); }

void ResetBlankMountedCard() {
    std::error_code error;
    fs::remove_all(NamespaceRoot(), error);
    Expect(!error, "blank-card namespace reset failed");
    fs::create_directories(MountPoint(), error);
    Expect(!error, "blank-card mount point creation failed");
}

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

void TestMountedBlankCardCreatesNamespaceHierarchy() {
    ResetBlankMountedCard();
    ResetMutationInjection();
    const std::string key = Key("hil-blank", 1, 'a');
    const std::string sibling = Key("hil-blank", 2, 'b');
    auto lease = Lease();
    const auto staged = StageLessonStorageHilFixture(
        lease, key, LessonStorageHilFixture::kPreservationSet, sibling
    );
    Expect(staged.code == LessonStorageHilFixtureCode::kStaged && staged.changed,
           "mounted blank card did not stage preservation fixture");
    Expect(fs::is_directory(NamespaceRoot()), "TBOT namespace was not created");
    Expect(fs::is_directory(Root()), "lesson-assets root was not created");
    Expect(fs::is_regular_file(Leaf(key) / ".tbot-hil-sentinel"),
           "primary preservation sentinel missing");
    Expect(fs::is_regular_file(Leaf(sibling) / ".tbot-hil-sentinel"),
           "sibling preservation sentinel missing");
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
        Expect(result.code ==
                       LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                   !result.changed && fs::is_directory(Leaf(key)),
               "unattested empty nested leaf was deleted");
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

void TestNestedStageRollbackReportsEveryResidual() {
    const std::string key = Key("hil-nested-rollback", 1, 'a');
    const fs::path leaf = Leaf(key);
    const auto stage = [&]() {
        auto lease = Lease();
        return StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
    };
    const auto cleanup = [&]() {
        auto lease = Lease();
        return CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kNestedDirectory, ""
        );
    };

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 2;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    auto result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && !result.changed,
           "clean nested parent rollback reported a residual");
    Expect(!fs::exists(Root()), "clean nested parent rollback left storage");

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 2;
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "nested EnsureParents rollback hid a created root");
    Expect(fs::is_directory(Root()) && fs::is_empty(Root()),
           "nested EnsureParents rollback residual was not exact");

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 3;
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "nested leaf mkdir parent rollback hid an empty slug");
    Expect(fs::is_directory(leaf.parent_path()) && fs::is_empty(leaf.parent_path()),
           "nested leaf mkdir rollback residual was not exact");

    ResetRoot();
    ResetMutationInjection();
    fs::create_directories(leaf.parent_path());
    g_fail_mkdir_call = 2;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && !result.changed,
           "clean nested leaf rollback reported a residual");
    Expect(!fs::exists(leaf), "clean nested leaf rollback left a leaf");

    ResetMutationInjection();
    g_fail_mkdir_call = 1;
    g_inspect_failure_path = leaf.string();
    g_fail_inspect_path_call = 2;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureInspectFailureCallbackForTest(
        InjectedInspectFailure
    );
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "uncertain nested rollback inspection was reported as restored");
    Expect(!fs::exists(leaf),
           "uncertain nested rollback test unexpectedly left a physical leaf");

    ResetMutationInjection();
    g_fail_mkdir_call = 2;
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "nested sentinel rollback hid an empty owned leaf");
    Expect(fs::is_directory(leaf) && fs::is_empty(leaf),
           "nested sentinel rollback residual was not exact");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
               !result.changed && fs::is_directory(leaf),
           "nested cleanup deleted an unattested empty rollback residual");
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
               !result.changed && fs::is_directory(leaf),
           "repeated nested cleanup changed unattested storage");

    ResetRoot();
    ResetMutationInjection();
    fs::create_directories(leaf.parent_path());
    g_create_then_fail_mkdir_call = 1;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed &&
               fs::is_directory(leaf) && fs::is_empty(leaf),
           "create-then-error leaf was deleted without ownership attestation");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
               !result.changed && fs::is_directory(leaf),
           "cleanup deleted create-then-error unattested leaf");

    ResetRoot();
    ResetMutationInjection();
    fs::create_directories(leaf.parent_path());
    g_create_then_fail_mkdir_call = 2;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    result = stage();
    const fs::path sentinel = leaf / ".tbot-hil-nested";
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed &&
               fs::is_directory(sentinel),
           "failed-attempt nested sentinel was removed as transaction-owned");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed &&
               !fs::exists(leaf),
           "exact nested sentinel proof was not safely recoverable");
}

void TestLeafStageRollbackReportsEveryResidual() {
    const std::string key = Key("hil-leaf-rollback", 1, 'b');
    const fs::path leaf = Leaf(key);
    const auto stage = [&]() {
        auto lease = Lease();
        return StageLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
    };
    const auto cleanup = [&]() {
        auto lease = Lease();
        return CleanupLessonStorageHilFixture(
            lease, key, LessonStorageHilFixture::kLeafRegularFile, ""
        );
    };

    ResetRoot();
    ResetMutationInjection();
    g_fail_mkdir_call = 2;
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureMkdirCallbackForTest(InjectedMkdir);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    auto result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "leaf EnsureParents rollback hid a created root");

    ResetRoot();
    ResetMutationInjection();
    fs::create_directories(leaf.parent_path());
    g_fail_fsync_call = 1;
    SetLessonStorageHilFixtureFsyncCallbackForTest(InjectedFsync);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && !result.changed,
           "clean leaf file rollback reported a residual");
    Expect(!fs::exists(leaf), "clean leaf file rollback left a file");

    ResetMutationInjection();
    g_fail_fsync_call = 1;
    g_fail_unlink_call = 1;
    SetLessonStorageHilFixtureFsyncCallbackForTest(InjectedFsync);
    SetLessonStorageHilFixtureUnlinkCallbackForTest(InjectedUnlink);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "leaf file rollback unlink failure hid exact owned bytes");
    Expect(Read(leaf) == "TBOT-HIL-LEAF-FIXTURE-V1\n",
           "leaf file rollback residual did not retain exact magic");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
           "leaf cleanup did not remove an exact rollback residual");
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kCleaned && !result.changed,
           "repeated leaf rollback cleanup did not converge");

    ResetRoot();
    ResetMutationInjection();
    g_fail_fsync_call = 1;
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureFsyncCallbackForTest(InjectedFsync);
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "leaf write parent rollback hid an empty slug");
    Expect(fs::is_directory(leaf.parent_path()) && fs::is_empty(leaf.parent_path()),
           "leaf write parent rollback residual was not exact");

    ResetRoot();
    ResetMutationInjection();
    fs::create_directories(leaf.parent_path());
    g_fail_write_call = 1;
    SetLessonStorageHilFixtureWriteCallbackForTest(InjectedWrite);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && !result.changed,
           "clean leaf write rollback reported a residual");
    Expect(!fs::exists(leaf), "clean leaf write rollback left a file");

    ResetMutationInjection();
    g_fail_write_call = 1;
    g_fail_unlink_call = 1;
    SetLessonStorageHilFixtureWriteCallbackForTest(InjectedWrite);
    SetLessonStorageHilFixtureUnlinkCallbackForTest(InjectedUnlink);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "failed leaf write rollback hid uncertain created state");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kSentinelMismatch &&
               !result.changed && fs::exists(leaf),
           "uncertain leaf write residual did not remain fail-closed");

    ResetRoot();
    ResetMutationInjection();
    fs::create_directories(leaf.parent_path());
    g_create_then_fail_open_call = 1;
    SetLessonStorageHilFixtureOpenCallbackForTest(InjectedOpen);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed &&
               fs::is_regular_file(leaf) && fs::file_size(leaf) == 0,
           "create-then-error file was deleted without ownership attestation");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kSentinelMismatch &&
               !result.changed && fs::is_regular_file(leaf),
           "cleanup did not preserve unattested create-then-error file");
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
    Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
               !result.changed && fs::is_directory(Leaf(key)) &&
               fs::is_regular_file(Leaf(sibling) / ".tbot-hil-sentinel"),
           "cleanup deleted an unattested empty primary leaf");

    ResetRoot();
    ResetMutationInjection();
    stage();
    g_fail_rmdir_call = 1;
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "first rmdir failure did not report deleted sentinels");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
               !result.changed && fs::is_directory(Leaf(key)) &&
               fs::is_directory(Leaf(sibling)),
           "cleanup deleted unattested empty preservation leaves");

    ResetRoot();
    ResetMutationInjection();
    stage();
    g_fail_rmdir_call = 2;
    SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "second rmdir failure did not report partial removal");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
               !result.changed && !fs::exists(Leaf(key)) &&
               fs::is_directory(Leaf(sibling)),
           "cleanup deleted an unattested empty sibling leaf");
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
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
               !result.changed && fs::is_directory(first_leaf),
           "later cleanup deleted an empty rollback leaf without proof");

    ResetRoot();
    ResetMutationInjection();
    fs::create_directories(first_leaf.parent_path());
    g_fail_write_call = 1;
    g_fail_unlink_call = 1;
    SetLessonStorageHilFixtureWriteCallbackForTest(InjectedWrite);
    SetLessonStorageHilFixtureUnlinkCallbackForTest(InjectedUnlink);
    result = stage_with_faults();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed && result.changed,
           "first preservation sentinel rollback hid uncertain created state");
    ResetMutationInjection();
    result = cleanup();
    Expect(result.code == LessonStorageHilFixtureCode::kSentinelMismatch &&
               !result.changed && fs::exists(first_sentinel),
           "uncertain first preservation leaf did not remain fail-closed");
}

void TestSecondPreservationSentinelRollbackCoversEveryCreatedNode() {
    const std::string key = Key("hil-second-rollback", 1, 'c');
    const std::string sibling = Key("hil-second-rollback", 2, 'd');
    const fs::path first_leaf = Leaf(key);
    const fs::path second_leaf = Leaf(sibling);
    const fs::path first_sentinel = first_leaf / ".tbot-hil-sentinel";
    const fs::path second_sentinel = second_leaf / ".tbot-hil-sentinel";
    const fs::path slug = first_leaf.parent_path();
    const auto stage = [&]() {
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

    ResetRoot();
    ResetMutationInjection();
    g_fail_write_call = 2;
    SetLessonStorageHilFixtureWriteCallbackForTest(InjectedWrite);
    auto result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed &&
               !result.changed && !fs::exists(Root()),
           "clean second-sentinel write rollback did not restore storage");

    ResetRoot();
    ResetMutationInjection();
    g_fail_fsync_call = 2;
    SetLessonStorageHilFixtureFsyncCallbackForTest(InjectedFsync);
    result = stage();
    Expect(result.code == LessonStorageHilFixtureCode::kIoFailed &&
               !result.changed && !fs::exists(Root()),
           "clean second-sentinel fsync rollback did not restore storage");

    struct RollbackFault {
        int fail_unlink_call;
        int fail_rmdir_call;
        int residual_node;
        const char* label;
    };
    const std::vector<RollbackFault> faults = {
        {1, 0, 0, "second sentinel"},
        {0, 1, 1, "second leaf"},
        {2, 0, 2, "first sentinel"},
        {0, 2, 3, "first leaf"},
        {0, 3, 4, "slug"},
        {0, 4, 5, "root"},
    };
    for (const auto& fault : faults) {
        ResetRoot();
        ResetMutationInjection();
        g_fail_fsync_call = 2;
        g_fail_unlink_call = fault.fail_unlink_call;
        g_fail_rmdir_call = fault.fail_rmdir_call;
        SetLessonStorageHilFixtureFsyncCallbackForTest(InjectedFsync);
        SetLessonStorageHilFixtureUnlinkCallbackForTest(InjectedUnlink);
        SetLessonStorageHilFixtureRmdirCallbackForTest(InjectedRmdir);
        result = stage();
        Expect(result.code == LessonStorageHilFixtureCode::kIoFailed &&
                   result.changed,
               fault.label);
        if (fault.residual_node == 0) {
            Expect(fs::is_regular_file(second_sentinel) &&
                       !fs::exists(first_leaf),
                   "second-sentinel unlink failure left the wrong state");
        } else if (fault.residual_node == 1) {
            Expect(fs::is_directory(second_leaf) && fs::is_empty(second_leaf) &&
                       !fs::exists(first_leaf),
                   "second-leaf rmdir failure left the wrong state");
        } else if (fault.residual_node == 2) {
            Expect(fs::is_regular_file(first_sentinel) &&
                       !fs::exists(second_leaf),
                   "first-sentinel unlink failure left the wrong state");
        } else if (fault.residual_node == 3) {
            Expect(fs::is_directory(first_leaf) && fs::is_empty(first_leaf) &&
                       !fs::exists(second_leaf),
                   "first-leaf rmdir failure left the wrong state");
        } else if (fault.residual_node == 4) {
            Expect(fs::is_directory(slug) && fs::is_empty(slug),
                   "slug rmdir failure left the wrong state");
        } else {
            Expect(fs::is_directory(Root()) && fs::is_empty(Root()),
                   "root rmdir failure left the wrong state");
        }

        ResetMutationInjection();
        result = cleanup();
        const bool unattested_empty =
            fault.residual_node == 1 || fault.residual_node == 3;
        if (unattested_empty) {
            Expect(result.code ==
                           LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                       !result.changed,
                   "later cleanup deleted an unattested empty rollback leaf");
            result = stage();
            Expect(result.code ==
                           LessonStorageHilFixtureCode::kUnexpectedExistingNode &&
                       !result.changed,
                   "stage accepted an unattested empty rollback leaf");
            continue;
        }
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned,
               "attested rollback residual was not safely recoverable");
        result = stage();
        Expect(result.code == LessonStorageHilFixtureCode::kStaged && result.changed,
               "staging did not recover after attested residual cleanup");
        result = cleanup();
        Expect(result.code == LessonStorageHilFixtureCode::kCleaned && result.changed,
               "cleanup did not converge after rollback recovery restage");
        Expect(!fs::exists(Leaf(key)) && !fs::exists(Leaf(sibling)),
               "rollback recovery left a fixture leaf");
    }
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

void TestPreservationCleanupRequiresProofForEveryExistingLeaf() {
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
    const auto matches_arranged_state = [](
                                            const fs::path& leaf,
                                            int state,
                                            const char* magic
                                        ) {
        if (state == 0) {
            return !fs::exists(leaf);
        }
        if (state == 1) {
            return fs::is_directory(leaf) && fs::is_empty(leaf);
        }
        return Read(leaf / ".tbot-hil-sentinel") == magic;
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

            const bool has_unattested_empty =
                first_state == 1 || second_state == 1;
            const bool expected_change =
                !has_unattested_empty && (first_state != 0 || second_state != 0);
            {
                auto lease = Lease();
                const auto result = CleanupLessonStorageHilFixture(
                    lease, key, LessonStorageHilFixture::kPreservationSet, sibling
                );
                const auto expected_code = has_unattested_empty
                                               ? LessonStorageHilFixtureCode::
                                                     kUnexpectedExistingNode
                                               : LessonStorageHilFixtureCode::kCleaned;
                Expect(result.code == expected_code &&
                           result.changed == expected_change,
                       "cleanup did not enforce preservation ownership proof");
            }
            if (has_unattested_empty) {
                Expect((first_state == 0 || fs::exists(first_leaf)) &&
                           (second_state == 0 || fs::exists(second_leaf)),
                       "cleanup mutated a pair containing an unattested empty leaf");
                Expect(matches_arranged_state(
                           first_leaf,
                           first_state,
                           "TBOT-HIL-PRESERVATION-PRIMARY-V1\n"
                       ),
                       "cleanup changed the primary while refusing empty ownership");
                Expect(matches_arranged_state(
                           second_leaf,
                           second_state,
                           "TBOT-HIL-PRESERVATION-SIBLING-V1\n"
                       ),
                       "cleanup changed the sibling while refusing empty ownership");
                continue;
            }
            Expect(!fs::exists(first_leaf) && !fs::exists(second_leaf),
                   "cleanup left an attested preservation partial pair");
            auto retry_lease = Lease();
            const auto retry_result = CleanupLessonStorageHilFixture(
                retry_lease,
                key,
                LessonStorageHilFixture::kPreservationSet,
                sibling
            );
            Expect(retry_result.code == LessonStorageHilFixtureCode::kCleaned &&
                       !retry_result.changed,
                   "repeated attested partial cleanup did not converge");
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

void TestInspectionNeverReadsPastPerFileOrAggregateBudgets() {
    ResetRoot();
    ResetMutationInjection();
    const std::string key = Key("hil-read-budget", 1, 'a');
    const std::string sibling = Key("hil-read-budget", 2, 'b');
    Write(Leaf(key) / "small.bin", std::string(20, 'a'));
    Write(Leaf(key) / "oversized.bin", std::string(65, 'x'));
    Write(Leaf(sibling) / "sibling.bin", std::string(20, 's'));
    Write(Root() / "current.json", std::string(50, 'b'));
    Write(Root() / "pvg" / "pvg.bin", std::string(50, 'c'));
    Write(Root() / "shared" / "shared.bin", std::string(50, 'd'));
    SetLessonStorageHilFixtureReadCallbackForTest(ObservedRead);

    const auto first = InspectLessonStorageHilStorage(key, sibling);
    Expect(first.truncated, "inspection byte-budget exhaustion was not reported");
    Expect(g_read_bytes <= 160, "inspection read past aggregate byte budget");
    Expect(g_largest_read_request <= 64,
           "inspection read past per-file byte budget");
    const auto* small = FindEntry(
        first, "lesson-assets/" + key + "/small.bin"
    );
    const auto* oversized = FindEntry(
        first, "lesson-assets/" + key + "/oversized.bin"
    );
    const auto* sibling_file = FindEntry(
        first, "lesson-assets/" + sibling + "/sibling.bin"
    );
    const auto* shared = FindEntry(first, "lesson-assets/shared/shared.bin");
    Expect(small != nullptr && small->node_type == "regular_file" &&
               small->bytes == 20 && small->sha256.size() == 64,
           "within-budget file lost its stable fingerprint");
    Expect(oversized != nullptr && oversized->node_type == "unexpected" &&
               oversized->bytes == 0 && oversized->sha256.empty(),
           "oversized file did not fail closed deterministically");
    Expect(sibling_file != nullptr &&
               sibling_file->node_type == "regular_file" &&
               sibling_file->bytes == 20,
           "sibling fingerprint did not share the aggregate budget");
    Expect(shared != nullptr && shared->node_type == "unexpected" &&
               shared->bytes == 0 && shared->sha256.empty(),
           "aggregate-exhausted file did not fail closed deterministically");

    const std::size_t first_read_bytes = g_read_bytes;
    ResetMutationInjection();
    SetLessonStorageHilFixtureReadCallbackForTest(ObservedRead);
    const auto repeated = InspectLessonStorageHilStorage(key, sibling);
    Expect(g_read_bytes == first_read_bytes,
           "repeated inspection changed deterministic read budget use");
    const auto* repeated_small = FindEntry(
        repeated, "lesson-assets/" + key + "/small.bin"
    );
    Expect(repeated_small != nullptr && small != nullptr &&
               repeated_small->sha256 == small->sha256,
           "within-budget SHA changed across identical inspections");
}

void TestInspectionBoundsDirectoryReadCalls() {
    ResetRoot();
    ResetMutationInjection();
    const std::string key = Key("hil-directory-budget", 1, 'e');
    for (int index = 0; index < 200; ++index) {
        Write(Leaf(key) / ("entry-" + std::to_string(index)), "x");
    }
    SetLessonStorageHilFixtureDirectoryReadCallbackForTest(
        ObserveDirectoryRead
    );

    const auto inspection = InspectLessonStorageHilStorage(key, "");
    Expect(inspection.truncated,
           "bounded directory inspection did not report truncation");
    Expect(g_directory_read_calls > 0 && g_directory_read_calls <= 24,
           "inspection exceeded the compiled directory read-call cap");
    Expect(inspection.entries.size() <= 21,
           "bounded directory scan emitted too many inspection entries");
}

}  // namespace

int main() {
    TestMountedBlankCardCreatesNamespaceHierarchy();
    TestValidationAndLeaseRefusalPrecedeFilesystemAccess();
    TestNestedDirectoryFixtureIsExactAndNonRecursive();
    TestLeafRegularFileFixtureRequiresExactMagic();
    TestNestedStageRollbackReportsEveryResidual();
    TestLeafStageRollbackReportsEveryResidual();
    TestPreservationSetUsesTwoPhaseCleanup();
    TestPreservationCleanupResumesOwnedPartialStates();
    TestPreservationStageRollbackReportsResidualTruthAndCleanupConverges();
    TestSecondPreservationSentinelRollbackCoversEveryCreatedNode();
    TestPreservationCleanupAcceptsAnExactlyEvictedPrimary();
    TestPreservationCleanupRequiresProofForEveryExistingLeaf();
    TestPreservationCleanupRejectsUnownedPartialStates();
    TestSymlinksAreRefusedWithoutFollowingTargets();
    TestInspectionIsReadOnlyBoundedSortedAndDataSensitive();
    TestInspectionNeverReadsPastPerFileOrAggregateBudgets();
    TestInspectionBoundsDirectoryReadCalls();
    ResetRoot();
    std::cout << "lesson storage HIL fixture host checks: " << g_checks << '\n';
    return 0;
}
