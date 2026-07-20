#ifndef LESSON_ASSET_SYNC_PATH_POLICY_H
#define LESSON_ASSET_SYNC_PATH_POLICY_H

#include <cstddef>
#include <string>
#include <string_view>

inline constexpr std::size_t kLessonAssetSyncUrlMaxBytes = 2048;
inline constexpr std::size_t kLessonAssetSyncKeyMaxBytes = 255;
inline constexpr std::size_t kLessonAssetSyncMetadataPathMaxBytes = 512;
inline constexpr std::size_t kLessonAssetSyncSha256HexBytes = 64;

enum class LessonAssetSyncPathCode {
    kValid,
    kInvalidCacheKey,
    kInvalidPath,
    kReservedDestination,
};

struct LessonAssetSyncPathResult {
    LessonAssetSyncPathCode code;
    std::string destination;
};

bool IsCanonicalLessonAssetSyncCacheKey(std::string_view cache_key);
bool IsExactLowerLessonAssetSha256(std::string_view value);
bool IsAllowedLessonAssetSyncUrl(std::string_view value);

LessonAssetSyncPathResult ValidateLessonAssetSyncPath(
    std::string_view cache_key,
    std::string_view local_path,
    std::string_view asset_key
);

bool LessonAssetSyncDestinationsCollide(
    std::string_view left,
    std::string_view right
);

#endif  // LESSON_ASSET_SYNC_PATH_POLICY_H
