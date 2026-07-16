#include "lesson_storage_hil_hooks.h"

#ifdef TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING
#include <atomic>
#endif

#ifdef ESP_PLATFORM
#include <cinttypes>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace {

#ifdef TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING
std::atomic<LessonStorageHilPauseCallback> g_pause_callback{nullptr};
#endif

#ifdef ESP_PLATFORM
constexpr const char* kTag = "LessonStorageHil";

const char* OperationName(LessonStorageHilOperation operation) noexcept {
    switch (operation) {
        case LessonStorageHilOperation::kEvict:
            return "evict";
        case LessonStorageHilOperation::kSync:
            return "sync";
    }
    return "unknown";
}

const char* CheckpointName(LessonStorageHilCheckpoint checkpoint) noexcept {
    switch (checkpoint) {
        case LessonStorageHilCheckpoint::kBeforeFirstUnlink:
            return "before_first_unlink";
        case LessonStorageHilCheckpoint::kAfterUnlinks:
            return "after_unlinks";
        case LessonStorageHilCheckpoint::kBeforeRmdir:
            return "before_rmdir";
        case LessonStorageHilCheckpoint::kBeforeDownloadWrite:
            return "before_download_write";
        case LessonStorageHilCheckpoint::kAfterDownloadBytes:
            return "after_download_bytes";
        case LessonStorageHilCheckpoint::kBeforeChecksumVerify:
            return "before_checksum_verify";
        case LessonStorageHilCheckpoint::kBeforeCommitRename:
            return "before_commit_rename";
    }
    return "unknown";
}
#endif

bool YieldingPause(std::uint32_t seconds) noexcept {
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000U));
    return true;
#elif defined(TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING)
    const auto callback = g_pause_callback.load(std::memory_order_acquire);
    if (callback == nullptr) {
        return false;
    }
    callback(seconds);
    return true;
#else
    (void)seconds;
    return false;
#endif
}

void LogCheckpointReached(
    const char* cache_key,
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    std::uint32_t progress
) noexcept {
#ifdef ESP_PLATFORM
    ESP_LOGW(kTag,
             "HIL_STORAGE_CHECKPOINT_REACHED operation=%s checkpoint=%s "
             "cache_key=%s count=%" PRIu32,
             OperationName(operation), CheckpointName(checkpoint), cache_key,
             progress);
#else
    (void)cache_key;
    (void)operation;
    (void)checkpoint;
    (void)progress;
#endif
}

void LogCheckpointContinued(
    const char* cache_key,
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    std::uint32_t progress
) noexcept {
#ifdef ESP_PLATFORM
    ESP_LOGW(kTag,
             "HIL_STORAGE_CHECKPOINT_CONTINUED operation=%s checkpoint=%s "
             "cache_key=%s count=%" PRIu32,
             OperationName(operation), CheckpointName(checkpoint), cache_key,
             progress);
#else
    (void)cache_key;
    (void)operation;
    (void)checkpoint;
    (void)progress;
#endif
}

}  // namespace

LessonStorageHilHookOutcome RunLessonStorageHilCheckpoint(
    const char* cache_key,
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    std::uint32_t progress,
    std::uint32_t declared_asset_bytes
) noexcept {
    const LessonStorageHilDecision decision =
        LessonStorageHilController::GetInstance().Observe(
            cache_key, operation, checkpoint, progress, declared_asset_bytes);
    if (!decision.matched || !decision.consumed) {
        return LessonStorageHilHookOutcome::kContinue;
    }

    switch (decision.action) {
        case LessonStorageHilAction::kFail:
            return LessonStorageHilHookOutcome::kFail;
        case LessonStorageHilAction::kNoSpace:
            return LessonStorageHilHookOutcome::kNoSpace;
        case LessonStorageHilAction::kPause:
            LogCheckpointReached(cache_key, operation, checkpoint, progress);
            if (!YieldingPause(decision.pause_seconds)) {
                return LessonStorageHilHookOutcome::kFail;
            }
            LogCheckpointContinued(cache_key, operation, checkpoint, progress);
            return LessonStorageHilHookOutcome::kContinue;
        case LessonStorageHilAction::kCorruptStaging:
            return LessonStorageHilHookOutcome::kFail;
    }
    return LessonStorageHilHookOutcome::kFail;
}

#ifdef TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING
void SetLessonStorageHilPauseCallbackForTest(
    LessonStorageHilPauseCallback callback
) noexcept {
    g_pause_callback.store(callback, std::memory_order_release);
}
#endif
