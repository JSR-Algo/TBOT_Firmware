#ifndef LESSON_STORAGE_HIL_FIXTURE_H
#define LESSON_STORAGE_HIL_FIXTURE_H

#include "lesson_asset_storage_coordinator.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <sys/types.h>
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

#ifdef TBOT_LESSON_STORAGE_HIL_FIXTURE_TESTING
using LessonStorageHilFixtureRemoveCallback = int (*)(const char* path);
using LessonStorageHilFixtureMkdirCallback = int (*)(const char* path);
using LessonStorageHilFixtureFsyncCallback = int (*)(int descriptor);
using LessonStorageHilFixtureWriteCallback = ssize_t (*)(
    int descriptor,
    const void* bytes,
    std::size_t length
);
using LessonStorageHilFixtureReadCallback = std::size_t (*)(
    void* bytes,
    std::size_t length,
    FILE* file
);
using LessonStorageHilFixtureInspectFailureCallback = bool (*)(const char* path);
void SetLessonStorageHilFixtureMkdirCallbackForTest(
    LessonStorageHilFixtureMkdirCallback callback
);
void SetLessonStorageHilFixtureFsyncCallbackForTest(
    LessonStorageHilFixtureFsyncCallback callback
);
void SetLessonStorageHilFixtureWriteCallbackForTest(
    LessonStorageHilFixtureWriteCallback callback
);
void SetLessonStorageHilFixtureReadCallbackForTest(
    LessonStorageHilFixtureReadCallback callback
);
void SetLessonStorageHilFixtureInspectFailureCallbackForTest(
    LessonStorageHilFixtureInspectFailureCallback callback
);
void SetLessonStorageHilFixtureUnlinkCallbackForTest(
    LessonStorageHilFixtureRemoveCallback callback
);
void SetLessonStorageHilFixtureRmdirCallbackForTest(
    LessonStorageHilFixtureRemoveCallback callback
);
#endif

#endif  // LESSON_STORAGE_HIL_FIXTURE_H
