#ifndef LESSON_ASSET_PACK_ACTIVATION_H
#define LESSON_ASSET_PACK_ACTIVATION_H

#include <string>

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

#endif  // LESSON_ASSET_PACK_ACTIVATION_H
