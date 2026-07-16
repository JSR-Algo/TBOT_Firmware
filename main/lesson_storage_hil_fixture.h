#ifndef LESSON_STORAGE_HIL_FIXTURE_H
#define LESSON_STORAGE_HIL_FIXTURE_H

#include "lesson_asset_storage_coordinator.h"

#include <cstddef>
#include <string>
#include <vector>

enum class LessonStorageHilFixture {
    kNestedDirectory,
    kLeafRegularFile,
    kPreservationSet,
};

enum class LessonStorageHilFixtureCode {
    kStaged,
    kCleaned,
    kInspected,
    kInvalidCacheKey,
    kInvalidSibling,
    kLeaseRefused,
    kUnexpectedExistingNode,
    kSentinelMismatch,
    kIoFailed,
};

struct LessonStorageHilFixtureResult {
    LessonStorageHilFixtureCode code;
    bool changed;
    std::string cache_key;
    std::string sibling_cache_key;
};

struct LessonStorageHilInspectionEntry {
    std::string label;
    std::string node_type;
    std::size_t bytes;
    std::string sha256;
};

struct LessonStorageHilInspection {
    std::string cache_key;
    std::string sibling_cache_key;
    bool truncated;
    std::vector<LessonStorageHilInspectionEntry> entries;
};

LessonStorageHilFixtureResult StageLessonStorageHilFixture(
    const LessonAssetMutationLease& mutation,
    const std::string& cache_key,
    LessonStorageHilFixture fixture,
    const std::string& sibling_cache_key
);

LessonStorageHilFixtureResult CleanupLessonStorageHilFixture(
    const LessonAssetMutationLease& mutation,
    const std::string& cache_key,
    LessonStorageHilFixture fixture,
    const std::string& sibling_cache_key
);

LessonStorageHilInspection InspectLessonStorageHilStorage(
    const std::string& cache_key,
    const std::string& sibling_cache_key
);

#endif  // LESSON_STORAGE_HIL_FIXTURE_H
