#include "lesson_storage_hil_fixture.h"

#include "lesson_asset_cache_evict.h"

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#ifdef ESP_PLATFORM
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef TBOT_LESSON_STORAGE_HIL_ROOT
#define TBOT_LESSON_STORAGE_HIL_ROOT "/sdcard/tbot/lesson-assets"
#endif

#ifndef TBOT_LESSON_STORAGE_HIL_INSPECTION_PER_FILE_BYTES
#define TBOT_LESSON_STORAGE_HIL_INSPECTION_PER_FILE_BYTES (4U * 1024U * 1024U)
#endif

#ifndef TBOT_LESSON_STORAGE_HIL_INSPECTION_AGGREGATE_BYTES
#define TBOT_LESSON_STORAGE_HIL_INSPECTION_AGGREGATE_BYTES (16U * 1024U * 1024U)
#endif

#ifndef TBOT_LESSON_STORAGE_HIL_DIRECTORY_READ_CALL_CAP
#define TBOT_LESSON_STORAGE_HIL_DIRECTORY_READ_CALL_CAP 64U
#endif

namespace {

constexpr char kNestedSentinelName[] = ".tbot-hil-nested";
constexpr char kPreservationSentinelName[] = ".tbot-hil-sentinel";
constexpr char kLeafMagic[] = "TBOT-HIL-LEAF-FIXTURE-V1\n";
constexpr char kPreservationPrimaryMagic[] =
    "TBOT-HIL-PRESERVATION-PRIMARY-V1\n";
constexpr char kPreservationSiblingMagic[] =
    "TBOT-HIL-PRESERVATION-SIBLING-V1\n";
constexpr std::size_t kInspectionDirectChildCap = 16;
constexpr std::size_t kInspectionRawNameMaxBytes = 48;
constexpr std::size_t kInspectionLabelMaxBytes = 384;
constexpr std::size_t kSha256BufferBytes = 512;
constexpr std::size_t kInspectionPerFileBytes =
    TBOT_LESSON_STORAGE_HIL_INSPECTION_PER_FILE_BYTES;
constexpr std::size_t kInspectionAggregateBytes =
    TBOT_LESSON_STORAGE_HIL_INSPECTION_AGGREGATE_BYTES;
constexpr std::size_t kDirectoryReadCallCap =
    TBOT_LESSON_STORAGE_HIL_DIRECTORY_READ_CALL_CAP;
static_assert(kInspectionPerFileBytes > 0);
static_assert(kInspectionAggregateBytes >= kInspectionPerFileBytes);
static_assert(kDirectoryReadCallCap >= kInspectionDirectChildCap + 3);

#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
LessonStorageHilFixtureMkdirCallback g_mkdir_callback = nullptr;
LessonStorageHilFixtureFsyncCallback g_fsync_callback = nullptr;
LessonStorageHilFixtureWriteCallback g_write_callback = nullptr;
LessonStorageHilFixtureOpenCallback g_open_callback = nullptr;
LessonStorageHilFixtureReadCallback g_read_callback = nullptr;
LessonStorageHilFixtureDirectoryReadCallback g_directory_read_callback = nullptr;
LessonStorageHilFixtureInspectFailureCallback g_inspect_failure_callback = nullptr;
LessonStorageHilFixtureRemoveCallback g_unlink_callback = nullptr;
LessonStorageHilFixtureRemoveCallback g_rmdir_callback = nullptr;
#endif

enum class NodeKind {
    kMissing,
    kRegularFile,
    kDirectory,
    kUnexpected,
    kIoFailed,
};

enum class FixtureState {
    kMissing,
    kComplete,
    kEmptyPartial,
    kUnexpected,
    kSentinelMismatch,
    kIoFailed,
};

struct ValidatedKeys {
    bool valid;
    LessonStorageHilFixtureCode code;
    std::string slug;
};

struct ParentCreation {
    bool root_created = false;
    bool slug_created = false;
};

struct CreatedNode {
    const std::string* path;
    NodeKind kind;
    bool created;
    bool creation_attempt_failed = false;
};

struct RollbackResult {
    bool all_created_nodes_removed_and_reverified;
};

struct InspectionByteBudget {
    std::size_t remaining = kInspectionAggregateBytes;
};

std::string JoinPath(const std::string& parent, const std::string& child) {
    return parent + "/" + child;
}

std::string RootPath() {
    return TBOT_LESSON_STORAGE_HIL_ROOT;
}

std::string SlugFromCacheKey(const std::string& cache_key) {
    return cache_key.substr(0, cache_key.find('/'));
}

std::string VersionFromCacheKey(const std::string& cache_key) {
    const std::size_t version_start = cache_key.find('/') + 2;
    const std::size_t version_end = cache_key.find('-', version_start);
    return cache_key.substr(version_start, version_end - version_start);
}

std::string SlugPath(const std::string& slug) {
    return JoinPath(RootPath(), slug);
}

std::string LeafPath(const std::string& cache_key) {
    return JoinPath(RootPath(), cache_key);
}

bool IsHilCacheKey(const std::string& cache_key, std::string* slug_out) {
    if (!IsCanonicalLessonCacheKey(cache_key)) {
        return false;
    }
    const std::string slug = SlugFromCacheKey(cache_key);
    if (slug.compare(0, 4, "hil-") != 0) {
        return false;
    }
    if (slug_out != nullptr) {
        *slug_out = slug;
    }
    return true;
}

int RemoveFixtureFile(const std::string& path) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_unlink_callback != nullptr) {
        return g_unlink_callback(path.c_str());
    }
