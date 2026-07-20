#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "lesson_storage_hil_controller.h"
#include "lesson_storage_hil_u64_format.h"

struct LessonStorageHilControllerTestPeer {
    static void SetNextSequence(std::uint64_t value) {
        auto& controller = LessonStorageHilController::GetInstance();
        LessonStorageHilController::LockGuard lock(controller.mutex_);
        controller.next_sequence_ = value;
    }
};

namespace {

int checks = 0;

void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

constexpr const char* kHilKey =
    "hil-space/v1-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kOtherHilKey =
    "hil-space/v2-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* kNormalKey =
    "space/v1-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

void TestUint64DecimalFormatting() {
    static_assert(noexcept(FormatLessonStorageHilUint64(0)));
    const auto zero = FormatLessonStorageHilUint64(0);
    const auto normal = FormatLessonStorageHilUint64(1234567890123456789ULL);
    const auto maximum = FormatLessonStorageHilUint64(
        std::numeric_limits<std::uint64_t>::max());

    Expect(std::string(zero.c_str()) == "0", "zero sequence must format as decimal");
    Expect(std::string(normal.c_str()) == "1234567890123456789",
           "normal sequence must format as decimal");
    Expect(std::string(maximum.c_str()) == "18446744073709551615",
           "UINT64_MAX sequence must format as decimal");
}

LessonStorageHilController& Controller() {
    return LessonStorageHilController::GetInstance();
}

LessonStorageHilArmRequest Request(
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    LessonStorageHilAction action,
    std::uint32_t threshold = 0,
    std::uint32_t declared_asset_bytes = 0,
    std::uint32_t pause_seconds = 0
) {
    return {
        kHilKey,
        operation,
        checkpoint,
        action,
        threshold,
        declared_asset_bytes,
        pause_seconds,
    };
}

void ExpectArmCode(
    const LessonStorageHilArmRequest& request,
    LessonStorageHilArmCode expected,
    const char* message
) {
    Controller().Reset();
    const auto result = Controller().Arm(request);
    Expect(result.code == expected, message);
    Expect(result.armed == (expected == LessonStorageHilArmCode::kArmed),
           "arm result armed flag must agree with code");
}

void TestCacheKeyValidation() {
    auto request = Request(
        LessonStorageHilOperation::kEvict,
        LessonStorageHilCheckpoint::kBeforeFirstUnlink,
        LessonStorageHilAction::kFail);
    ExpectArmCode(request, LessonStorageHilArmCode::kArmed,
                  "canonical HIL key must arm");

    request.cache_key = kNormalKey;
    ExpectArmCode(request, LessonStorageHilArmCode::kInvalidCacheKey,
                  "canonical non-HIL key must be rejected");
    request.cache_key = "Hil-space/v1-" + std::string(64, 'a');
    ExpectArmCode(request, LessonStorageHilArmCode::kInvalidCacheKey,
                  "noncanonical HIL spelling must be rejected");
    request.cache_key = "hil-space//v1-" + std::string(64, 'a');
    ExpectArmCode(request, LessonStorageHilArmCode::kInvalidCacheKey,
                  "noncanonical path must be rejected");
    request.cache_key = std::string(kHilKey) + "x";
    ExpectArmCode(request, LessonStorageHilArmCode::kInvalidCacheKey,
                  "oversize checksum must be rejected");
    request.cache_key = std::string(kHilKey, 12);
    request.cache_key.push_back('\0');
    request.cache_key.append(kHilKey + 12);
    ExpectArmCode(request, LessonStorageHilArmCode::kInvalidCacheKey,
                  "embedded NUL key must be rejected");
}

void TestClosedCompatibilityMatrix() {
    using O = LessonStorageHilOperation;
    using C = LessonStorageHilCheckpoint;
    using A = LessonStorageHilAction;
    struct Allowed {
        O operation;
        C checkpoint;
        A action;
        std::uint32_t threshold;
        std::uint32_t declared;
    };
    const std::vector<Allowed> allowed = {
        {O::kEvict, C::kBeforeFirstUnlink, A::kFail, 0, 0},
        {O::kEvict, C::kBeforeFirstUnlink, A::kPause, 0, 0},
        {O::kEvict, C::kAfterUnlinks, A::kFail, 1, 0},
        {O::kEvict, C::kAfterUnlinks, A::kPause, 64, 0},
        {O::kEvict, C::kBeforeRmdir, A::kFail, 0, 0},
        {O::kEvict, C::kBeforeRmdir, A::kPause, 0, 0},
        {O::kSync, C::kBeforeDownloadWrite, A::kFail, 0, 0},
        {O::kSync, C::kBeforeDownloadWrite, A::kPause, 0, 0},
        {O::kSync, C::kBeforeDownloadWrite, A::kNoSpace, 0, 0},
        {O::kSync, C::kAfterDownloadBytes, A::kFail, 1, 1},
        {O::kSync, C::kAfterDownloadBytes, A::kPause, 9, 10},
        {O::kSync, C::kAfterDownloadBytes, A::kNoSpace, 512 * 1024, 512 * 1024},
        {O::kSync, C::kBeforeChecksumVerify, A::kFail, 0, 0},
        {O::kSync, C::kBeforeChecksumVerify, A::kPause, 0, 0},
        {O::kSync, C::kBeforeChecksumVerify, A::kCorruptStaging, 0, 0},
        {O::kSync, C::kBeforeCommitRename, A::kFail, 0, 0},
        {O::kSync, C::kBeforeCommitRename, A::kPause, 0, 0},
        {O::kSync, C::kBeforeCommitRename, A::kNoSpace, 0, 0},
    };
    for (const auto& item : allowed) {
        const auto pause = item.action == A::kPause ? 5U : 0U;
        ExpectArmCode(Request(item.operation, item.checkpoint, item.action,
                              item.threshold, item.declared, pause),
                      LessonStorageHilArmCode::kArmed,
                      "allowed matrix entry must arm");
    }

    const O operations[] = {O::kEvict, O::kSync};
    const C checkpoints[] = {
        C::kBeforeFirstUnlink, C::kAfterUnlinks, C::kBeforeRmdir,
        C::kBeforeDownloadWrite, C::kAfterDownloadBytes,
        C::kBeforeChecksumVerify, C::kBeforeCommitRename,
    };
    const A actions[] = {A::kFail, A::kPause, A::kNoSpace, A::kCorruptStaging};
    int forbidden = 0;
    for (const auto operation : operations) {
        for (const auto checkpoint : checkpoints) {
            for (const auto action : actions) {
                bool is_allowed = false;
                for (const auto& item : allowed) {
                    is_allowed = is_allowed ||
                        (item.operation == operation && item.checkpoint == checkpoint &&
                         item.action == action);
                }
                if (is_allowed) continue;
                ++forbidden;
                const auto result = Controller().Arm(Request(
                    operation, checkpoint, action, 0, 0,
                    action == A::kPause ? 5U : 0U));
                Expect(result.code == LessonStorageHilArmCode::kInvalidCombination,
                       "forbidden matrix entry must be rejected");
                Controller().Reset();
            }
        }
    }
    Expect(forbidden == 38, "closed matrix must reject all 38 unlisted pairs");
}

void TestNumericValidation() {
    using O = LessonStorageHilOperation;
    using C = LessonStorageHilCheckpoint;
    using A = LessonStorageHilAction;
    ExpectArmCode(Request(O::kEvict, C::kAfterUnlinks, A::kFail, 0),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "after-unlinks threshold zero must reject");
    ExpectArmCode(Request(O::kEvict, C::kAfterUnlinks, A::kFail, 65),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "after-unlinks threshold above 64 must reject");
    ExpectArmCode(Request(O::kEvict, C::kBeforeRmdir, A::kFail, 1),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "non-count checkpoint threshold must be zero");
    ExpectArmCode(Request(O::kSync, C::kAfterDownloadBytes, A::kFail, 0, 1),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "download byte threshold must be positive");
    ExpectArmCode(Request(O::kSync, C::kAfterDownloadBytes, A::kFail, 2, 1),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "declared bytes must cover threshold");
    ExpectArmCode(Request(O::kSync, C::kAfterDownloadBytes, A::kFail,
                          1, 512 * 1024 + 1),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "declared bytes must be bounded");
    ExpectArmCode(Request(O::kSync, C::kBeforeDownloadWrite, A::kFail, 0, 1),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "non-byte checkpoint must reject declared bytes");
    ExpectArmCode(Request(O::kEvict, C::kBeforeRmdir, A::kPause, 0, 0, 4),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "pause below five seconds must reject");
    ExpectArmCode(Request(O::kEvict, C::kBeforeRmdir, A::kPause, 0, 0, 5),
                  LessonStorageHilArmCode::kArmed,
                  "five-second pause must arm");
    ExpectArmCode(Request(O::kEvict, C::kBeforeRmdir, A::kPause, 0, 0, 60),
                  LessonStorageHilArmCode::kArmed,
                  "sixty-second pause must arm");
    ExpectArmCode(Request(O::kEvict, C::kBeforeRmdir, A::kPause, 0, 0, 61),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "pause above sixty seconds must reject");
    ExpectArmCode(Request(O::kEvict, C::kBeforeRmdir, A::kFail, 0, 0, 5),
                  LessonStorageHilArmCode::kInvalidThreshold,
                  "non-pause action must reject pause duration");
    ExpectArmCode(Request(static_cast<O>(99), C::kBeforeRmdir, A::kFail),
                  LessonStorageHilArmCode::kInvalidCombination,
                  "unknown operation must reject");
    ExpectArmCode(Request(O::kEvict, static_cast<C>(99), A::kFail),
                  LessonStorageHilArmCode::kInvalidCombination,
                  "unknown checkpoint must reject");
    ExpectArmCode(Request(O::kEvict, C::kBeforeRmdir, static_cast<A>(99)),
                  LessonStorageHilArmCode::kInvalidCombination,
                  "unknown action must reject");
}

void TestObserveMatchingAndOneShot() {
    using O = LessonStorageHilOperation;
    using C = LessonStorageHilCheckpoint;
    using A = LessonStorageHilAction;
    Controller().Reset();
    const auto arm = Controller().Arm(Request(O::kSync, C::kAfterDownloadBytes,
                                               A::kNoSpace, 10, 20));
    Expect(arm.code == LessonStorageHilArmCode::kArmed && arm.arm_sequence != 0,
           "valid request must receive nonzero arm sequence");
    const auto initial = Controller().Status();
    Expect(initial.armed && !initial.reached && !initial.consumed,
           "fresh status must be armed and unreached");
    Expect(initial.arm_sequence == arm.arm_sequence,
           "status must expose arm sequence");

    Expect(!Controller().Observe(kOtherHilKey, O::kSync, C::kAfterDownloadBytes,
                                 10, 20).matched,
           "foreign key must not match");
    Expect(!Controller().Observe(nullptr, O::kSync, C::kAfterDownloadBytes,
                                 10, 20).matched,
           "null key must not match");
    Expect(!Controller().Observe(kHilKey, O::kEvict, C::kAfterDownloadBytes,
                                 10, 20).matched,
           "foreign operation must not match");
    Expect(!Controller().Observe(kHilKey, O::kSync, C::kBeforeChecksumVerify,
                                 10, 20).matched,
           "foreign checkpoint must not match");
    Expect(!Controller().Observe(kHilKey, O::kSync, C::kAfterDownloadBytes,
                                 9, 20).matched,
           "progress below threshold must not match");
    Expect(!Controller().Observe(kHilKey, O::kSync, C::kAfterDownloadBytes,
                                 10, 21).matched,
           "different declared bytes must not consume");
    Expect(Controller().Status().armed, "nonmatches must leave arm active");

    const auto decision = Controller().Observe(
        kHilKey, O::kSync, C::kAfterDownloadBytes, 10, 20);
    Expect(decision.matched && decision.consumed,
           "exact observation must consume");
    Expect(decision.action == A::kNoSpace && decision.pause_seconds == 0,
           "decision must preserve configured action");
    Expect(decision.sequence > arm.arm_sequence,
           "consumed decision sequence must follow arm");
    const auto consumed = Controller().Status();
    Expect(!consumed.armed && consumed.reached && consumed.consumed,
           "consumed status must be retained after clearing arm");
    Expect(consumed.reached_sequence > consumed.arm_sequence &&
               consumed.consumed_sequence > consumed.reached_sequence &&
               decision.sequence == consumed.consumed_sequence,
           "status sequences must be nonzero and strictly ordered");
    Expect(!Controller().Observe(kHilKey, O::kSync, C::kAfterDownloadBytes,
                                 10, 20).matched,
           "one-shot arm must not consume twice");

    const auto already = Controller().Arm(Request(
        O::kEvict, C::kBeforeRmdir, A::kFail));
    Expect(already.code == LessonStorageHilArmCode::kArmed,
           "new arm is allowed after consumption");
    const auto duplicate = Controller().Arm(Request(
        O::kEvict, C::kBeforeRmdir, A::kFail));
    Expect(duplicate.code == LessonStorageHilArmCode::kAlreadyArmed &&
               duplicate.armed && duplicate.arm_sequence == already.arm_sequence,
           "second active arm must report original sequence");
}

void TestResetDoesNotReuseSequence() {
    using O = LessonStorageHilOperation;
    using C = LessonStorageHilCheckpoint;
    using A = LessonStorageHilAction;
    Controller().Reset();
    const auto first = Controller().Arm(Request(O::kEvict, C::kBeforeRmdir, A::kFail));
    Controller().Reset();
    const auto cleared = Controller().Status();
    Expect(!cleared.armed && !cleared.reached && !cleared.consumed &&
               cleared.arm_sequence == 0 && cleared.reached_sequence == 0 &&
               cleared.consumed_sequence == 0,
           "reset must clear volatile arm and visible status");
    const auto second = Controller().Arm(Request(O::kEvict, C::kBeforeRmdir, A::kFail));
    Expect(second.arm_sequence > first.arm_sequence,
           "reset must not permit boot sequence reuse");
}

void TestDownloadReadLimiter() {
    using O = LessonStorageHilOperation;
    using C = LessonStorageHilCheckpoint;
    using A = LessonStorageHilAction;
    Controller().Reset();
    Controller().Arm(Request(O::kSync, C::kAfterDownloadBytes, A::kFail, 10, 20));
    Expect(Controller().LimitDownloadRead(kHilKey, 0, 4, 20) == 4,
           "read entirely below threshold must be unchanged");
    Expect(Controller().LimitDownloadRead(kHilKey, 7, 8, 20) == 3,
           "crossing read must stop exactly at threshold");
    Expect(Controller().LimitDownloadRead(kHilKey, 10, 8, 20) == 0,
           "read at armed threshold must not overshoot");
    Expect(Controller().LimitDownloadRead(kOtherHilKey, 7, 8, 20) == 8,
           "foreign key read must be unchanged");
    Expect(Controller().LimitDownloadRead(kHilKey, 7, 8, 21) == 8,
           "foreign declared size read must be unchanged");
    Expect(Controller().Status().armed,
           "read limiter must not consume by itself");
    Controller().Observe(kHilKey, O::kSync, C::kAfterDownloadBytes, 10, 20);
    Expect(Controller().LimitDownloadRead(kHilKey, 10, 8, 20) == 8,
           "consumed limiter must stop affecting reads");
}

void TestConcurrentArmAndObserve() {
    using O = LessonStorageHilOperation;
    using C = LessonStorageHilCheckpoint;
    using A = LessonStorageHilAction;
    for (int iteration = 0; iteration < 100; ++iteration) {
        Controller().Reset();
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::atomic<int> armed{0};
        std::atomic<int> already{0};
        auto arm_once = [&]() {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            const auto result = Controller().Arm(Request(
                O::kEvict, C::kAfterUnlinks, A::kFail, 1));
            if (result.code == LessonStorageHilArmCode::kArmed) armed.fetch_add(1);
            if (result.code == LessonStorageHilArmCode::kAlreadyArmed) already.fetch_add(1);
        };
        std::thread first(arm_once);
        std::thread second(arm_once);
        while (ready.load() != 2) std::this_thread::yield();
        go.store(true);
        first.join();
        second.join();
        Expect(armed.load() == 1 && already.load() == 1,
               "concurrent arm must publish exactly one record");

        std::atomic<int> consumed{0};
        auto observe_once = [&]() {
            const auto decision = Controller().Observe(
                kHilKey, O::kEvict, C::kAfterUnlinks, 1, 0);
            if (decision.consumed) consumed.fetch_add(1);
        };
        std::thread observer1(observe_once);
        std::thread observer2(observe_once);
        observer1.join();
        observer2.join();
        Expect(consumed.load() == 1,
               "concurrent observe must consume at most once");
    }
}

void TestSequenceExhaustionFailsClosed() {
    using O = LessonStorageHilOperation;
    using C = LessonStorageHilCheckpoint;
    using A = LessonStorageHilAction;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();

    Controller().Reset();
    LessonStorageHilControllerTestPeer::SetNextSequence(maximum - 2);
    const auto final_arm = Controller().Arm(Request(
        O::kEvict, C::kBeforeRmdir, A::kFail));
    Expect(final_arm.code == LessonStorageHilArmCode::kArmed &&
               final_arm.arm_sequence == maximum - 2,
           "last complete lifecycle must reserve final three sequences");
    const auto final_decision = Controller().Observe(
        kHilKey, O::kEvict, C::kBeforeRmdir, 0, 0);
    const auto final_status = Controller().Status();
    Expect(final_decision.consumed && final_decision.sequence == maximum,
           "last lifecycle must consume maximum sequence without wrapping");
    Expect(final_status.reached_sequence == maximum - 1 &&
               final_status.consumed_sequence == maximum,
           "last reached and consumed sequences must remain ordered");

    Controller().Reset();
    const auto exhausted = Controller().Arm(Request(
        O::kEvict, C::kBeforeRmdir, A::kFail));
    Expect(exhausted.code == LessonStorageHilArmCode::kSequenceExhausted &&
               !exhausted.armed && exhausted.arm_sequence == 0,
           "wrapped sequence state must fail closed");

    LessonStorageHilControllerTestPeer::SetNextSequence(1);
    Controller().Reset();
}

}  // namespace

int main() {
    TestUint64DecimalFormatting();
    TestCacheKeyValidation();
    TestClosedCompatibilityMatrix();
    TestNumericValidation();
    TestObserveMatchingAndOneShot();
    TestResetDoesNotReuseSequence();
    TestDownloadReadLimiter();
    TestConcurrentArmAndObserve();
    TestSequenceExhaustionFailsClosed();
    Controller().Reset();
    std::cout << "lesson storage HIL controller host test OK (" << checks
              << " checks)" << std::endl;
    return 0;
}
