#include "lesson_asset_storage_coordinator.h"

#include <cstring>
#include <limits>
#include <utility>

namespace {

bool IsValidIdentity(const std::string& identity) {
    return !identity.empty() && identity.size() <= kLessonAssetIdentityMaxBytes &&
           identity.find('\0') == std::string::npos;
}

}  // namespace

LessonAssetReadLease::LessonAssetReadLease(
    LessonAssetStorageCoordinator* coordinator,
    std::uint64_t generation
) : coordinator_(coordinator), generation_(generation) {}

LessonAssetReadLease::LessonAssetReadLease(LessonAssetReadLease&& other) noexcept
    : coordinator_(other.coordinator_), generation_(other.generation_) {
    other.coordinator_ = nullptr;
    other.generation_ = 0;
}

LessonAssetReadLease& LessonAssetReadLease::operator=(LessonAssetReadLease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    coordinator_ = other.coordinator_;
    generation_ = other.generation_;
    other.coordinator_ = nullptr;
    other.generation_ = 0;
    return *this;
}

LessonAssetReadLease::~LessonAssetReadLease() {
    Release();
}

LessonAssetReadLease::operator bool() const {
    return coordinator_ != nullptr && generation_ != 0;
}

void LessonAssetReadLease::Release() {
    if (coordinator_ != nullptr && generation_ != 0) {
        coordinator_->ReleaseLessonRead(generation_);
    }
    coordinator_ = nullptr;
    generation_ = 0;
}

LessonAssetMutationLease::LessonAssetMutationLease(
    LessonAssetStorageCoordinator* coordinator,
    LessonAssetReservationCode code,
    bool owns_reservation,
    tbot::SdFatSessionLease sd_session
)
    : coordinator_(coordinator), code_(code), owns_reservation_(owns_reservation),
      sd_session_(std::move(sd_session)) {}

LessonAssetMutationLease::LessonAssetMutationLease(
    LessonAssetMutationLease&& other
) noexcept
    : coordinator_(other.coordinator_),
      code_(other.code_),
      owns_reservation_(other.owns_reservation_),
      sd_session_(std::move(other.sd_session_)) {
    other.coordinator_ = nullptr;
    other.owns_reservation_ = false;
}

LessonAssetMutationLease& LessonAssetMutationLease::operator=(
    LessonAssetMutationLease&& other
) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    coordinator_ = other.coordinator_;
    code_ = other.code_;
    owns_reservation_ = other.owns_reservation_;
    sd_session_ = std::move(other.sd_session_);
    other.coordinator_ = nullptr;
    other.owns_reservation_ = false;
    return *this;
}

LessonAssetMutationLease::~LessonAssetMutationLease() {
    Release();
}

LessonAssetMutationLease::operator bool() const {
    return owns_reservation_;
}

LessonAssetReservationCode LessonAssetMutationLease::code() const {
    return code_;
}

void LessonAssetMutationLease::Release() {
    if (!owns_reservation_ || coordinator_ == nullptr) {
        return;
    }
    sd_session_ = {};
    coordinator_->ReleaseMutation();
    coordinator_ = nullptr;
    owns_reservation_ = false;
}

LessonAssetStorageCoordinator& LessonAssetStorageCoordinator::GetInstance() {
    static LessonAssetStorageCoordinator coordinator;
    return coordinator;
}

LessonAssetMutationLease LessonAssetStorageCoordinator::TryBeginMutation(
    const char* operation
) {
    const MutationOperationLabel operation_label = ClassifyOperationLabel(operation);
    std::lock_guard<std::mutex> lock(mutex_);
    if (mutation_active_) {
        return LessonAssetMutationLease(
            nullptr, LessonAssetReservationCode::kMutationActive, false
        );
    }
    if (lesson_session_active_) {
        return LessonAssetMutationLease(
            nullptr, LessonAssetReservationCode::kLessonSessionActive, false
        );
    }
    auto sd_session = tbot::SdFatSessionGuard::GetInstance().TryAcquire();
    if (!sd_session) {
        return LessonAssetMutationLease(
            nullptr, LessonAssetReservationCode::kMutationActive, false
        );
    }

    mutation_active_ = true;
    mutation_operation_ = operation_label;
    return LessonAssetMutationLease(
        this, LessonAssetReservationCode::kAcquired, true, std::move(sd_session)
    );
}