#endif
    return unlink(path.c_str());
}

int CreateFixtureDirectory(const std::string& path) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_mkdir_callback != nullptr) {
        return g_mkdir_callback(path.c_str());
    }
#endif
    return mkdir(path.c_str(), 0755);
}

int SyncFixtureFile(int descriptor) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_fsync_callback != nullptr) {
        return g_fsync_callback(descriptor);
    }
#endif
    return fsync(descriptor);
}

ssize_t WriteFixtureBytes(
    int descriptor,
    const void* bytes,
    std::size_t length
) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_write_callback != nullptr) {
        return g_write_callback(descriptor, bytes, length);
    }
#endif
    return write(descriptor, bytes, length);
}

int OpenFixtureFile(const std::string& path, int flags, mode_t mode) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_open_callback != nullptr) {
        return g_open_callback(path.c_str(), flags, mode);
    }
#endif
    return open(path.c_str(), flags, mode);
}

std::size_t ReadFixtureBytes(
    void* bytes,
    std::size_t length,
    FILE* file
) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_read_callback != nullptr) {
        return g_read_callback(bytes, length, file);
    }
#endif
    return std::fread(bytes, 1, length, file);
}

int RemoveFixtureDirectory(const std::string& path) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_rmdir_callback != nullptr) {
        return g_rmdir_callback(path.c_str());
    }
#endif
    return rmdir(path.c_str());
}

ValidatedKeys ValidateKeys(
    const std::string& cache_key,
    LessonStorageHilFixture fixture,
    const std::string& sibling_cache_key
) {
    std::string slug;
    if (!IsHilCacheKey(cache_key, &slug)) {
        return {false, LessonStorageHilFixtureCode::kInvalidCacheKey, ""};
    }
    if (fixture != LessonStorageHilFixture::kPreservationSet) {
        if (!sibling_cache_key.empty()) {
            return {false, LessonStorageHilFixtureCode::kInvalidSibling, slug};
        }
        return {true, LessonStorageHilFixtureCode::kStaged, slug};
    }

    std::string sibling_slug;
    if (!IsHilCacheKey(sibling_cache_key, &sibling_slug) ||
        sibling_slug != slug ||
        VersionFromCacheKey(sibling_cache_key) == VersionFromCacheKey(cache_key)) {
        return {false, LessonStorageHilFixtureCode::kInvalidSibling, slug};
    }
    return {true, LessonStorageHilFixtureCode::kStaged, slug};
}

LessonStorageHilFixtureResult Result(
    LessonStorageHilFixtureCode code,
    bool changed,
    const std::string& cache_key,
    const std::string& sibling_cache_key
) {
    return {code, changed, cache_key, sibling_cache_key};
}

LessonStorageHilFixtureResult ValidationFailure(
    LessonStorageHilFixtureCode code,
    const std::string& cache_key
) {
    if (code == LessonStorageHilFixtureCode::kInvalidCacheKey) {
        return Result(code, false, "", "");
    }
    return Result(code, false, cache_key, "");
}

NodeKind ReadNodeKind(const std::string& path, struct stat* metadata = nullptr) {
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
    if (g_inspect_failure_callback != nullptr &&
        g_inspect_failure_callback(path.c_str())) {
        return NodeKind::kIoFailed;
    }
#endif
#ifndef ESP_PLATFORM
    // ESP-IDF FAT VFS cannot create symlinks. Native tests can, so reject the
    // exact component before stat follows it into an external fixture target.
    char link_probe = '\0';
    if (readlink(path.c_str(), &link_probe, sizeof(link_probe)) >= 0) {
        return NodeKind::kUnexpected;
    }
    if (errno != EINVAL && errno != ENOENT) {
        return NodeKind::kIoFailed;
    }
#endif
    struct stat local_metadata {};
    if (stat(path.c_str(), &local_metadata) != 0) {
        return errno == ENOENT ? NodeKind::kMissing : NodeKind::kIoFailed;
    }
    if (metadata != nullptr) {
        *metadata = local_metadata;
    }
    if (S_ISREG(local_metadata.st_mode)) {
        return NodeKind::kRegularFile;
    }
    if (S_ISDIR(local_metadata.st_mode)) {
        return NodeKind::kDirectory;
    }
    return NodeKind::kUnexpected;
}

bool ReadDirectoryNames(
    const std::string& path,
    std::vector<std::string>* names,
    bool* truncated,
    std::size_t cap
) {
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
        return false;
    }
    std::size_t read_calls = 0;
    bool reached_end = false;
    int scan_errno = 0;
    while (read_calls < kDirectoryReadCallCap) {
        ++read_calls;
#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
        if (g_directory_read_callback != nullptr) {
            g_directory_read_callback();
        }
#endif
        errno = 0;
        dirent* entry = readdir(directory);
#ifdef ESP_PLATFORM
        esp_task_wdt_reset();
        taskYIELD();
#endif
        if (entry == nullptr) {
            scan_errno = errno;
            reached_end = true;
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string name(entry->d_name);
        if (names->size() < cap) {
            names->push_back(std::move(name));
            continue;
        }
        *truncated = true;
        auto largest = std::max_element(names->begin(), names->end());
        if (largest != names->end() && name < *largest) {
            *largest = std::move(name);
        }
    }
    if (!reached_end) {
        *truncated = true;
    }
    std::sort(names->begin(), names->end());
    const bool close_ok = closedir(directory) == 0;
    return scan_errno == 0 && close_ok;
}

