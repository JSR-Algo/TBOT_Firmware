#include "lesson_storage_hil_controller.h"

#include <algorithm>
#include <limits>

namespace {

constexpr std::uint32_t kMaxUnlinkThreshold = 64;
constexpr std::uint32_t kMaxDeclaredAssetBytes = 512 * 1024;
constexpr std::uint32_t kMinPauseSeconds = 5;
constexpr std::uint32_t kMaxPauseSeconds = 60;

bool IsKnownOperation(LessonStorageHilOperation operation) {
    switch (operation) {
        case LessonStorageHilOperation::kEvict:
        case LessonStorageHilOperation::kSync:
            return true;
    }
    return false;
}

bool IsKnownCheckpoint(LessonStorageHilCheckpoint checkpoint) {
    switch (checkpoint) {
        case LessonStorageHilCheckpoint::kBeforeFirstUnlink:
        case LessonStorageHilCheckpoint::kAfterUnlinks:
        case LessonStorageHilCheckpoint::kBeforeRmdir:
        case LessonStorageHilCheckpoint::kBeforeDownloadWrite:
        case LessonStorageHilCheckpoint::kAfterDownloadBytes:
        case LessonStorageHilCheckpoint::kBeforeChecksumVerify:
        case LessonStorageHilCheckpoint::kBeforeCommitRename:
            return true;
    }
    return false;
}

bool IsKnownAction(LessonStorageHilAction action) {
    switch (action) {
        case LessonStorageHilAction::kFail:
        case LessonStorageHilAction::kPause:
        case LessonStorageHilAction::kNoSpace:
        case LessonStorageHilAction::kCorruptStaging:
            return true;
    }
    return false;
}

bool IsHilCacheKey(const std::string& cache_key) {
    return IsCanonicalLessonCacheKey(cache_key) &&
           cache_key.size() >= 4 && cache_key.compare(0, 4, "hil-") == 0;
}

bool IsCompatible(
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    LessonStorageHilAction action
) {
    using Operation = LessonStorageHilOperation;
    using Checkpoint = LessonStorageHilCheckpoint;
    using Action = LessonStorageHilAction;

    if (operation == Operation::kEvict) {
        if (checkpoint == Checkpoint::kBeforeFirstUnlink ||
            checkpoint == Checkpoint::kAfterUnlinks ||
            checkpoint == Checkpoint::kBeforeRmdir) {
            return action == Action::kFail || action == Action::kPause;
        }
        return false;
    }
    if (operation != Operation::kSync) {
        return false;
    }
    if (checkpoint == Checkpoint::kBeforeDownloadWrite ||
        checkpoint == Checkpoint::kAfterDownloadBytes ||
        checkpoint == Checkpoint::kBeforeCommitRename) {
        return action == Action::kFail || action == Action::kPause ||
               action == Action::kNoSpace;
    }
    if (checkpoint == Checkpoint::kBeforeChecksumVerify) {
        return action == Action::kFail || action == Action::kPause ||
               action == Action::kCorruptStaging;
    }
    return false;
}

bool HasValidNumbers(const LessonStorageHilArmRequest& request) {
    using Checkpoint = LessonStorageHilCheckpoint;
    using Action = LessonStorageHilAction;

    if (request.action == Action::kPause) {
        if (request.pause_seconds < kMinPauseSeconds ||
            request.pause_seconds > kMaxPauseSeconds) {
            return false;
        }
    } else if (request.pause_seconds != 0) {
        return false;
    }

    if (request.checkpoint == Checkpoint::kAfterUnlinks) {
        return request.threshold >= 1 &&
               request.threshold <= kMaxUnlinkThreshold &&
               request.declared_asset_bytes == 0;
    }
    if (request.checkpoint == Checkpoint::kAfterDownloadBytes) {
        return request.threshold >= 1 &&
               request.declared_asset_bytes >= request.threshold &&
               request.declared_asset_bytes <= kMaxDeclaredAssetBytes;
    }
    return request.threshold == 0 && request.declared_asset_bytes == 0;
}

}  // namespace

LessonStorageHilController& LessonStorageHilController::GetInstance() {
    static LessonStorageHilController instance;
    return instance;
}

LessonStorageHilArmResult LessonStorageHilController::Arm(
    const LessonStorageHilArmRequest& request
) {
    if (!IsHilCacheKey(request.cache_key)) {
        return {LessonStorageHilArmCode::kInvalidCacheKey, false, 0};
    }
    if (!IsKnownOperation(request.operation) ||
        !IsKnownCheckpoint(request.checkpoint) ||
        !IsKnownAction(request.action) ||
        !IsCompatible(request.operation, request.checkpoint, request.action)) {
        return {LessonStorageHilArmCode::kInvalidCombination, false, 0};
    }
    if (!HasValidNumbers(request)) {
        return {LessonStorageHilArmCode::kInvalidThreshold, false, 0};
    }

    std::array<char, kLessonAssetCacheKeyMaxBytes + 1> validated_key{};
    std::copy(request.cache_key.begin(), request.cache_key.end(),
              validated_key.begin());

    LockGuard lock(mutex_);
    if (active_) {
        return {LessonStorageHilArmCode::kAlreadyArmed, true, arm_sequence_};
    }
    if (!HasLifecycleSequences()) {
        return {LessonStorageHilArmCode::kSequenceExhausted, false, 0};
    }

    cache_key_ = validated_key;
    reached_ = false;
    consumed_ = false;
    operation_ = request.operation;
    checkpoint_ = request.checkpoint;
    action_ = request.action;
    threshold_ = request.threshold;
    declared_asset_bytes_ = request.declared_asset_bytes;
    pause_seconds_ = request.pause_seconds;
    arm_sequence_ = TakeSequence();
    reached_sequence_ = 0;
    consumed_sequence_ = 0;
    active_ = true;
    return {LessonStorageHilArmCode::kArmed, true, arm_sequence_};
}