LessonAssetSessionResult LessonAssetStorageCoordinator::TryBeginLessonSession(
    const std::string& assignment_id,
    const std::string& session_id
) {
    if (!IsValidIdentity(assignment_id) || !IsValidIdentity(session_id)) {
        return {LessonAssetReservationCode::kInvalidIdentity, false, false, 0};
    }

    std::string validated_assignment_id(assignment_id);
    std::string validated_session_id(session_id);
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef TBOT_LESSON_ASSET_COORDINATOR_TESTING
    ++lesson_session_reservation_attempts_for_test_;
#endif
    if (mutation_active_) {
        return {LessonAssetReservationCode::kMutationActive, false, false, 0};
    }
    if (lesson_session_active_) {
        if (assignment_id_ == validated_assignment_id &&
            session_id_ == validated_session_id) {
            return {
                LessonAssetReservationCode::kAcquired,
                true,
                true,
                lesson_session_generation_,
            };
        }
        return {
            LessonAssetReservationCode::kLessonSessionMismatch, false, false, 0
        };
    }
    if (last_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return {
            LessonAssetReservationCode::kGenerationExhausted, false, false, 0
        };
    }

    auto sd_session = tbot::SdFatSessionGuard::GetInstance().TryAcquire();
    if (!sd_session) {
        return {LessonAssetReservationCode::kMutationActive, false, false, 0};
    }
    const std::uint64_t generation = last_generation_ + 1;
    assignment_id_.swap(validated_assignment_id);
    session_id_.swap(validated_session_id);
    lesson_session_generation_ = generation;
    last_generation_ = generation;
    lesson_session_active_ = true;
    lesson_sd_session_.emplace(std::move(sd_session));
    return {LessonAssetReservationCode::kAcquired, true, false, generation};
}

LessonAssetReadLease LessonAssetStorageCoordinator::TryRetainLessonSession(
    const std::string& assignment_id,
    const std::string& session_id,
    std::uint64_t generation
) {
    if (generation == 0 || !IsValidIdentity(assignment_id) || !IsValidIdentity(session_id)) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lesson_session_active_ || assignment_id_ != assignment_id || session_id_ != session_id ||
        lesson_session_generation_ != generation || lesson_readers_ == std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    ++lesson_readers_;
    return LessonAssetReadLease(this, generation);
}

bool LessonAssetStorageCoordinator::EndLessonSession(
    const std::string& assignment_id,
    const std::string& session_id,
    std::uint64_t generation
) {
    if (generation == 0 || !IsValidIdentity(assignment_id) ||
        !IsValidIdentity(session_id)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lesson_session_active_ || lesson_readers_ != 0 || assignment_id_ != assignment_id ||
        session_id_ != session_id || lesson_session_generation_ != generation) {
        return false;
    }
    lesson_session_active_ = false;
    assignment_id_.clear();
    session_id_.clear();
    lesson_session_generation_ = 0;
    lesson_readers_ = 0;
    lesson_sd_session_.reset();
    return true;
}

void LessonAssetStorageCoordinator::ForceEndLessonSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    lesson_session_active_ = false;
    if (lesson_readers_ != 0) {
        return;
    }
    assignment_id_.clear();
    session_id_.clear();
    lesson_session_generation_ = 0;
    lesson_readers_ = 0;
    lesson_sd_session_.reset();
}

bool LessonAssetStorageCoordinator::HasMutation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mutation_active_;
}

bool LessonAssetStorageCoordinator::HasLessonSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lesson_session_active_;
}

#ifdef TBOT_LESSON_ASSET_COORDINATOR_TESTING
bool LessonAssetStorageCoordinator::SetLastGenerationForTest(
    std::uint64_t generation
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mutation_active_ || lesson_session_active_) {
        return false;
    }
    last_generation_ = generation;
    return true;
}

void LessonAssetStorageCoordinator::ResetLessonSessionReservationAttemptsForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    lesson_session_reservation_attempts_for_test_ = 0;
}

std::uint64_t LessonAssetStorageCoordinator::LessonSessionReservationAttemptsForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lesson_session_reservation_attempts_for_test_;
}
#endif

LessonAssetStorageCoordinator::MutationOperationLabel
LessonAssetStorageCoordinator::ClassifyOperationLabel(const char* operation) {
    if (operation == nullptr) {
        return MutationOperationLabel::kOther;
    }
    if (std::strcmp(operation, "evict") == 0) {
        return MutationOperationLabel::kEvict;
    }
    if (std::strcmp(operation, "sync") == 0) {
        return MutationOperationLabel::kSync;
    }
    return MutationOperationLabel::kOther;
}

void LessonAssetStorageCoordinator::ReleaseMutation() {
    std::lock_guard<std::mutex> lock(mutex_);
    mutation_active_ = false;
    mutation_operation_ = MutationOperationLabel::kNone;
}

void LessonAssetStorageCoordinator::ReleaseLessonRead(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lesson_session_generation_ == generation && lesson_readers_ != 0) {
        --lesson_readers_;
        if (lesson_readers_ == 0 && !lesson_session_active_) {
            assignment_id_.clear();
            session_id_.clear();
            lesson_session_generation_ = 0;
            lesson_sd_session_.reset();
        }
    }
}
