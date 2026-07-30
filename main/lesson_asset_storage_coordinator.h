#ifndef LESSON_ASSET_STORAGE_COORDINATOR_H
#define LESSON_ASSET_STORAGE_COORDINATOR_H

#include "sd_fat_session_guard.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

inline constexpr std::size_t kLessonAssetIdentityMaxBytes = 128;

enum class LessonAssetReservationCode {
    kAcquired,
    kMutationActive,
    kLessonSessionActive,
    kLessonSessionMismatch,
    kInvalidIdentity,
    kGenerationExhausted,
};

class LessonAssetStorageCoordinator;

class LessonAssetReadLease {
public:
    LessonAssetReadLease() = default;
    LessonAssetReadLease(LessonAssetReadLease&& other) noexcept;
    LessonAssetReadLease& operator=(LessonAssetReadLease&& other) noexcept;
    ~LessonAssetReadLease();
    explicit operator bool() const;

    LessonAssetReadLease(const LessonAssetReadLease&) = delete;
    LessonAssetReadLease& operator=(const LessonAssetReadLease&) = delete;

private:
    friend class LessonAssetStorageCoordinator;
    LessonAssetReadLease(LessonAssetStorageCoordinator* coordinator, std::uint64_t generation);
    void Release();

    LessonAssetStorageCoordinator* coordinator_{nullptr};
    std::uint64_t generation_{0};
};

class LessonAssetMutationLease {
public:
    LessonAssetMutationLease(LessonAssetMutationLease&& other) noexcept;
    LessonAssetMutationLease& operator=(LessonAssetMutationLease&& other) noexcept;
    ~LessonAssetMutationLease();

    explicit operator bool() const;
    LessonAssetReservationCode code() const;

    LessonAssetMutationLease(const LessonAssetMutationLease&) = delete;
    LessonAssetMutationLease& operator=(const LessonAssetMutationLease&) = delete;

private:
    friend class LessonAssetStorageCoordinator;

    LessonAssetMutationLease(
        LessonAssetStorageCoordinator* coordinator,
        LessonAssetReservationCode code,
        bool owns_reservation,
        tbot::SdFatSessionLease sd_session = {}
    );
    void Release();

    LessonAssetStorageCoordinator* coordinator_;
    LessonAssetReservationCode code_;
    bool owns_reservation_;
    tbot::SdFatSessionLease sd_session_;
};

struct LessonAssetSessionResult {
    LessonAssetReservationCode code;
    bool acquired;
    bool idempotent;
    std::uint64_t generation;
};

class LessonAssetStorageCoordinator {
public:
    static LessonAssetStorageCoordinator& GetInstance();

    LessonAssetMutationLease TryBeginMutation(const char* operation);
    LessonAssetSessionResult TryBeginLessonSession(
        const std::string& assignment_id,
        const std::string& session_id
    );
    LessonAssetReadLease TryRetainLessonSession(
        const std::string& assignment_id,
        const std::string& session_id,
        std::uint64_t generation
    );
    bool EndLessonSession(
        const std::string& assignment_id,
        const std::string& session_id,
        std::uint64_t generation
    );
    void ForceEndLessonSession();
    bool HasMutation() const;
    bool HasLessonSession() const;

#ifdef TBOT_LESSON_ASSET_COORDINATOR_TESTING
    bool SetLastGenerationForTest(std::uint64_t generation);
#endif

    LessonAssetStorageCoordinator(const LessonAssetStorageCoordinator&) = delete;
    LessonAssetStorageCoordinator& operator=(const LessonAssetStorageCoordinator&) = delete;

private:
    friend class LessonAssetMutationLease;
    friend class LessonAssetReadLease;

    enum class MutationOperationLabel : std::uint8_t {
        kNone,
        kEvict,
        kSync,
        kOther,
    };

    LessonAssetStorageCoordinator() = default;
    static MutationOperationLabel ClassifyOperationLabel(const char* operation);
    void ReleaseMutation();
    void ReleaseLessonRead(std::uint64_t generation);

    mutable std::mutex mutex_;
    bool mutation_active_ = false;
    MutationOperationLabel mutation_operation_ = MutationOperationLabel::kNone;
    bool lesson_session_active_ = false;
    std::string assignment_id_;
    std::string session_id_;
    std::uint64_t lesson_session_generation_ = 0;
    std::size_t lesson_readers_ = 0;
    std::uint64_t last_generation_ = 0;
    std::optional<tbot::SdFatSessionLease> lesson_sd_session_;
};

#endif  // LESSON_ASSET_STORAGE_COORDINATOR_H