bool DirectoryIsEmpty(const std::string& path, bool* empty) {
    std::vector<std::string> names;
    bool truncated = false;
    if (!ReadDirectoryNames(path, &names, &truncated, 1)) {
        return false;
    }
    *empty = names.empty();
    return true;
}

FixtureState InspectNestedFixture(const std::string& leaf_path) {
    const NodeKind leaf_kind = ReadNodeKind(leaf_path);
    if (leaf_kind == NodeKind::kMissing) {
        return FixtureState::kMissing;
    }
    if (leaf_kind == NodeKind::kIoFailed) {
        return FixtureState::kIoFailed;
    }
    if (leaf_kind != NodeKind::kDirectory) {
        return FixtureState::kUnexpected;
    }

    std::vector<std::string> names;
    bool truncated = false;
    if (!ReadDirectoryNames(leaf_path, &names, &truncated, 2)) {
        return FixtureState::kIoFailed;
    }
    if (names.empty()) {
        return FixtureState::kEmptyPartial;
    }
    if (truncated || names.size() != 1 || names[0] != kNestedSentinelName) {
        return FixtureState::kUnexpected;
    }
    const std::string sentinel_path = JoinPath(leaf_path, kNestedSentinelName);
    const NodeKind sentinel_kind = ReadNodeKind(sentinel_path);
    if (sentinel_kind == NodeKind::kIoFailed) {
        return FixtureState::kIoFailed;
    }
    if (sentinel_kind != NodeKind::kDirectory) {
        return FixtureState::kUnexpected;
    }
    bool empty = false;
    if (!DirectoryIsEmpty(sentinel_path, &empty)) {
        return FixtureState::kIoFailed;
    }
    return empty ? FixtureState::kComplete : FixtureState::kUnexpected;
}

FixtureState ReadExactFileState(const std::string& path, const char* magic) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return FixtureState::kIoFailed;
    }
    const std::size_t expected = std::strlen(magic);
    std::array<unsigned char, 96> bytes {};
    if (expected + 1 > bytes.size()) {
        std::fclose(file);
        return FixtureState::kIoFailed;
    }
    const std::size_t read = std::fread(bytes.data(), 1, expected + 1, file);
    const bool read_ok = !std::ferror(file);
    const bool close_ok = std::fclose(file) == 0;
    if (!read_ok || !close_ok) {
        return FixtureState::kIoFailed;
    }
    return read == expected && std::memcmp(bytes.data(), magic, expected) == 0
               ? FixtureState::kComplete
               : FixtureState::kSentinelMismatch;
}

FixtureState InspectLeafFileFixture(const std::string& leaf_path) {
    const NodeKind kind = ReadNodeKind(leaf_path);
    if (kind == NodeKind::kMissing) {
        return FixtureState::kMissing;
    }
    if (kind == NodeKind::kIoFailed) {
        return FixtureState::kIoFailed;
    }
    if (kind != NodeKind::kRegularFile) {
        return FixtureState::kUnexpected;
    }
    return ReadExactFileState(leaf_path, kLeafMagic);
}

FixtureState InspectPreservationLeaf(
    const std::string& leaf_path,
    const char* magic
) {
    const NodeKind leaf_kind = ReadNodeKind(leaf_path);
    if (leaf_kind == NodeKind::kMissing) {
        return FixtureState::kMissing;
    }
    if (leaf_kind == NodeKind::kIoFailed) {
        return FixtureState::kIoFailed;
    }
    if (leaf_kind != NodeKind::kDirectory) {
        return FixtureState::kUnexpected;
    }

    std::vector<std::string> names;
    bool truncated = false;
    if (!ReadDirectoryNames(leaf_path, &names, &truncated, 2)) {
        return FixtureState::kIoFailed;
    }
    if (names.empty()) {
        return FixtureState::kEmptyPartial;
    }
    if (truncated || names.size() != 1 || names[0] != kPreservationSentinelName) {
        return FixtureState::kUnexpected;
    }
    const std::string sentinel_path = JoinPath(leaf_path, kPreservationSentinelName);
    const NodeKind sentinel_kind = ReadNodeKind(sentinel_path);
    if (sentinel_kind == NodeKind::kIoFailed) {
        return FixtureState::kIoFailed;
    }
    if (sentinel_kind != NodeKind::kRegularFile) {
        return FixtureState::kUnexpected;
    }
    return ReadExactFileState(sentinel_path, magic);
}

LessonStorageHilFixtureCode CodeForState(FixtureState state) {
    if (state == FixtureState::kSentinelMismatch) {
        return LessonStorageHilFixtureCode::kSentinelMismatch;
    }
    if (state == FixtureState::kIoFailed) {
        return LessonStorageHilFixtureCode::kIoFailed;
    }
    return LessonStorageHilFixtureCode::kUnexpectedExistingNode;
}

FixtureState PreservationStageFailure(
    FixtureState first,
    FixtureState second
) {
    const auto is_actual_failure = [](FixtureState state) {
        return state != FixtureState::kMissing && state != FixtureState::kComplete;
    };
    if (is_actual_failure(first)) {
        return first;
    }
    if (is_actual_failure(second)) {
        return second;
    }
    return FixtureState::kUnexpected;
}

