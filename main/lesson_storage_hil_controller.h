#ifndef LESSON_STORAGE_HIL_CONTROLLER_H
#define LESSON_STORAGE_HIL_CONTROLLER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "lesson_asset_cache_evict.h"

enum class LessonStorageHilOperation { kEvict, kSync };

enum class LessonStorageHilCheckpoint {
    kBeforeFirstUnlink,
    kAfterUnlinks,
    kBeforeRmdir,
    kBeforeDownloadWrite,
    kAfterDownloadBytes,
    kBeforeChecksumVerify,
    kBeforeCommitRename,
};

enum class LessonStorageHilAction { kFail, kPause, kNoSpace, kCorruptStaging };

struct LessonStorageHilArmRequest {
    std::string cache_key;
    LessonStorageHilOperation operation;
    LessonStorageHilCheckpoint checkpoint;
    LessonStorageHilAction action;
    std::uint32_t threshold;
    std::uint32_t declared_asset_bytes;
    std::uint32_t pause_seconds;
};

struct LessonStorageHilDecision {
    bool matched;
    bool consumed;
    LessonStorageHilAction action;
    std::uint64_t sequence;
    std::uint32_t pause_seconds;
};

enum class LessonStorageHilArmCode {
    kArmed,
    kAlreadyArmed,
    kInvalidCacheKey,
    kInvalidCombination,
    kInvalidThreshold,
    kSequenceExhausted,
};

struct LessonStorageHilArmResult {
    LessonStorageHilArmCode code;
    bool armed;
    std::uint64_t arm_sequence;
};

struct LessonStorageHilStatus {
    bool armed;
    bool reached;
    bool consumed;
    LessonStorageHilOperation operation;
    LessonStorageHilCheckpoint checkpoint;
    LessonStorageHilAction action;
    std::uint32_t threshold;
    std::uint32_t declared_asset_bytes;
    std::uint32_t pause_seconds;
    std::uint64_t arm_sequence;
    std::uint64_t reached_sequence;
    std::uint64_t consumed_sequence;
};

class LessonStorageHilController {
public:
    static LessonStorageHilController& GetInstance();

    LessonStorageHilArmResult Arm(const LessonStorageHilArmRequest& request);
    LessonStorageHilStatus Status() const;
    void Reset();
    std::size_t LimitDownloadRead(
        const char* cache_key,
        std::size_t downloaded,
        std::size_t requested,
        std::size_t declared_asset_bytes
    ) noexcept;
    LessonStorageHilDecision Observe(
        const char* cache_key,
        LessonStorageHilOperation operation,
        LessonStorageHilCheckpoint checkpoint,
        std::uint32_t progress,
        std::uint32_t declared_asset_bytes
    ) noexcept;

private:
    LessonStorageHilController() = default;

#ifdef TBOT_LESSON_STORAGE_HIL_CONTROLLER_TESTING
    friend struct LessonStorageHilControllerTestPeer;
#endif

    bool KeyMatches(const char* cache_key) const noexcept;
    bool HasLifecycleSequences() const noexcept;
    std::uint64_t TakeSequence() noexcept;
    void ClearStatus() noexcept;

    mutable std::mutex mutex_;
    std::array<char, kLessonAssetCacheKeyMaxBytes + 1> cache_key_{};
    bool active_ = false;
    bool reached_ = false;
    bool consumed_ = false;
    LessonStorageHilOperation operation_ = LessonStorageHilOperation::kEvict;
    LessonStorageHilCheckpoint checkpoint_ =
        LessonStorageHilCheckpoint::kBeforeFirstUnlink;
    LessonStorageHilAction action_ = LessonStorageHilAction::kFail;
    std::uint32_t threshold_ = 0;
    std::uint32_t declared_asset_bytes_ = 0;
    std::uint32_t pause_seconds_ = 0;
    std::uint64_t arm_sequence_ = 0;
    std::uint64_t reached_sequence_ = 0;
    std::uint64_t consumed_sequence_ = 0;
    std::uint64_t next_sequence_ = 1;
};

#endif  // LESSON_STORAGE_HIL_CONTROLLER_H
