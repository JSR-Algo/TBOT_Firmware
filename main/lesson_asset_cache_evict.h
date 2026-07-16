#ifndef LESSON_ASSET_CACHE_EVICT_H
#define LESSON_ASSET_CACHE_EVICT_H

#include <cstddef>
#include <string>

inline constexpr std::size_t kLessonAssetCacheSlugMaxBytes = 128;
inline constexpr std::size_t kLessonAssetCacheVersionDigitsMax = 10;
inline constexpr std::size_t kLessonAssetCacheChecksumHexBytes = 64;
inline constexpr std::size_t kLessonAssetCacheKeyMaxBytes =
    kLessonAssetCacheSlugMaxBytes + 2 + kLessonAssetCacheVersionDigitsMax + 1 +
    kLessonAssetCacheChecksumHexBytes;

enum class LessonAssetCacheEvictCode {
    kEvicted,
    kNotFound,
    kInvalidCacheKey,
    kLessonSessionActive,
    kPathMismatch,
    kNestedDirectory,
    kUnexpectedNodeType,
    kScanFailed,
    kUnlinkFailed,
    kRmdirFailed,
    kPartialEvictRecoveryRequired,
};

struct LessonAssetCacheEvictResult {
    LessonAssetCacheEvictCode code;
    std::string cache_key;
    int file_count;
    bool evicted;
    bool not_found;
};

bool IsCanonicalLessonCacheKey(const std::string& value);
const char* LessonAssetCacheEvictCodeName(LessonAssetCacheEvictCode code);
LessonAssetCacheEvictResult EvictLessonAssetCacheKey(
    const std::string& cache_key,
    bool lesson_session_active
);

#ifdef TBOT_LESSON_ASSET_CACHE_EVICT_TESTING
int LessonAssetCacheEvictOpenDirectoryCountForTest();
#endif

#endif  // LESSON_ASSET_CACHE_EVICT_H
