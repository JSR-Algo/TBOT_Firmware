#include "lesson_asset_cache_evict.h"

#if defined(ESP_PLATFORM) || defined(TBOT_LESSON_ASSET_CACHE_EVICT_TESTING)
#include "lesson_asset_storage_coordinator.h"
#endif

#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
#include "lesson_storage_hil_hooks.h"
#endif

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif

#ifndef TBOT_LESSON_ASSET_ROOT
#define TBOT_LESSON_ASSET_ROOT "/sdcard/tbot/lesson-assets"
#endif

namespace {

#ifdef TBOT_LESSON_ASSET_CACHE_EVICT_TESTING
int g_open_directory_count = 0;
#endif

#ifdef ESP_PLATFORM
constexpr const char* kTag = "LessonCacheEvict";
#endif

LessonAssetCacheEvictResult MakeResult(
    LessonAssetCacheEvictCode code,
    const std::string& cache_key = std::string(),
    int file_count = 0
) {
    return {
        code,
        cache_key,
        file_count,
        code == LessonAssetCacheEvictCode::kEvicted,
        code == LessonAssetCacheEvictCode::kNotFound,
    };
}

void LogResult(const LessonAssetCacheEvictResult& result) {
#ifdef ESP_PLATFORM
    if (result.cache_key.empty()) {
        ESP_LOGI(kTag, "result=%s file_count=%d",
                 LessonAssetCacheEvictCodeName(result.code), result.file_count);
    } else {
        ESP_LOGI(kTag, "cache_key=%s result=%s file_count=%d", result.cache_key.c_str(),
                 LessonAssetCacheEvictCodeName(result.code), result.file_count);
    }
#else
    (void)result;
#endif
}

LessonAssetCacheEvictResult Finish(LessonAssetCacheEvictResult result) {
    if (result.code == LessonAssetCacheEvictCode::kInvalidCacheKey) {
        result.cache_key.clear();
    }
    LogResult(result);
    return result;
}

bool IsLowerAlphaNumeric(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

bool IsLowerHex(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

bool HasExactPrefix(const std::string& path, const std::string& prefix) {
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

LessonAssetCacheEvictCode InspectRequiredDirectory(const std::string& path) {
    struct stat directory_stat {};
    errno = 0;
    if (stat(path.c_str(), &directory_stat) != 0) {
        return LessonAssetCacheEvictCode::kScanFailed;
    }
    if (!S_ISDIR(directory_stat.st_mode)) {
        return LessonAssetCacheEvictCode::kUnexpectedNodeType;
    }
    return LessonAssetCacheEvictCode::kEvicted;
}

LessonAssetCacheEvictCode NodeCode(const struct stat& st) {
    if (S_ISDIR(st.st_mode)) {
        return LessonAssetCacheEvictCode::kNestedDirectory;
    }
    if (!S_ISREG(st.st_mode)) {
        return LessonAssetCacheEvictCode::kUnexpectedNodeType;
    }
    return LessonAssetCacheEvictCode::kEvicted;
}

class LessonAssetDirectoryHandle {
public:
    explicit LessonAssetDirectoryHandle(DIR* directory) : directory_(directory) {
#ifdef TBOT_LESSON_ASSET_CACHE_EVICT_TESTING
        if (directory_ != nullptr) {
            ++g_open_directory_count;
        }
#endif
    }

    ~LessonAssetDirectoryHandle() {
        Close();
    }

    DIR* get() const {
        return directory_;
    }

    bool Close() {
        if (directory_ == nullptr) {
            return true;
        }
        DIR* directory = directory_;
        directory_ = nullptr;
        const bool closed = closedir(directory) == 0;
#ifdef TBOT_LESSON_ASSET_CACHE_EVICT_TESTING
        --g_open_directory_count;
#endif
        return closed;
    }

    LessonAssetDirectoryHandle(const LessonAssetDirectoryHandle&) = delete;
    LessonAssetDirectoryHandle& operator=(const LessonAssetDirectoryHandle&) = delete;

private:
    DIR* directory_;
};

LessonAssetCacheEvictCode ScanDirectRegularFiles(
    const std::string& leaf_path,
    const std::string& leaf_prefix,
    std::vector<std::string>* names
) {
    LessonAssetDirectoryHandle directory(opendir(leaf_path.c_str()));
    if (directory.get() == nullptr) {
        return LessonAssetCacheEvictCode::kScanFailed;
    }

#ifndef ESP_PLATFORM
    if (std::getenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN") != nullptr) {
        return LessonAssetCacheEvictCode::kScanFailed;
    }
#endif

    LessonAssetCacheEvictCode code = LessonAssetCacheEvictCode::kEvicted;
    try {
        names->clear();
        errno = 0;
        while (dirent* entry = readdir(directory.get())) {
#ifndef ESP_PLATFORM
            if (std::getenv(
                    "TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SCAN_ALLOCATION") !=
                nullptr) {
                throw std::bad_alloc();
            }
#endif
            const std::string name(entry->d_name);
            if (name == "." || name == "..") {
                errno = 0;
                continue;
            }
            if (name.empty() || name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos) {
                code = LessonAssetCacheEvictCode::kPathMismatch;
                break;
            }

            const std::string child_path = leaf_prefix + name;
            if (!HasExactPrefix(child_path, leaf_prefix)) {
                code = LessonAssetCacheEvictCode::kPathMismatch;
                break;
            }

            struct stat child_stat {};
            if (stat(child_path.c_str(), &child_stat) != 0) {
                code = LessonAssetCacheEvictCode::kScanFailed;
                break;
            }
            code = NodeCode(child_stat);
            if (code != LessonAssetCacheEvictCode::kEvicted) {
                break;
            }
            names->push_back(name);
            errno = 0;
        }
        if (code == LessonAssetCacheEvictCode::kEvicted && errno != 0) {
            code = LessonAssetCacheEvictCode::kScanFailed;
        }
    } catch (...) {
        code = LessonAssetCacheEvictCode::kScanFailed;
    }
    if (!directory.Close() && code == LessonAssetCacheEvictCode::kEvicted) {
        code = LessonAssetCacheEvictCode::kScanFailed;
    }
    if (code == LessonAssetCacheEvictCode::kEvicted) {
        std::sort(names->begin(), names->end());
    }
    return code;
}

#ifndef ESP_PLATFORM
bool ShouldFailUnlink(const std::string& name) {
    const char* requested = std::getenv("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_UNLINK");
    return requested != nullptr && name == requested;
}

bool HasFailureSeam(const char* name) {
    return std::getenv(name) != nullptr;
}

bool ShouldFailAllocationAfterUnlink(int deleted_count) {
    const char* requested = std::getenv(
        "TBOT_TEST_LESSON_CACHE_EVICT_FAIL_ALLOCATION_AFTER_UNLINK");
    if (requested == nullptr || requested[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(requested, &end, 10);
    return errno == 0 && end != requested && *end == '\0' &&
           value == deleted_count;
}
#endif

void SetMutationResult(
    LessonAssetCacheEvictResult* result,
    LessonAssetCacheEvictCode code,
    int deleted_count
) {
    if (deleted_count > 0 && code != LessonAssetCacheEvictCode::kEvicted) {
        code = LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired;
    }
    result->code = code;
    result->file_count = deleted_count;
    result->evicted = code == LessonAssetCacheEvictCode::kEvicted;
    result->not_found = code == LessonAssetCacheEvictCode::kNotFound;
}

LessonAssetCacheEvictResult FinishMutationResult(
    LessonAssetCacheEvictResult&& result,
    LessonAssetCacheEvictCode code,
    int deleted_count
) {
    SetMutationResult(&result, code, deleted_count);
    return Finish(std::move(result));
}

}  // namespace

#ifdef TBOT_LESSON_ASSET_CACHE_EVICT_TESTING
int LessonAssetCacheEvictOpenDirectoryCountForTest() {
    return g_open_directory_count;
}
#endif

bool IsCanonicalLessonCacheKey(const std::string& value) {
    if (value.size() > kLessonAssetCacheKeyMaxBytes) {
        return false;
    }
    size_t index = 0;
    const size_t slug_start = index;
    if (index >= value.size() || !IsLowerAlphaNumeric(value[index])) {
        return false;
    }
    ++index;
    while (index < value.size() && value[index] != '/') {
        if (IsLowerAlphaNumeric(value[index])) {
            ++index;
            continue;
        }
        if (value[index] != '-' || index + 1 >= value.size() ||
            !IsLowerAlphaNumeric(value[index + 1])) {
            return false;
        }
        ++index;
    }
    const size_t slug_size = index - slug_start;
    if (slug_size > kLessonAssetCacheSlugMaxBytes) {
        return false;
    }

    if (index >= value.size() || value[index++] != '/' || index >= value.size() ||
        value[index++] != 'v') {
        return false;
    }
    const size_t version_start = index;
    if (index >= value.size() || value[index] < '1' || value[index] > '9') {
        return false;
    }
    ++index;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
        ++index;
    }
    const size_t version_size = index - version_start;
    if (version_size > kLessonAssetCacheVersionDigitsMax) {
        return false;
    }
    if (index >= value.size() || value[index++] != '-') {
        return false;
    }

    const size_t checksum_start = index;
    if (value.size() - checksum_start != kLessonAssetCacheChecksumHexBytes) {
        return false;
    }
    while (index < value.size()) {
        if (!IsLowerHex(value[index])) {
            return false;
        }
        ++index;
    }
    const std::string slug = value.substr(slug_start, slug_size);
    const std::string version = value.substr(version_start, version_size);
    const std::string checksum = value.substr(checksum_start);
    const std::string reconstructed = slug + "/v" + version + "-" + checksum;
    return reconstructed.size() == value.size() && reconstructed.compare(value) == 0;
}

const char* LessonAssetCacheEvictCodeName(LessonAssetCacheEvictCode code) {
    switch (code) {
        case LessonAssetCacheEvictCode::kEvicted:
            return "evicted";
        case LessonAssetCacheEvictCode::kNotFound:
            return "not_found";
        case LessonAssetCacheEvictCode::kInvalidCacheKey:
            return "invalid_cache_key";
        case LessonAssetCacheEvictCode::kLessonSessionActive:
            return "lesson_session_active";
        case LessonAssetCacheEvictCode::kPathMismatch:
            return "path_mismatch";
        case LessonAssetCacheEvictCode::kNestedDirectory:
            return "nested_directory";
        case LessonAssetCacheEvictCode::kUnexpectedNodeType:
            return "unexpected_node_type";
        case LessonAssetCacheEvictCode::kScanFailed:
            return "scan_failed";
        case LessonAssetCacheEvictCode::kUnlinkFailed:
            return "unlink_failed";
        case LessonAssetCacheEvictCode::kRmdirFailed:
            return "rmdir_failed";
        case LessonAssetCacheEvictCode::kPartialEvictRecoveryRequired:
            return "partial_evict_recovery_required";
    }
    return "unexpected_node_type";
}

LessonAssetCacheEvictResult EvictLessonAssetCacheKey(
    const std::string& cache_key,
    bool lesson_session_active
) {
    if (!IsCanonicalLessonCacheKey(cache_key)) {
        return Finish(MakeResult(LessonAssetCacheEvictCode::kInvalidCacheKey));
    }

    const std::string root(TBOT_LESSON_ASSET_ROOT);
    const std::string root_prefix = root + "/";
    const std::string leaf_path = root_prefix + cache_key;
    if (!HasExactPrefix(leaf_path, root_prefix) || leaf_path.substr(root_prefix.size()) != cache_key) {
        return Finish(MakeResult(LessonAssetCacheEvictCode::kPathMismatch, cache_key));
    }
    const std::string leaf_prefix = leaf_path + "/";

    const size_t cache_separator = cache_key.find('/');
    const std::string slug_path = root_prefix + cache_key.substr(0, cache_separator);
    if (!HasExactPrefix(slug_path, root_prefix)) {
        return Finish(MakeResult(LessonAssetCacheEvictCode::kPathMismatch, cache_key));
    }

#if defined(ESP_PLATFORM) || defined(TBOT_LESSON_ASSET_CACHE_EVICT_TESTING)
    auto mutation =
        LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("evict");
    const bool mutation_refused = !mutation;
#else
    const bool mutation_refused = false;
#endif
    if (mutation_refused || lesson_session_active) {
        return Finish(MakeResult(
            LessonAssetCacheEvictCode::kLessonSessionActive, cache_key));
    }

    auto code = InspectRequiredDirectory(root);
    if (code != LessonAssetCacheEvictCode::kEvicted) {
        return Finish(MakeResult(code, cache_key));
    }
    code = InspectRequiredDirectory(slug_path);
    if (code != LessonAssetCacheEvictCode::kEvicted) {
        return Finish(MakeResult(code, cache_key));
    }

    struct stat leaf_stat {};
#ifndef ESP_PLATFORM
    if (HasFailureSeam("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_LEAF_STAT")) {
        return Finish(MakeResult(LessonAssetCacheEvictCode::kScanFailed, cache_key));
    }
#endif
    errno = 0;
    if (stat(leaf_path.c_str(), &leaf_stat) != 0) {
        if (errno == ENOENT) {
            code = InspectRequiredDirectory(root);
            if (code != LessonAssetCacheEvictCode::kEvicted) {
                return Finish(MakeResult(code, cache_key));
            }
#ifndef ESP_PLATFORM
            if (HasFailureSeam(
                    "TBOT_TEST_LESSON_CACHE_EVICT_FAIL_SLUG_RECHECK")) {
                return Finish(MakeResult(
                    LessonAssetCacheEvictCode::kScanFailed, cache_key));
            }
#endif
            code = InspectRequiredDirectory(slug_path);
            if (code != LessonAssetCacheEvictCode::kEvicted) {
                return Finish(MakeResult(code, cache_key));
            }
            return Finish(MakeResult(LessonAssetCacheEvictCode::kNotFound, cache_key));
        }
        return Finish(MakeResult(LessonAssetCacheEvictCode::kScanFailed, cache_key));
    }
    if (!S_ISDIR(leaf_stat.st_mode)) {
        return Finish(MakeResult(LessonAssetCacheEvictCode::kUnexpectedNodeType, cache_key));
    }

    std::vector<std::string> validated_names;
    code = ScanDirectRegularFiles(leaf_path, leaf_prefix, &validated_names);
    if (code != LessonAssetCacheEvictCode::kEvicted) {
        return Finish(MakeResult(code, cache_key));
    }

    std::vector<std::string> second_pass_names;
    code = ScanDirectRegularFiles(leaf_path, leaf_prefix, &second_pass_names);
    if (code != LessonAssetCacheEvictCode::kEvicted) {
        return Finish(MakeResult(code, cache_key));
    }
    if (second_pass_names != validated_names) {
        return Finish(MakeResult(LessonAssetCacheEvictCode::kScanFailed, cache_key));
    }
    if (validated_names.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Finish(MakeResult(LessonAssetCacheEvictCode::kScanFailed, cache_key));
    }

    LessonAssetCacheEvictResult mutation_result{
        LessonAssetCacheEvictCode::kScanFailed, std::string(), 0, false, false};
    std::vector<std::string> validated_paths;
    try {
        mutation_result.cache_key = cache_key;
        validated_paths.reserve(validated_names.size());
        for (const auto& name : validated_names) {
            validated_paths.push_back(leaf_prefix + name);
            if (!HasExactPrefix(validated_paths.back(), leaf_prefix)) {
                return FinishMutationResult(
                    std::move(mutation_result),
                    LessonAssetCacheEvictCode::kPathMismatch,
                    0);
            }
        }
#ifndef ESP_PLATFORM
        if (HasFailureSeam(
                "TBOT_TEST_LESSON_CACHE_EVICT_FAIL_PREDELETE_ALLOCATION")) {
            throw std::bad_alloc();
        }
#endif
    } catch (...) {
        return FinishMutationResult(
            std::move(mutation_result), LessonAssetCacheEvictCode::kScanFailed, 0);
    }

    code = InspectRequiredDirectory(root);
    if (code != LessonAssetCacheEvictCode::kEvicted) {
        return FinishMutationResult(std::move(mutation_result), code, 0);
    }
    code = InspectRequiredDirectory(slug_path);
    if (code != LessonAssetCacheEvictCode::kEvicted) {
        return FinishMutationResult(std::move(mutation_result), code, 0);
    }
    code = InspectRequiredDirectory(leaf_path);
    if (code != LessonAssetCacheEvictCode::kEvicted) {
        return FinishMutationResult(std::move(mutation_result), code, 0);
    }
    int deleted_count = 0;
    try {
        for (std::size_t index = 0; index < validated_paths.size(); ++index) {
            const std::string& child_path = validated_paths[index];
            struct stat child_stat {};
            if (stat(child_path.c_str(), &child_stat) != 0) {
                return FinishMutationResult(
                    std::move(mutation_result),
                    LessonAssetCacheEvictCode::kUnlinkFailed,
                    deleted_count);
            }
            code = NodeCode(child_stat);
            if (code != LessonAssetCacheEvictCode::kEvicted) {
                return FinishMutationResult(
                    std::move(mutation_result), code, deleted_count);
            }
#ifndef ESP_PLATFORM
            if (ShouldFailUnlink(validated_names[index])) {
                return FinishMutationResult(
                    std::move(mutation_result),
                    LessonAssetCacheEvictCode::kUnlinkFailed,
                    deleted_count);
            }
#endif
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
            if (index == 0 &&
                RunLessonStorageHilCheckpoint(
                    cache_key.c_str(), LessonStorageHilOperation::kEvict,
                    LessonStorageHilCheckpoint::kBeforeFirstUnlink, 0, 0) !=
                    LessonStorageHilHookOutcome::kContinue) {
                return FinishMutationResult(
                    std::move(mutation_result),
                    LessonAssetCacheEvictCode::kUnlinkFailed,
                    deleted_count);
            }
#endif
            if (unlink(child_path.c_str()) != 0) {
                return FinishMutationResult(
                    std::move(mutation_result),
                    LessonAssetCacheEvictCode::kUnlinkFailed,
                    deleted_count);
            }
            ++deleted_count;
#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
            if (RunLessonStorageHilCheckpoint(
                    cache_key.c_str(), LessonStorageHilOperation::kEvict,
                    LessonStorageHilCheckpoint::kAfterUnlinks,
                    static_cast<std::uint32_t>(deleted_count), 0) !=
                LessonStorageHilHookOutcome::kContinue) {
                return FinishMutationResult(
                    std::move(mutation_result),
                    LessonAssetCacheEvictCode::kUnlinkFailed,
                    deleted_count);
            }
#endif
#ifndef ESP_PLATFORM
            if (ShouldFailAllocationAfterUnlink(deleted_count)) {
                throw std::bad_alloc();
            }
#endif
        }

#if defined(CONFIG_TBOT_HIL_STORAGE_FAULTS) || \
    defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
        if (RunLessonStorageHilCheckpoint(
                cache_key.c_str(), LessonStorageHilOperation::kEvict,
                LessonStorageHilCheckpoint::kBeforeRmdir,
                static_cast<std::uint32_t>(deleted_count), 0) !=
            LessonStorageHilHookOutcome::kContinue) {
            return FinishMutationResult(
                std::move(mutation_result),
                LessonAssetCacheEvictCode::kRmdirFailed,
                deleted_count);
        }
#endif
#ifndef ESP_PLATFORM
        if (HasFailureSeam("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_RMDIR")) {
            return FinishMutationResult(
                std::move(mutation_result),
                LessonAssetCacheEvictCode::kRmdirFailed,
                deleted_count);
        }
#endif
        if (rmdir(leaf_path.c_str()) != 0) {
            return FinishMutationResult(
                std::move(mutation_result),
                LessonAssetCacheEvictCode::kRmdirFailed,
                deleted_count);
        }

#ifndef ESP_PLATFORM
        if (HasFailureSeam("TBOT_TEST_LESSON_CACHE_EVICT_FAIL_FINAL_STAT")) {
            return FinishMutationResult(
                std::move(mutation_result),
                LessonAssetCacheEvictCode::kScanFailed,
                deleted_count);
        }
#endif
        errno = 0;
        if (stat(leaf_path.c_str(), &leaf_stat) == 0 || errno != ENOENT) {
            return FinishMutationResult(
                std::move(mutation_result),
                LessonAssetCacheEvictCode::kScanFailed,
                deleted_count);
        }
    } catch (...) {
        return FinishMutationResult(
            std::move(mutation_result),
            LessonAssetCacheEvictCode::kScanFailed,
            deleted_count);
    }
    return FinishMutationResult(
        std::move(mutation_result), LessonAssetCacheEvictCode::kEvicted, deleted_count);
}