LessonStorageHilFixtureCode ValidateParents(
    const std::string& slug,
    bool* root_missing,
    bool* slug_missing
) {
    const NodeKind root_kind = ReadNodeKind(RootPath());
    if (root_kind == NodeKind::kIoFailed) {
        return LessonStorageHilFixtureCode::kIoFailed;
    }
    if (root_kind != NodeKind::kMissing && root_kind != NodeKind::kDirectory) {
        return LessonStorageHilFixtureCode::kUnexpectedExistingNode;
    }
    *root_missing = root_kind == NodeKind::kMissing;
    if (*root_missing) {
        *slug_missing = true;
        return LessonStorageHilFixtureCode::kStaged;
    }
    const NodeKind slug_kind = ReadNodeKind(SlugPath(slug));
    if (slug_kind == NodeKind::kIoFailed) {
        return LessonStorageHilFixtureCode::kIoFailed;
    }
    if (slug_kind != NodeKind::kMissing && slug_kind != NodeKind::kDirectory) {
        return LessonStorageHilFixtureCode::kUnexpectedExistingNode;
    }
    *slug_missing = slug_kind == NodeKind::kMissing;
    return LessonStorageHilFixtureCode::kStaged;
}

bool EnsureParents(
    const std::string& slug,
    bool root_missing,
    bool slug_missing,
    ParentCreation* creation
) {
    if (root_missing) {
        if (CreateFixtureDirectory(RootPath()) != 0) {
            return false;
        }
        creation->root_created = true;
    }
    if (slug_missing) {
        if (CreateFixtureDirectory(SlugPath(slug)) != 0) {
            return false;
        }
        creation->slug_created = true;
    }
    return true;
}

bool WriteExactFile(
    const std::string& path,
    const char* bytes,
    bool* created
) {
    const int descriptor =
        OpenFixtureFile(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        return false;
    }
    *created = true;
    const std::size_t length = std::strlen(bytes);
    std::size_t written = 0;
    bool ok = true;
    while (written < length) {
        const ssize_t result = WriteFixtureBytes(
            descriptor, bytes + written, length - written
        );
        if (result <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(result);
    }
    if (ok && SyncFixtureFile(descriptor) != 0) {
        ok = false;
    }
    if (close(descriptor) != 0) {
        ok = false;
    }
    return ok;
}

RollbackResult RollBackCreatedNodes(
    std::initializer_list<CreatedNode> nodes
) {
    for (const CreatedNode& node : nodes) {
        if (!node.created) {
            continue;
        }
        if (node.kind == NodeKind::kRegularFile) {
            RemoveFixtureFile(*node.path);
        } else {
            RemoveFixtureDirectory(*node.path);
        }
    }
    bool all_missing = true;
    for (const CreatedNode& node : nodes) {
        if ((node.created || node.creation_attempt_failed) &&
            ReadNodeKind(*node.path) != NodeKind::kMissing) {
            all_missing = false;
        }
    }
    return {all_missing};
}

LessonStorageHilFixtureResult StageNested(
    const std::string& cache_key,
    const std::string& slug
) {
    bool root_missing = false;
    bool slug_missing = false;
    const auto parent_code = ValidateParents(slug, &root_missing, &slug_missing);
    if (parent_code != LessonStorageHilFixtureCode::kStaged) {
        return Result(parent_code, false, cache_key, "");
    }
    const std::string leaf_path = LeafPath(cache_key);
    const FixtureState state = root_missing || slug_missing
                                   ? FixtureState::kMissing
                                   : InspectNestedFixture(leaf_path);
    if (state == FixtureState::kComplete) {
        return Result(LessonStorageHilFixtureCode::kStaged, false, cache_key, "");
    }
    if (state != FixtureState::kMissing) {
        return Result(CodeForState(state), false, cache_key, "");
    }

    ParentCreation creation;
    const std::string slug_path = SlugPath(slug);
    const std::string root_path = RootPath();
    if (!EnsureParents(slug, root_missing, slug_missing, &creation)) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&slug_path,
             NodeKind::kDirectory,
             creation.slug_created,
             slug_missing && !creation.slug_created &&
                 (!root_missing || creation.root_created)},
            {&root_path,
             NodeKind::kDirectory,
             creation.root_created,
             root_missing && !creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            ""
        );
    }
    if (CreateFixtureDirectory(leaf_path) != 0) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&leaf_path, NodeKind::kDirectory, false, true},
            {&slug_path, NodeKind::kDirectory, creation.slug_created},
            {&root_path, NodeKind::kDirectory, creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            ""
        );
    }
    const std::string sentinel_path = JoinPath(leaf_path, kNestedSentinelName);
    if (CreateFixtureDirectory(sentinel_path) != 0) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&sentinel_path, NodeKind::kDirectory, false, true},
            {&leaf_path, NodeKind::kDirectory, true},
            {&slug_path, NodeKind::kDirectory, creation.slug_created},
            {&root_path, NodeKind::kDirectory, creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            ""
        );
    }
    return Result(LessonStorageHilFixtureCode::kStaged, true, cache_key, "");
}

