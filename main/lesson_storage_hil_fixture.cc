#include "lesson_storage_hil_fixture.h"

#include "lesson_asset_cache_evict.h"

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef TBOT_LESSON_STORAGE_HIL_ROOT
#define TBOT_LESSON_STORAGE_HIL_ROOT "/sdcard/tbot/lesson-assets"
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
constexpr std::size_t kSha256BufferBytes = 512;

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

std::string JoinPath(const std::string& parent, const std::string& child) {
    return parent + "/" + child;
}

std::string RootPath() {
    return TBOT_LESSON_STORAGE_HIL_ROOT;
}

std::string SlugFromCacheKey(const std::string& cache_key) {
    return cache_key.substr(0, cache_key.find('/'));
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
        sibling_slug != slug || sibling_cache_key == cache_key) {
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
    struct stat local_metadata {};
    if (lstat(path.c_str(), &local_metadata) != 0) {
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
    errno = 0;
    while (dirent* entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string name(entry->d_name);
        if (names->size() < cap) {
            names->push_back(std::move(name));
            std::sort(names->begin(), names->end());
            continue;
        }
        *truncated = true;
        if (name < names->back()) {
            names->back() = std::move(name);
            std::sort(names->begin(), names->end());
        }
    }
    const int scan_errno = errno;
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

void RollBackParents(const std::string& slug, const ParentCreation& creation) {
    if (creation.slug_created) {
        rmdir(SlugPath(slug).c_str());
    }
    if (creation.root_created) {
        rmdir(RootPath().c_str());
    }
}

bool EnsureParents(
    const std::string& slug,
    bool root_missing,
    bool slug_missing,
    ParentCreation* creation
) {
    if (root_missing) {
        if (mkdir(RootPath().c_str(), 0755) != 0) {
            return false;
        }
        creation->root_created = true;
    }
    if (slug_missing) {
        if (mkdir(SlugPath(slug).c_str(), 0755) != 0) {
            RollBackParents(slug, *creation);
            *creation = {};
            return false;
        }
        creation->slug_created = true;
    }
    return true;
}

bool WriteExactFile(const std::string& path, const char* bytes) {
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        return false;
    }
    const std::size_t length = std::strlen(bytes);
    std::size_t written = 0;
    bool ok = true;
    while (written < length) {
        const ssize_t result = write(
            descriptor, bytes + written, length - written
        );
        if (result <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(result);
    }
    if (ok && fsync(descriptor) != 0) {
        ok = false;
    }
    if (close(descriptor) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(path.c_str());
    }
    return ok;
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
    if (!EnsureParents(slug, root_missing, slug_missing, &creation)) {
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
    }
    if (mkdir(leaf_path.c_str(), 0755) != 0) {
        RollBackParents(slug, creation);
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
    }
    const std::string sentinel_path = JoinPath(leaf_path, kNestedSentinelName);
    if (mkdir(sentinel_path.c_str(), 0755) != 0) {
        rmdir(leaf_path.c_str());
        RollBackParents(slug, creation);
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
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
    if (!EnsureParents(slug, root_missing, slug_missing, &creation)) {
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
    }
    if (!WriteExactFile(leaf_path, kLeafMagic)) {
        RollBackParents(slug, creation);
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
    }
    return Result(LessonStorageHilFixtureCode::kStaged, true, cache_key, "");
}

bool CreatePreservationLeaf(
    const std::string& leaf_path,
    const char* magic
) {
    if (mkdir(leaf_path.c_str(), 0755) != 0) {
        return false;
    }
    const std::string sentinel = JoinPath(leaf_path, kPreservationSentinelName);
    if (!WriteExactFile(sentinel, magic)) {
        rmdir(leaf_path.c_str());
        return false;
    }
    return true;
}

void RemovePreservationLeaf(const std::string& leaf_path) {
    unlink(JoinPath(leaf_path, kPreservationSentinelName).c_str());
    rmdir(leaf_path.c_str());
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
        const FixtureState failure = first_state != FixtureState::kMissing
                                         ? first_state
                                         : second_state;
        return Result(CodeForState(failure), false, cache_key, sibling_cache_key);
    }

    ParentCreation creation;
    if (!EnsureParents(slug, root_missing, slug_missing, &creation)) {
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            false,
            cache_key,
            sibling_cache_key
        );
    }
    if (!CreatePreservationLeaf(first_leaf, kPreservationPrimaryMagic)) {
        RollBackParents(slug, creation);
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            false,
            cache_key,
            sibling_cache_key
        );
    }
    if (!CreatePreservationLeaf(second_leaf, kPreservationSiblingMagic)) {
        RemovePreservationLeaf(first_leaf);
        RollBackParents(slug, creation);
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            false,
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
        const bool removed = rmdir(leaf_path.c_str()) == 0;
        return Result(
            removed ? LessonStorageHilFixtureCode::kCleaned
                    : LessonStorageHilFixtureCode::kIoFailed,
            removed,
            cache_key,
            ""
        );
    }
    if (state != FixtureState::kComplete) {
        return Result(CodeForState(state), false, cache_key, "");
    }
    const std::string sentinel_path = JoinPath(leaf_path, kNestedSentinelName);
    if (rmdir(sentinel_path.c_str()) != 0) {
        return Result(LessonStorageHilFixtureCode::kIoFailed, false, cache_key, "");
    }
    if (rmdir(leaf_path.c_str()) != 0) {
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
    if (unlink(leaf_path.c_str()) != 0) {
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
    if (first_state != FixtureState::kComplete ||
        second_state != FixtureState::kComplete) {
        const FixtureState failure = first_state != FixtureState::kComplete
                                         ? first_state
                                         : second_state;
        return Result(CodeForState(failure), false, cache_key, sibling_cache_key);
    }

    bool changed = false;
    const std::string first_sentinel =
        JoinPath(first_leaf, kPreservationSentinelName);
    const std::string second_sentinel =
        JoinPath(second_leaf, kPreservationSentinelName);
    if (unlink(first_sentinel.c_str()) != 0) {
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            false,
            cache_key,
            sibling_cache_key
        );
    }
    changed = true;
    if (unlink(second_sentinel.c_str()) != 0 || rmdir(first_leaf.c_str()) != 0 ||
        rmdir(second_leaf.c_str()) != 0) {
        return Result(
            LessonStorageHilFixtureCode::kIoFailed,
            changed,
            cache_key,
            sibling_cache_key
        );
    }
    return Result(
        LessonStorageHilFixtureCode::kCleaned,
        true,
        cache_key,
        sibling_cache_key
    );
}

bool HashFileSha256(
    const std::string& path,
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
    while (result == 0) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            if (total > std::numeric_limits<std::size_t>::max() - read) {
                result = -1;
                break;
            }
            total += read;
#if MBEDTLS_VERSION_MAJOR >= 3
            result = mbedtls_sha256_update(&context, buffer.data(), read);
#else
            result = mbedtls_sha256_update_ret(&context, buffer.data(), read);
#endif
        }
        if (read < buffer.size()) {
            if (std::ferror(file)) {
                result = -1;
            }
            break;
        }
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
    const std::string& label
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
    std::size_t bytes = 0;
    std::string sha256;
    if (!HashFileSha256(path, &bytes, &sha256)) {
        return {label, "unexpected", 0, ""};
    }
    return {label, "regular_file", bytes, std::move(sha256)};
}

