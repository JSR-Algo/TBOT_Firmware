#ifndef LESSON_STORAGE_HIL_HOOKS_H
#define LESSON_STORAGE_HIL_HOOKS_H

#include <cstdint>

#include "lesson_storage_hil_controller.h"

enum class LessonStorageHilHookOutcome { kContinue, kFail, kNoSpace };

LessonStorageHilHookOutcome RunLessonStorageHilCheckpoint(
    const char* cache_key,
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    std::uint32_t progress,
    std::uint32_t declared_asset_bytes
) noexcept;

LessonStorageHilHookOutcome RunLessonStorageHilStagingCheckpoint(
    const char* cache_key,
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    std::uint32_t progress,
    std::uint32_t declared_asset_bytes,
    const char* staging_path
) noexcept;

#ifdef TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING
using LessonStorageHilPauseCallback = void (*)(std::uint32_t seconds) noexcept;

void SetLessonStorageHilPauseCallbackForTest(
    LessonStorageHilPauseCallback callback
) noexcept;
#endif

#endif  // LESSON_STORAGE_HIL_HOOKS_H