LessonStorageHilFixtureResult StageLeafFile(
    const std::string& cache_key,
    const std::string& slug
) {
    bool root_missing = false;
    bool slug_missing = false;
    const auto parent_code = ValidateParents(slug, &root_missing, &slug_missing);
    if (parent_code != LessonStorageHilFixtureCode::kStaged) {
        return Result(parent_code, false, cache_key, "");
    }
    const std::string leaf_path = LeafPath(cache_key);
    const FixtureState state = root_missing || slug_missing
                                   ? FixtureState::kMissing
                                   : InspectLeafFileFixture(leaf_path);
    if (state == FixtureState::kComplete) {
        return Result(LessonStorageHilFixtureCode::kStaged, false, cache_key, "");
    }
    if (state != FixtureState::kMissing) {
        return Result(CodeForState(state), false, cache_key, "");
    }

    ParentCreation creation;
    const std::string slug_path = SlugPath(slug);
    const std::string root_path = RootPath();
    if (!EnsureParents(slug, root_missing, slug_missing, &creation)) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&slug_path,
             NodeKind::kDirectory,
             creation.slug_created,
             slug_missing && !creation.slug_created &&
                 (!root_missing || creation.root_created)},
            {&root_path,
             NodeKind::kDirectory,
             creation.root_created,
             root_missing && !creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            ""
        );
    }
    bool leaf_created = false;
    if (!WriteExactFile(leaf_path, kLeafMagic, &leaf_created)) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&leaf_path, NodeKind::kRegularFile, leaf_created, !leaf_created},
            {&slug_path, NodeKind::kDirectory, creation.slug_created},
            {&root_path, NodeKind::kDirectory, creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            ""
        );
    }
    return Result(LessonStorageHilFixtureCode::kStaged, true, cache_key, "");
}

LessonStorageHilFixtureResult StagePreservation(
    const std::string& cache_key,
    const std::string& sibling_cache_key,
    const std::string& slug
) {
    bool root_missing = false;
    bool slug_missing = false;
    const auto parent_code = ValidateParents(slug, &root_missing, &slug_missing);
    if (parent_code != LessonStorageHilFixtureCode::kStaged) {
        return Result(parent_code, false, cache_key, sibling_cache_key);
    }
    const std::string first_leaf = LeafPath(cache_key);
    const std::string second_leaf = LeafPath(sibling_cache_key);
    const FixtureState first_state = root_missing || slug_missing
                                         ? FixtureState::kMissing
                                         : InspectPreservationLeaf(
                                               first_leaf, kPreservationPrimaryMagic
                                           );
    const FixtureState second_state = root_missing || slug_missing
                                          ? FixtureState::kMissing
                                          : InspectPreservationLeaf(
                                                second_leaf,
                                                kPreservationSiblingMagic
                                            );
    if (first_state == FixtureState::kComplete &&
        second_state == FixtureState::kComplete) {
        return Result(
            LessonStorageHilFixtureCode::kStaged,
            false,
            cache_key,
            sibling_cache_key
        );
    }
    if (first_state != FixtureState::kMissing ||
        second_state != FixtureState::kMissing) {
        const FixtureState failure =
            PreservationStageFailure(first_state, second_state);
        return Result(CodeForState(failure), false, cache_key, sibling_cache_key);
    }

    ParentCreation creation;
    const std::string slug_path = SlugPath(slug);
    const std::string root_path = RootPath();
    const std::string first_sentinel =
        JoinPath(first_leaf, kPreservationSentinelName);
    const std::string second_sentinel =
        JoinPath(second_leaf, kPreservationSentinelName);
    if (!EnsureParents(slug, root_missing, slug_missing, &creation)) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&slug_path,
             NodeKind::kDirectory,
             creation.slug_created,
             slug_missing && !creation.slug_created &&
                 (!root_missing || creation.root_created)},
            {&root_path,
             NodeKind::kDirectory,
             creation.root_created,
             root_missing && !creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            sibling_cache_key
        );
    }
    if (CreateFixtureDirectory(first_leaf) != 0) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&first_leaf, NodeKind::kDirectory, false, true},
            {&slug_path, NodeKind::kDirectory, creation.slug_created},
            {&root_path, NodeKind::kDirectory, creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            sibling_cache_key
        );
    }
    bool first_sentinel_created = false;
    if (!WriteExactFile(
            first_sentinel,
            kPreservationPrimaryMagic,
            &first_sentinel_created
        )) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&first_sentinel,
             NodeKind::kRegularFile,
             first_sentinel_created,
             !first_sentinel_created},
            {&first_leaf, NodeKind::kDirectory, true},
            {&slug_path, NodeKind::kDirectory, creation.slug_created},
            {&root_path, NodeKind::kDirectory, creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            sibling_cache_key
        );
    }
    if (CreateFixtureDirectory(second_leaf) != 0) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&second_leaf, NodeKind::kDirectory, false, true},
            {&first_sentinel, NodeKind::kRegularFile, true},
            {&first_leaf, NodeKind::kDirectory, true},
            {&slug_path, NodeKind::kDirectory, creation.slug_created},
            {&root_path, NodeKind::kDirectory, creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            sibling_cache_key
        );
    }
    bool second_sentinel_created = false;
    if (!WriteExactFile(
            second_sentinel,
            kPreservationSiblingMagic,
            &second_sentinel_created
        )) {
        const RollbackResult rollback = RollBackCreatedNodes({
            {&second_sentinel,
             NodeKind::kRegularFile,
             second_sentinel_created,
             !second_sentinel_created},
            {&second_leaf, NodeKind::kDirectory, true},
            {&first_sentinel, NodeKind::kRegularFile, true},
            {&first_leaf, NodeKind::kDirectory, true},
            {&slug_path, NodeKind::kDirectory, creation.slug_created},
            {&root_path, NodeKind::kDirectory, creation.root_created},
        });
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            !rollback.all_created_nodes_removed_and_reverified,
            cache_key,
            sibling_cache_key
        );
    }
    return Result(
        LessonStorageHilFixtureCode::kStaged,
        true,
        cache_key,
        sibling_cache_key
    );
}