void InspectPathAndDirectChildren(
    const std::string& path,
    const std::string& label,
    LessonStorageHilInspection* inspection
) {
    inspection->entries.push_back(InspectEntry(path, label));
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
        inspection->entries.push_back(
            InspectEntry(JoinPath(path, name), JoinPath(label, name))
        );
    }
}

}  // namespace

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
            sibling_slug != slug || sibling_cache_key == cache_key) {
            return inspection;
        }
    }
    inspection.cache_key = cache_key;
    inspection.sibling_cache_key = sibling_cache_key;
    InspectPathAndDirectChildren(
        LeafPath(cache_key), "/lesson-assets/" + cache_key, &inspection
    );
    if (!sibling_cache_key.empty()) {
        InspectPathAndDirectChildren(
            LeafPath(sibling_cache_key),
            "/lesson-assets/" + sibling_cache_key,
            &inspection
        );
    }
    inspection.entries.push_back(InspectEntry(
        JoinPath(RootPath(), "current.json"), "/lesson-assets/current.json"
    ));
    InspectPathAndDirectChildren(
        JoinPath(RootPath(), "pvg"), "/lesson-assets/pvg", &inspection
    );
    InspectPathAndDirectChildren(
        JoinPath(RootPath(), "shared"), "/lesson-assets/shared", &inspection
    );
    std::sort(inspection.entries.begin(), inspection.entries.end(), [](const auto& a,
                                                                       const auto& b) {
        return a.label < b.label;
    });
    return inspection;
}
