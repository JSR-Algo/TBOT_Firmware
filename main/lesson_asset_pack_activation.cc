#include "lesson_asset_pack_activation.h"

#include "lesson_asset_cache_evict.h"
#include "lesson_asset_storage_coordinator.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TBOT_LESSON_ASSET_ROOT
#define TBOT_LESSON_ASSET_ROOT "/sdcard/tbot/lesson-assets"
#endif

namespace {

bool IsLowerAlphaNumeric(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

bool IsLowerHex(const std::string& value) {
    if (value.size() != kLessonAssetCacheChecksumHexBytes) {
        return false;
    }
    for (char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string LessonIdFromCacheKey(const std::string& cache_key) {
    const size_t separator = cache_key.find('/');
    return separator == std::string::npos ? std::string()
                                          : cache_key.substr(0, separator);
}

bool IsCanonicalLessonId(const std::string& lesson_id) {
    if (lesson_id.empty() || lesson_id.size() > kLessonAssetCacheSlugMaxBytes ||
        !IsLowerAlphaNumeric(lesson_id.front())) {
        return false;
    }
    for (size_t index = 1; index < lesson_id.size(); ++index) {
        if (IsLowerAlphaNumeric(lesson_id[index])) {
            continue;
        }
        if (lesson_id[index] != '-' || index + 1 >= lesson_id.size() ||
            !IsLowerAlphaNumeric(lesson_id[index + 1])) {
            return false;
        }
    }
    return true;
}

bool EnsureDir(const std::string& path) {
    errno = 0;
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool EnsureDirTree(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    size_t pos = path[0] == '/' ? 1 : 0;
    while ((pos = path.find('/', pos)) != std::string::npos) {
        if (pos > 1 && !EnsureDir(path.substr(0, pos))) {
            return false;
        }
        ++pos;
    }
    return EnsureDir(path);
}

void EnsureActivationDirectories(const std::string& lesson_id) {
    if (!EnsureDirTree(std::string(TBOT_LESSON_ASSET_ROOT)) ||
        !EnsureDir(std::string(TBOT_LESSON_ASSET_ROOT) + "/" + lesson_id)) {
        throw std::runtime_error("active pointer directory unavailable");
    }
}

std::string ActivePointerPath(const std::string& lesson_id) {
    return std::string(TBOT_LESSON_ASSET_ROOT) + "/" + lesson_id + "/active.json";
}

std::string ActivePointerJson(
    const std::string& lesson_id,
    const std::string& cache_key,
    const std::string& manifest_checksum
) {
    return std::string("{\"lessonId\":\"") + lesson_id + "\",\"cacheKey\":\"" +
           cache_key + "\",\"manifestChecksum\":\"" + manifest_checksum + "\"}";
}

std::string ReadTextFileIfPresent(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::string();
    }
    std::string body;
    char buffer[256];
    while (true) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
        body.append(buffer, count);
        if (count < sizeof(buffer)) {
            break;
        }
    }
    std::fclose(file);
    return body;
}

std::string ExtractFlatJsonString(
    const std::string& body,
    const char* field
) {
    const std::string prefix = std::string("\"") + field + "\":\"";
    const size_t start = body.find(prefix);
    if (start == std::string::npos) {
        return std::string();
    }
    const size_t value_start = start + prefix.size();
    const size_t end = body.find('"', value_start);
    if (end == std::string::npos) {
        return std::string();
    }
    return body.substr(value_start, end - value_start);
}

void WriteActivePointerAtomically(
    const std::string& lesson_id,
    const std::string& cache_key,
    const std::string& manifest_checksum
) {
    EnsureActivationDirectories(lesson_id);
    const std::string active_path = ActivePointerPath(lesson_id);
    const std::string tmp_path = active_path + ".tmp";
    const std::string body =
        ActivePointerJson(lesson_id, cache_key, manifest_checksum);

    FILE* file = std::fopen(tmp_path.c_str(), "wb");
    if (file == nullptr) {
        throw std::runtime_error("active pointer open failed");
    }
    if (std::fwrite(body.data(), 1, body.size(), file) != body.size()) {
        std::fclose(file);
        std::remove(tmp_path.c_str());
        throw std::runtime_error("active pointer write failed");
    }
    if (std::fflush(file) != 0) {
        std::fclose(file);
        std::remove(tmp_path.c_str());
        throw std::runtime_error("active pointer flush failed");
    }
    const int descriptor = fileno(file);
    if (descriptor < 0 || fsync(descriptor) != 0) {
        std::fclose(file);
        std::remove(tmp_path.c_str());
        throw std::runtime_error("active pointer sync failed");
    }
    if (std::fclose(file) != 0) {
        std::remove(tmp_path.c_str());
        throw std::runtime_error("active pointer close failed");
    }
    if (std::rename(tmp_path.c_str(), active_path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        throw std::runtime_error("active pointer rename failed");
    }
}

bool PreviousKeyBelongsToLesson(
    const std::string& previous_cache_key,
    const std::string& lesson_id
) {
    return IsCanonicalLessonCacheKey(previous_cache_key) &&
           LessonIdFromCacheKey(previous_cache_key) == lesson_id;
}

}  // namespace

LessonAssetPackActivationResult ActivateLessonAssetPack(
    const std::string& lesson_id,
    const std::string& cache_key,
    const std::string& manifest_checksum,
    bool all_critical_verified
) {
    LessonAssetPackActivationResult activation{
        false, false, std::string(), std::string()};
    {
        auto mutation =
            LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("activate");
        if (!mutation) {
            activation.error_code = "activation_storage_busy";
            return activation;
        }
        activation = ActivateLessonAssetPack(
            mutation, lesson_id, cache_key, manifest_checksum, all_critical_verified);
    }
    EvictPreviousLessonAssetPackAfterActivation(activation, lesson_id, cache_key);
    return activation;
}

LessonAssetPackActivationResult ActivateLessonAssetPack(
    const LessonAssetMutationLease& mutation,
    const std::string& lesson_id,
    const std::string& cache_key,
    const std::string& manifest_checksum,
    bool all_critical_verified
) {
    LessonAssetPackActivationResult activation{
        false, false, std::string(), std::string()};
    if (!all_critical_verified) {
        activation.error_code = "critical_assets_unverified";
        return activation;
    }
    if (!IsCanonicalLessonId(lesson_id) ||
        !IsCanonicalLessonCacheKey(cache_key) ||
        LessonIdFromCacheKey(cache_key) != lesson_id ||
        !IsLowerHex(manifest_checksum)) {
        activation.error_code = "activation_request_invalid";
        return activation;
    }
    if (!mutation) {
        activation.error_code = "activation_storage_busy";
        return activation;
    }

    const std::string active_path = ActivePointerPath(lesson_id);
    const std::string previous_body = ReadTextFileIfPresent(active_path);
    std::string previous_cache_key =
        ExtractFlatJsonString(previous_body, "cacheKey");
    activation.previous_cache_key = previous_cache_key;

    try {
        WriteActivePointerAtomically(lesson_id, cache_key, manifest_checksum);
    } catch (...) {
        activation.error_code = "activation_write_failed";
        return activation;
    }
    activation.activated = true;
    return activation;
}

void EvictPreviousLessonAssetPackAfterActivation(
    LessonAssetPackActivationResult& activation,
    const std::string& lesson_id,
    const std::string& cache_key
) {
    if (!activation.activated) {
        return;
    }
    const std::string& previous_cache_key = activation.previous_cache_key;
    if (PreviousKeyBelongsToLesson(previous_cache_key, lesson_id) &&
        previous_cache_key != cache_key) {
        const auto evict_result =
            EvictLessonAssetCacheKey(previous_cache_key, false);
        activation.previous_evicted = evict_result.evicted;
        if (!evict_result.evicted && !evict_result.not_found) {
            activation.error_code = "previous_evict_retryable";
        }
    }
}