LessonStorageHilFixtureResult CleanupNested(
    const std::string& cache_key,
    const std::string& slug
) {
    bool root_missing = false;
    bool slug_missing = false;
    const auto parent_code = ValidateParents(slug, &root_missing, &slug_missing);
    if (parent_code != LessonStorageHilFixtureCode::kStaged) {
        return Result(parent_code, false, cache_key, "");
    }
    if (root_missing || slug_missing) {
        return Result(LessonStorageHilFixtureCode::kCleaned, false, cache_key, "");
    }
    const std::string leaf_path = LeafPath(cache_key);
    const FixtureState state = InspectNestedFixture(leaf_path);
    if (state == FixtureState::kMissing) {
        return Result(LessonStorageHilFixtureCode::kCleaned, false, cache_key, "");
    }
    if (state == FixtureState::kEmptyPartial) {
        return Result(
            LessonStorageHilFixtureCode::kUnexpectedExistingNode,
            false,
            cache_key,
            ""
        );
    }
    if (state != FixtureState::kComplete) {
        return Result(CodeForState(state), false, cache_key, "");
    }
    const std::string sentinel_path = JoinPath(leaf_path, kNestedSentinelName);
    if (RemoveFixtureDirectory(sentinel_path) != 0) {
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
    }
    if (RemoveFixtureDirectory(leaf_path) != 0) {
        return Result(LessonStorageHilFixtureCode::kIoFailed, true, cache_key, "");
    }
    return Result(LessonStorageHilFixtureCode::kCleaned, true, cache_key, "");
}

LessonStorageHilFixtureResult CleanupLeafFile(
    const std::string& cache_key,
    const std::string& slug
) {
    bool root_missing = false;
    bool slug_missing = false;
    const auto parent_code = ValidateParents(slug, &root_missing, &slug_missing);
    if (parent_code != LessonStorageHilFixtureCode::kStaged) {
        return Result(parent_code, false, cache_key, "");
    }
    if (root_missing || slug_missing) {
        return Result(LessonStorageHilFixtureCode::kCleaned, false, cache_key, "");
    }
    const std::string leaf_path = LeafPath(cache_key);
    const FixtureState state = InspectLeafFileFixture(leaf_path);
    if (state == FixtureState::kMissing) {
        return Result(LessonStorageHilFixtureCode::kCleaned, false, cache_key, "");
    }
    if (state != FixtureState::kComplete) {
        return Result(CodeForState(state), false, cache_key, "");
    }
    if (RemoveFixtureFile(leaf_path) != 0) {
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
    }
    return Result(LessonStorageHilFixtureCode::kCleaned, true, cache_key, "");
}

LessonStorageHilFixtureResult CleanupPreservation(
    const std::string& cache_key,
    const std::string& sibling_cache_key,
    const std::string& slug
) {
    bool root_missing = false;
    bool slug_missing = false;
    const auto parent_code = ValidateParents(slug, &root_missing, &slug_missing);
    if (parent_code != LessonStorageHilFixtureCode::kStaged) {
        return Result(parent_code, false, cache_key, sibling_cache_key);
    }
    if (root_missing || slug_missing) {
        return Result(
            LessonStorageHilFixtureCode::kCleaned,
            false,
            cache_key,
            sibling_cache_key
        );
    }
    const std::string first_leaf = LeafPath(cache_key);
    const std::string second_leaf = LeafPath(sibling_cache_key);
    const FixtureState first_state =
        InspectPreservationLeaf(first_leaf, kPreservationPrimaryMagic);
    const FixtureState second_state =
        InspectPreservationLeaf(second_leaf, kPreservationSiblingMagic);
    if (first_state == FixtureState::kMissing &&
        second_state == FixtureState::kMissing) {
        return Result(
            LessonStorageHilFixtureCode::kCleaned,
            false,
            cache_key,
            sibling_cache_key
        );
    }
    const auto is_attested_or_missing = [](FixtureState state) {
        return state == FixtureState::kMissing ||
               state == FixtureState::kComplete;
    };
    if (!is_attested_or_missing(first_state) ||
        !is_attested_or_missing(second_state)) {
        const FixtureState failure = !is_attested_or_missing(first_state)
                                         ? first_state
                                         : second_state;
        return Result(CodeForState(failure), false, cache_key, sibling_cache_key);
    }
    bool changed = false;
    const std::string first_sentinel =
        JoinPath(first_leaf, kPreservationSentinelName);
    const std::string second_sentinel =
        JoinPath(second_leaf, kPreservationSentinelName);
    if (first_state == FixtureState::kComplete) {
        if (RemoveFixtureFile(first_sentinel) != 0) {
            return Result(
                LessonStorageHilFixtureCode::kIoFailed,
                false,
                cache_key,
                sibling_cache_key
            );
        }
        changed = true;
    }
    if (second_state == FixtureState::kComplete) {
        if (RemoveFixtureFile(second_sentinel) != 0) {
            return Result(
                LessonStorageHilFixtureCode::kIoFailed,
                changed,
                cache_key,
                sibling_cache_key
            );
        }
        changed = true;
    }
    if (first_state != FixtureState::kMissing) {
        if (RemoveFixtureDirectory(first_leaf) != 0) {
            return Result(
                LessonStorageHilFixtureCode::kIoFailed,
                changed,
                cache_key,
                sibling_cache_key
            );
        }
        changed = true;
    }
    if (second_state != FixtureState::kMissing &&
        RemoveFixtureDirectory(second_leaf) != 0) {
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            changed,
            cache_key,
            sibling_cache_key
        );
    }
    changed = changed || second_state != FixtureState::kMissing;
    return Result(
        LessonStorageHilFixtureCode::kCleaned,
        changed,
        cache_key,
        sibling_cache_key
    );
}

