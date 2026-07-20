#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "lesson_asset_storage_coordinator.h"

namespace {

int checks = 0;

void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

LessonAssetStorageCoordinator& Coordinator() {
    return LessonAssetStorageCoordinator::GetInstance();
}

void ResetCoordinator() {
    Coordinator().ForceEndLessonSession();
    Expect(!Coordinator().HasMutation(), "test must not inherit a mutation reservation");
    Expect(!Coordinator().HasLessonSession(), "forced reset must clear lesson reservation");
}

void TestMutationReservationAndCodes() {
    ResetCoordinator();
    auto mutation = Coordinator().TryBeginMutation("evict");
    Expect(static_cast<bool>(mutation), "first mutation must acquire");
    Expect(mutation.code() == LessonAssetReservationCode::kAcquired,
           "acquired mutation must expose acquired code");
    Expect(Coordinator().HasMutation(), "coordinator must report active mutation");

    auto second = Coordinator().TryBeginMutation("sync");
    Expect(!second, "second mutation must refuse immediately");
    Expect(second.code() == LessonAssetReservationCode::kMutationActive,
           "second mutation must report mutation_active");
    const auto lesson = Coordinator().TryBeginLessonSession("assignment-a", "session-a");
    Expect(!lesson.acquired, "lesson prepare must not overlap mutation");
    Expect(!lesson.idempotent, "mutation conflict cannot be idempotent");
    Expect(lesson.code == LessonAssetReservationCode::kMutationActive,
           "lesson prepare must report mutation_active");
}

void TestMoveConstructionAndSingleRelease() {
    ResetCoordinator();
    {
        auto original = Coordinator().TryBeginMutation("evict");
        LessonAssetMutationLease moved(std::move(original));
        Expect(!original, "moved-from lease must be empty");
        Expect(static_cast<bool>(moved),
               "move-constructed lease must own reservation");
        Expect(Coordinator().HasMutation(), "move construction must not release reservation");
    }
    Expect(!Coordinator().HasMutation(), "lease destructor must release reservation once");

    auto next = Coordinator().TryBeginMutation("sync");
    Expect(static_cast<bool>(next),
           "reservation must be reusable after destructor release");
}

void TestMoveAssignmentReleasesPreviousOwnership() {
    ResetCoordinator();
    auto owner = Coordinator().TryBeginMutation("evict");
    auto refused = Coordinator().TryBeginMutation("sync");
    Expect(owner && !refused, "move assignment fixture must have one owner");

    owner = std::move(refused);
    Expect(!owner, "assigning a refused lease must leave destination empty");
    Expect(!Coordinator().HasMutation(),
           "move assignment must release destination's previous reservation");

    auto replacement = Coordinator().TryBeginMutation("sync");
    Expect(static_cast<bool>(replacement),
           "move-assignment release must permit a new mutation");
    auto destination = Coordinator().TryBeginMutation("evict");
    Expect(!destination, "fixture destination must start refused");
    destination = std::move(replacement);
    Expect(destination && !replacement, "move assignment must transfer ownership");
}

void AcquireAndReturnEarly() {
    auto mutation = Coordinator().TryBeginMutation("evict");
    Expect(static_cast<bool>(mutation), "early-return fixture must acquire");
}

void AcquireAndThrow() {
    auto mutation = Coordinator().TryBeginMutation("sync");
    Expect(static_cast<bool>(mutation), "exception fixture must acquire");
    throw std::runtime_error("expected host-test exception");
}

void TestScopeAndExceptionRelease() {
    ResetCoordinator();
    AcquireAndReturnEarly();
    Expect(!Coordinator().HasMutation(), "early return must release mutation lease");

    try {
        AcquireAndThrow();
    } catch (const std::runtime_error&) {
        Expect(true, "expected exception observed");
    }
    Expect(!Coordinator().HasMutation(), "exception unwinding must release mutation lease");
}

void TestLessonSessionOwnership() {
    ResetCoordinator();
    const auto first = Coordinator().TryBeginLessonSession("assignment-a", "session-a");
    Expect(first.code == LessonAssetReservationCode::kAcquired && first.acquired,
           "first lesson session must acquire");
    Expect(!first.idempotent, "first lesson session cannot be idempotent");
    Expect(first.generation != 0, "acquired lesson session must have nonzero generation");
    Expect(Coordinator().HasLessonSession(), "coordinator must report lesson session");

    const auto duplicate = Coordinator().TryBeginLessonSession("assignment-a", "session-a");
    Expect(duplicate.code == LessonAssetReservationCode::kAcquired && duplicate.acquired,
           "same lesson session must be accepted");
    Expect(duplicate.idempotent, "same lesson session must be idempotent");
    Expect(duplicate.generation == first.generation,
           "idempotent prepare must preserve generation");

    const auto foreign_assignment =
        Coordinator().TryBeginLessonSession("assignment-b", "session-a");
    Expect(foreign_assignment.code == LessonAssetReservationCode::kLessonSessionMismatch,
           "foreign assignment must report session mismatch");
    Expect(!foreign_assignment.acquired && !foreign_assignment.idempotent,
           "foreign assignment must not acquire");
    const auto foreign_session =
        Coordinator().TryBeginLessonSession("assignment-a", "session-b");
    Expect(foreign_session.code == LessonAssetReservationCode::kLessonSessionMismatch,
           "foreign session id must report session mismatch");

    auto mutation = Coordinator().TryBeginMutation("evict");
    Expect(!mutation, "mutation must not overlap prepared lesson");
    Expect(mutation.code() == LessonAssetReservationCode::kLessonSessionActive,
           "mutation must report lesson_session_active");

    Expect(!Coordinator().EndLessonSession("assignment-b", "session-a", first.generation),
           "foreign assignment cannot release owner");
    Expect(!Coordinator().EndLessonSession("assignment-a", "session-b", first.generation),
           "foreign session cannot release owner");
    Expect(!Coordinator().EndLessonSession("assignment-a", "session-a", 0),
           "zero generation cannot release owner");
    Expect(!Coordinator().EndLessonSession(
               "assignment-a", "session-a", first.generation + 1),
           "wrong generation cannot release owner");
    Expect(Coordinator().HasLessonSession(), "foreign release must preserve owner");
    Expect(Coordinator().EndLessonSession(
               "assignment-a", "session-a", first.generation),
           "exact owner must release lesson session");
    Expect(!Coordinator().HasLessonSession(), "exact release must clear session");
    Expect(!Coordinator().EndLessonSession(
               "assignment-a", "session-a", first.generation),
           "duplicate terminal release must return false");
}

void TestIdentityValidation() {
    ResetCoordinator();
    const std::string max_identity(kLessonAssetIdentityMaxBytes, 'a');
    const std::string too_large(kLessonAssetIdentityMaxBytes + 1, 'b');
    std::string embedded_nul = "valid-id";
    embedded_nul.insert(3, 1, '\0');

    for (const auto& identities : {
             std::pair<std::string, std::string>{"", "session"},
             std::pair<std::string, std::string>{"assignment", ""},
             std::pair<std::string, std::string>{embedded_nul, "session"},
             std::pair<std::string, std::string>{"assignment", embedded_nul},
             std::pair<std::string, std::string>{too_large, "session"},
             std::pair<std::string, std::string>{"assignment", too_large},
         }) {
        const auto result =
            Coordinator().TryBeginLessonSession(identities.first, identities.second);
        Expect(result.code == LessonAssetReservationCode::kInvalidIdentity,
               "invalid identity must return invalid_identity");
        Expect(!result.acquired && !result.idempotent && result.generation == 0,
               "invalid identity result must be inert");
        Expect(!Coordinator().HasLessonSession(),
               "invalid identity must not mutate coordinator state");
    }

    const auto accepted =
        Coordinator().TryBeginLessonSession(max_identity, max_identity);
    Expect(accepted.acquired && accepted.generation != 0,
           "maximum-length identities must be accepted");
    Expect(Coordinator().EndLessonSession(
               max_identity, max_identity, accepted.generation),
           "maximum-length identity owner must release");
}

void TestForceTeardownRejectsStaleGeneration() {
    ResetCoordinator();
    const auto old_owner =
        Coordinator().TryBeginLessonSession("assignment-aba", "session-aba");
    Expect(old_owner.acquired, "ABA fixture must acquire old owner");
    Coordinator().ForceEndLessonSession();
    const auto new_owner =
        Coordinator().TryBeginLessonSession("assignment-aba", "session-aba");
    Expect(new_owner.acquired && new_owner.generation != old_owner.generation,
           "reacquired same identity must receive a fresh generation");
    Expect(!Coordinator().EndLessonSession(
               "assignment-aba", "session-aba", old_owner.generation),
           "stale generation must not release reacquired owner");
    Expect(Coordinator().HasLessonSession(),
           "stale release must preserve reacquired owner");
    Expect(Coordinator().EndLessonSession(
               "assignment-aba", "session-aba", new_owner.generation),
           "fresh generation must release reacquired owner");
}

void TestGenerationWrapFailsClosed() {
    ResetCoordinator();
    Expect(Coordinator().SetLastGenerationForTest(
               std::numeric_limits<std::uint64_t>::max() - 1),
           "wrap fixture must set generation while idle");
    const auto final_owner =
        Coordinator().TryBeginLessonSession("assignment-wrap", "session-wrap");
    Expect(final_owner.acquired &&
               final_owner.generation == std::numeric_limits<std::uint64_t>::max(),
           "maximum nonzero generation may be issued once");
    Expect(Coordinator().EndLessonSession(
               "assignment-wrap", "session-wrap", final_owner.generation),
           "maximum generation owner must release");
    const auto exhausted =
        Coordinator().TryBeginLessonSession("assignment-wrap", "session-wrap");
    Expect(exhausted.code == LessonAssetReservationCode::kGenerationExhausted,
           "generation wrap must fail closed");
    Expect(!exhausted.acquired && exhausted.generation == 0,
           "generation exhaustion must never issue zero");
    Expect(!Coordinator().HasLessonSession(),
           "generation exhaustion must leave coordinator free");
    Expect(Coordinator().SetLastGenerationForTest(0),
           "wrap fixture must restore generation while idle");
}

void TestForceTeardown() {
    ResetCoordinator();
    Expect(Coordinator().TryBeginLessonSession("assignment-a", "session-a").acquired,
           "force teardown fixture must acquire");
    Coordinator().ForceEndLessonSession();
    Expect(!Coordinator().HasLessonSession(), "forced teardown must clear session");
    Coordinator().ForceEndLessonSession();
    Expect(!Coordinator().HasLessonSession(), "forced teardown must be idempotent");
    auto mutation = Coordinator().TryBeginMutation("evict");
    Expect(static_cast<bool>(mutation),
           "mutation must acquire after forced teardown");
}

void TestConcurrentForceReacquireRejectsStaleRelease() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        ResetCoordinator();
        const auto old_owner =
            Coordinator().TryBeginLessonSession("assignment-force", "session-force");
        Expect(old_owner.acquired, "force race must acquire old owner");
        std::atomic<bool> reacquired{false};
        std::atomic<std::uint64_t> new_generation{0};
        std::atomic<bool> stale_release_result{true};