LessonStorageHilStatus LessonStorageHilController::Status() const {
    LockGuard lock(mutex_);
    return {
        cache_key_,
        active_,
        reached_,
        consumed_,
        operation_,
        checkpoint_,
        action_,
        threshold_,
        declared_asset_bytes_,
        pause_seconds_,
        arm_sequence_,
        reached_sequence_,
        consumed_sequence_,
    };
}

void LessonStorageHilController::Reset() {
    LockGuard lock(mutex_);
    ClearStatus();
}

std::uint64_t LessonStorageHilController::NextEvidenceSequence() noexcept {
    LockGuard lock(mutex_);
    return next_sequence_ == 0 ? 0 : TakeSequence();
}

std::size_t LessonStorageHilController::LimitDownloadRead(
    const char* cache_key,
    std::size_t downloaded,
    std::size_t requested,
    std::size_t declared_asset_bytes
) noexcept {
    LockGuard lock(mutex_);
    if (!active_ || operation_ != LessonStorageHilOperation::kSync ||
        checkpoint_ != LessonStorageHilCheckpoint::kAfterDownloadBytes ||
        declared_asset_bytes != declared_asset_bytes_ || !KeyMatches(cache_key)) {
        return requested;
    }
    if (downloaded >= threshold_) {
        return 0;
    }
    return std::min(requested, static_cast<std::size_t>(threshold_) - downloaded);
}

LessonStorageHilDecision LessonStorageHilController::Observe(
    const char* cache_key,
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    std::uint32_t progress,
    std::uint32_t declared_asset_bytes
) noexcept {
    LockGuard lock(mutex_);
    LessonStorageHilDecision decision{
        false, false, LessonStorageHilAction::kFail, 0, 0, 0, 0};
    if (!active_ || operation != operation_ || checkpoint != checkpoint_ ||
        !KeyMatches(cache_key)) {
        return decision;
    }
    if (checkpoint_ == LessonStorageHilCheckpoint::kAfterUnlinks &&
        progress < threshold_) {
        return decision;
    }
    if (checkpoint_ == LessonStorageHilCheckpoint::kAfterDownloadBytes &&
        (progress < threshold_ || declared_asset_bytes != declared_asset_bytes_)) {
        return decision;
    }
    if (next_sequence_ == 0 ||
        next_sequence_ > std::numeric_limits<std::uint64_t>::max() - 1) {
        return decision;
    }

    reached_ = true;
    reached_sequence_ = TakeSequence();
    consumed_ = true;
    consumed_sequence_ = TakeSequence();
    active_ = false;
    decision.matched = true;
    decision.consumed = true;
    decision.action = action_;
    decision.sequence = consumed_sequence_;
    decision.pause_seconds = pause_seconds_;
    decision.reached_sequence = reached_sequence_;
    decision.consumed_sequence = consumed_sequence_;
    return decision;
}

bool LessonStorageHilController::KeyMatches(const char* cache_key) const noexcept {
    if (cache_key == nullptr) {
        return false;
    }
    std::size_t index = 0;
    while (index < cache_key_.size()) {
        if (cache_key[index] != cache_key_[index]) {
            return false;
        }
        if (cache_key_[index] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

bool LessonStorageHilController::HasLifecycleSequences() const noexcept {
    return next_sequence_ != 0 &&
           next_sequence_ <= std::numeric_limits<std::uint64_t>::max() - 2;
}

std::uint64_t LessonStorageHilController::TakeSequence() noexcept {
    const std::uint64_t sequence = next_sequence_;
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        next_sequence_ = 0;
    } else {
        ++next_sequence_;
    }
    return sequence;
}

void LessonStorageHilController::ClearStatus() noexcept {
    cache_key_.fill('\0');
    active_ = false;
    reached_ = false;
    consumed_ = false;
    operation_ = LessonStorageHilOperation::kEvict;
    checkpoint_ = LessonStorageHilCheckpoint::kBeforeFirstUnlink;
    action_ = LessonStorageHilAction::kFail;
    threshold_ = 0;
    declared_asset_bytes_ = 0;
    pause_seconds_ = 0;
    arm_sequence_ = 0;
    reached_sequence_ = 0;
    consumed_sequence_ = 0;
}