bool HashFileSha256(
    const std::string& path,
    std::size_t expected_bytes,
    std::size_t* bytes,
    std::string* sha256
) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
#if MBEDTLS_VERSION_MAJOR >= 3
    int result = mbedtls_sha256_starts(&context, 0);
#else
    int result = mbedtls_sha256_starts_ret(&context, 0);
#endif
    std::array<unsigned char, kSha256BufferBytes> buffer {};
    std::size_t total = 0;
    while (result == 0 && total < expected_bytes) {
        const std::size_t request =
            std::min(buffer.size(), expected_bytes - total);
        const std::size_t read = ReadFixtureBytes(buffer.data(), request, file);
        if (read > request) {
            result = -1;
            break;
        }
        if (read > 0) {
            total += read;
#if MBEDTLS_VERSION_MAJOR >= 3
            result = mbedtls_sha256_update(&context, buffer.data(), read);
#else
            result = mbedtls_sha256_update_ret(&context, buffer.data(), read);
#endif
        }
#ifdef ESP_PLATFORM
        esp_task_wdt_reset();
        taskYIELD();
#endif
        if (read != request || std::ferror(file)) {
            result = -1;
        }
    }
    struct stat final_metadata {};
    if (result == 0 &&
        (fstat(fileno(file), &final_metadata) != 0 ||
         !S_ISREG(final_metadata.st_mode) || final_metadata.st_size < 0 ||
         static_cast<std::size_t>(final_metadata.st_size) != expected_bytes)) {
        result = -1;
    }
    std::array<unsigned char, 32> digest {};
    if (result == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
        result = mbedtls_sha256_finish(&context, digest.data());
#else
        result = mbedtls_sha256_finish_ret(&context, digest.data());
#endif
    }
    mbedtls_sha256_free(&context);
    if (std::fclose(file) != 0) {
        result = -1;
    }
    if (result != 0) {
        return false;
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        encoded[index * 2] = kHex[digest[index] >> 4U];
        encoded[index * 2 + 1] = kHex[digest[index] & 0x0FU];
    }
    *bytes = total;
    *sha256 = std::move(encoded);
    return true;
}

LessonStorageHilInspectionEntry InspectEntry(
    const std::string& path,
    const std::string& label,
    InspectionByteBudget* budget,
    bool* truncated
) {
    struct stat metadata {};
    const NodeKind kind = ReadNodeKind(path, &metadata);
    if (kind == NodeKind::kMissing) {
        return {label, "missing", 0, ""};
    }
    if (kind == NodeKind::kDirectory) {
        return {label, "directory", 0, ""};
    }
    if (kind != NodeKind::kRegularFile) {
        return {label, "unexpected", 0, ""};
    }
    if (metadata.st_size < 0) {
        *truncated = true;
        return {label, "unexpected", 0, ""};
    }
    const std::size_t declared_bytes =
        static_cast<std::size_t>(metadata.st_size);
    if (declared_bytes > kInspectionPerFileBytes ||
        declared_bytes > budget->remaining) {
        *truncated = true;
        return {label, "unexpected", 0, ""};
    }
    budget->remaining -= declared_bytes;
    std::size_t bytes = 0;
    std::string sha256;
    if (!HashFileSha256(path, declared_bytes, &bytes, &sha256)) {
        *truncated = true;
        return {label, "unexpected", 0, ""};
    }
    return {label, "regular_file", bytes, std::move(sha256)};
}

bool EncodeLabelComponent(const std::string& raw, std::string* encoded) {
    if (raw.size() > kInspectionRawNameMaxBytes) {
        return false;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    encoded->clear();
    encoded->reserve(raw.size() * 3);
    for (const unsigned char byte : raw) {
        const bool safe = (byte >= 'A' && byte <= 'Z') ||
                          (byte >= 'a' && byte <= 'z') ||
                          (byte >= '0' && byte <= '9') || byte == '.' ||
                          byte == '_' || byte == '-';
        if (safe) {
            encoded->push_back(static_cast<char>(byte));
            continue;
        }
        encoded->push_back('%');
        encoded->push_back(kHex[byte >> 4U]);
        encoded->push_back(kHex[byte & 0x0FU]);
    }
    return true;
}

void InspectPathAndDirectChildren(
    const std::string& path,
    const std::string& label,
    LessonStorageHilInspection* inspection,
    InspectionByteBudget* budget
) {
    inspection->entries.push_back(
        InspectEntry(path, label, budget, &inspection->truncated)
    );
    if (inspection->entries.back().node_type != "directory") {
        return;
    }
    std::vector<std::string> names;
    bool truncated = false;
    if (!ReadDirectoryNames(
            path, &names, &truncated, kInspectionDirectChildCap
        )) {
        inspection->entries.back().node_type = "unexpected";
        inspection->truncated = true;
        return;
    }
    inspection->truncated = inspection->truncated || truncated;
    for (const auto& name : names) {
        std::string encoded_name;
        if (!EncodeLabelComponent(name, &encoded_name) ||
            label.size() + 1 + encoded_name.size() > kInspectionLabelMaxBytes) {
            inspection->truncated = true;
            continue;
        }
        inspection->entries.push_back(
            InspectEntry(
                JoinPath(path, name),
                JoinPath(label, encoded_name),
                budget,
                &inspection->truncated
            )
        );
    }
}

}  // namespace