        std::thread teardown_and_reacquire([&]() {
            Coordinator().ForceEndLessonSession();
            const auto replacement = Coordinator().TryBeginLessonSession(
                "assignment-force", "session-force");
            new_generation.store(replacement.generation, std::memory_order_release);
            reacquired.store(replacement.acquired, std::memory_order_release);
        });
        std::thread stale_terminal([&]() {
            while (!reacquired.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            stale_release_result.store(
                Coordinator().EndLessonSession(
                    "assignment-force", "session-force", old_owner.generation),
                std::memory_order_release);
        });
        teardown_and_reacquire.join();
        stale_terminal.join();

        Expect(new_generation.load() != 0 &&
                   new_generation.load() != old_owner.generation,
               "force race replacement must have a fresh generation");
        Expect(!stale_release_result.load(),
               "concurrent stale terminal must not release replacement");
        Expect(Coordinator().HasLessonSession(),
               "replacement must remain active after stale terminal");
        Expect(Coordinator().EndLessonSession(
                   "assignment-force", "session-force", new_generation.load()),
               "replacement generation must release after race");
    }
}

void WaitForStart(const std::atomic<int>& ready, const std::atomic<bool>& start) {
    while (ready.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void TestMutationVersusMutationRace() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        ResetCoordinator();
        std::atomic<int> ready{0};
        std::atomic<int> attempted{0};
        std::atomic<int> acquired{0};
        std::atomic<int> mutation_conflicts{0};
        std::atomic<bool> start{false};

        auto contender = [&](const char* operation) {
            ready.fetch_add(1, std::memory_order_release);
            WaitForStart(ready, start);
            auto lease = Coordinator().TryBeginMutation(operation);
            if (lease) {
                acquired.fetch_add(1, std::memory_order_relaxed);
            } else if (lease.code() == LessonAssetReservationCode::kMutationActive) {
                mutation_conflicts.fetch_add(1, std::memory_order_relaxed);
            }
            attempted.fetch_add(1, std::memory_order_release);
            while (attempted.load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }
        };

        std::thread first(contender, "evict");
        std::thread second(contender, "sync");
        while (ready.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        first.join();
        second.join();

        Expect(acquired.load() == 1, "mutation race must have exactly one winner");
        Expect(mutation_conflicts.load() == 1,
               "mutation race loser must receive mutation_active");
        Expect(!Coordinator().HasMutation(), "mutation race must not leak reservation");
    }
}

void TestMutationVersusPrepareRace() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        ResetCoordinator();
        std::atomic<int> ready{0};
        std::atomic<int> attempted{0};
        std::atomic<int> mutation_winners{0};
        std::atomic<int> lesson_winners{0};
        std::atomic<int> expected_conflicts{0};
        std::atomic<std::uint64_t> lesson_generation{0};
        std::atomic<bool> start{false};

        std::thread mutation([&]() {
            ready.fetch_add(1, std::memory_order_release);
            WaitForStart(ready, start);
            auto lease = Coordinator().TryBeginMutation("evict");
            if (lease) {
                mutation_winners.fetch_add(1, std::memory_order_relaxed);
            } else if (lease.code() == LessonAssetReservationCode::kLessonSessionActive) {
                expected_conflicts.fetch_add(1, std::memory_order_relaxed);
            }
            attempted.fetch_add(1, std::memory_order_release);
            while (attempted.load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }
        });
        std::thread lesson([&]() {
            ready.fetch_add(1, std::memory_order_release);
            WaitForStart(ready, start);
            const auto result =
                Coordinator().TryBeginLessonSession("assignment-race", "session-race");
            if (result.acquired) {
                lesson_winners.fetch_add(1, std::memory_order_relaxed);
                lesson_generation.store(result.generation, std::memory_order_release);
            } else if (result.code == LessonAssetReservationCode::kMutationActive) {
                expected_conflicts.fetch_add(1, std::memory_order_relaxed);
            }
            attempted.fetch_add(1, std::memory_order_release);
            while (attempted.load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }
        });

        while (ready.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        mutation.join();
        lesson.join();

        Expect(mutation_winners.load() + lesson_winners.load() == 1,
               "mutation/prepare race must have exactly one winner");
        Expect(expected_conflicts.load() == 1,
               "mutation/prepare race loser must receive stable conflict code");
        const bool exact_session_released =
            lesson_winners.load() == 0 ||
            Coordinator().EndLessonSession(
                "assignment-race", "session-race", lesson_generation.load());
        Expect(exact_session_released,
               "race lesson winner must release exact reservation");
        Expect(!Coordinator().HasMutation() && !Coordinator().HasLessonSession(),
               "mutation/prepare race must not leak reservation");
    }
}

}  // namespace

static_assert(!std::is_copy_constructible_v<LessonAssetMutationLease>);
static_assert(!std::is_copy_assignable_v<LessonAssetMutationLease>);
static_assert(std::is_nothrow_move_constructible_v<LessonAssetMutationLease>);
static_assert(std::is_nothrow_move_assignable_v<LessonAssetMutationLease>);

int main() {
    TestMutationReservationAndCodes();
    TestMoveConstructionAndSingleRelease();
    TestMoveAssignmentReleasesPreviousOwnership();
    TestScopeAndExceptionRelease();
    TestLessonSessionOwnership();
    TestIdentityValidation();
    TestForceTeardownRejectsStaleGeneration();
    TestGenerationWrapFailsClosed();
    TestForceTeardown();
    TestConcurrentForceReacquireRejectsStaleRelease();
    TestMutationVersusMutationRace();
    TestMutationVersusPrepareRace();
    ResetCoordinator();
    std::cout << "lesson asset storage coordinator host test OK (" << checks
              << " checks)" << std::endl;
    return 0;
}
