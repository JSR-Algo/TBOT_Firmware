#ifndef LESSON_ASSET_PACK_ACTIVATION_H
#define LESSON_ASSET_PACK_ACTIVATION_H

#include <string>

class LessonAssetMutationLease;

struct LessonAssetPackActivationResult {
    bool activated;
    bool previous_evicted;
    std::string previous_cache_key;
    std::string error_code;
};

LessonAssetPackActivationResult ActivateLessonAssetPack(
    const std::string& lesson_id,
    const std::string& cache_key,
    const std::string& manifest_checksum,
    bool all_critical_verified
);

LessonAssetPackActivationResult ActivateLessonAssetPack(
    const LessonAssetMutationLease& mutation,
    const std::string& lesson_id,
    const std::string& cache_key,
    const std::string& manifest_checksum,
    bool all_critical_verified
);

void EvictPreviousLessonAssetPackAfterActivation(
    LessonAssetPackActivationResult& activation,
    const std::string& lesson_id,
    const std::string& cache_key
);

#if defined(TBOT_LESSON_ASSET_CACHE_EVICT_TESTING) && !defined(ESP_PLATFORM)
enum class LessonAssetPackActivationFsTestMode {
    kNone,
    kFatFsNoOverwriteRename,
};

enum class LessonAssetPackActivationFsTestFailure {
    kNone,
    kInterruptAfterActiveBackup,
    kTmpToActiveRename,
    kBackupCleanupRemove,
};

void SetLessonAssetPackActivationFsTestMode(
    LessonAssetPackActivationFsTestMode mode
);
void SetLessonAssetPackActivationFsTestFailure(
    LessonAssetPackActivationFsTestFailure failure
);
#endif

#endif  // LESSON_ASSET_PACK_ACTIVATION_H