#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
void SetLessonStorageHilFixtureMkdirCallbackForTest(
    LessonStorageHilFixtureMkdirCallback callback
) {
    g_mkdir_callback = callback;
}

void SetLessonStorageHilFixtureFsyncCallbackForTest(
    LessonStorageHilFixtureFsyncCallback callback
) {
    g_fsync_callback = callback;
}

void SetLessonStorageHilFixtureWriteCallbackForTest(
    LessonStorageHilFixtureWriteCallback callback
) {
    g_write_callback = callback;
}

void SetLessonStorageHilFixtureOpenCallbackForTest(
    LessonStorageHilFixtureOpenCallback callback
) {
    g_open_callback = callback;
}

void SetLessonStorageHilFixtureReadCallbackForTest(
    LessonStorageHilFixtureReadCallback callback
) {
    g_read_callback = callback;
}

void SetLessonStorageHilFixtureDirectoryReadCallbackForTest(
    LessonStorageHilFixtureDirectoryReadCallback callback
) {
    g_directory_read_callback = callback;
}

void SetLessonStorageHilFixtureInspectFailureCallbackForTest(
    LessonStorageHilFixtureInspectFailureCallback callback
) {
    g_inspect_failure_callback = callback;
}

void SetLessonStorageHilFixtureUnlinkCallbackForTest(
    LessonStorageHilFixtureRemoveCallback callback
) {
    g_unlink_callback = callback;
}

void SetLessonStorageHilFixtureRmdirCallbackForTest(
    LessonStorageHilFixtureRemoveCallback callback
) {
    g_rmdir_callback = callback;
}
#endif

LessonStorageHilFixtureResult StageLessonStorageHilFixture(
    const LessonAssetMutationLease& mutation,
    const std::string& cache_key,
    LessonStorageHilFixture fixture,
    const std::string& sibling_cache_key
) {
    if (!mutation) {
        return Result(LessonStorageHilFixtureCode::kLeaseRefused, false, "", "");
    }
    const ValidatedKeys keys = ValidateKeys(cache_key, fixture, sibling_cache_key);
    if (!keys.valid) {
        return ValidationFailure(keys.code, cache_key);
    }
    switch (fixture) {
        case LessonStorageHilFixture::kNestedDirectory:
            return StageNested(cache_key, keys.slug);
        case LessonStorageHilFixture::kLeafRegularFile:
            return StageLeafFile(cache_key, keys.slug);
        case LessonStorageHilFixture::kPreservationSet:
            return StagePreservation(cache_key, sibling_cache_key, keys.slug);
    }
    return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
}

LessonStorageHilFixtureResult CleanupLessonStorageHilFixture(
    const LessonAssetMutationLease& mutation,
    const std::string& cache_key,
    LessonStorageHilFixture fixture,
    const std::string& sibling_cache_key
) {
    if (!mutation) {
        return Result(LessonStorageHilFixtureCode::kLeaseRefused, false, "", "");
    }
    const ValidatedKeys keys = ValidateKeys(cache_key, fixture, sibling_cache_key);
    if (!keys.valid) {
        return ValidationFailure(keys.code, cache_key);
    }
    switch (fixture) {
        case LessonStorageHilFixture::kNestedDirectory:
            return CleanupNested(cache_key, keys.slug);
        case LessonStorageHilFixture::kLeafRegularFile:
            return CleanupLeafFile(cache_key, keys.slug);
        case LessonStorageHilFixture::kPreservationSet:
            return CleanupPreservation(cache_key, sibling_cache_key, keys.slug);
    }
    return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
}

LessonStorageHilInspection InspectLessonStorageHilStorage(
    const std::string& cache_key,
    const std::string& sibling_cache_key
) {
    LessonStorageHilInspection inspection {"", "", false, {}};
    std::string slug;
    if (!IsHilCacheKey(cache_key, &slug)) {
        return inspection;
    }
    if (!sibling_cache_key.empty()) {
        std::string sibling_slug;
        if (!IsHilCacheKey(sibling_cache_key, &sibling_slug) ||
            sibling_slug != slug ||
            VersionFromCacheKey(sibling_cache_key) ==
                VersionFromCacheKey(cache_key)) {
            return inspection;
        }
    }
    inspection.cache_key = cache_key;
    inspection.sibling_cache_key = sibling_cache_key;
    InspectionByteBudget budget;
    InspectPathAndDirectChildren(
        LeafPath(cache_key), "lesson-assets/" + cache_key, &inspection, &budget
    );
    if (!sibling_cache_key.empty()) {
        InspectPathAndDirectChildren(
            LeafPath(sibling_cache_key),
            "lesson-assets/" + sibling_cache_key,
            &inspection,
            &budget
        );
    }
    inspection.entries.push_back(InspectEntry(
        JoinPath(RootPath(), "current.json"),
        "lesson-assets/current.json",
        &budget,
        &inspection.truncated
    ));
    InspectPathAndDirectChildren(
        JoinPath(RootPath(), "pvg"),
        "lesson-assets/pvg",
        &inspection,
        &budget
    );
    InspectPathAndDirectChildren(
        JoinPath(RootPath(), "shared"),
        "lesson-assets/shared",
        &inspection,
        &budget
    );
    std::sort(inspection.entries.begin(), inspection.entries.end(), [](const auto& a,
                                                                       const auto& b) {
        return a.label < b.label;
    });
    return inspection;
}
