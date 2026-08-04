#include "lesson_cinematic_evidence.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(ESP_PLATFORM) && defined(CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE) && \
    CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_rom_sys.h>
#include <freertos/semphr.h>
#endif

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson cinematic evidence test failed: " << message << '\n';
        std::exit(1);
    }
}

#if defined(ESP_PLATFORM)

#if defined(CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE) && \
    CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE

void TestEspEvidenceMutexUsesOneStaticSemaphore() {
    Require(HostEvidenceStaticMutexCreateCalls() == 1,
            "ESP evidence mutex is created exactly once from static storage");
    const unsigned take_calls = HostEvidenceStaticMutexTakeCalls();
    const unsigned give_calls = HostEvidenceStaticMutexGiveCalls();

    tbot::LessonCinematicEvidenceResetForTest();

    Require(HostEvidenceStaticMutexTakeCalls() == take_calls + 1,
            "ESP evidence state locks through the static semaphore");
    Require(HostEvidenceStaticMutexGiveCalls() == give_calls + 1,
            "ESP evidence state unlocks through the static semaphore");
    Require(HostEvidenceStaticMutexLastWait() == portMAX_DELAY,
            "ESP evidence mutex waits without spinning across formatting or heap queries");
    Require(HostEvidenceStaticMutexHandle() != nullptr &&
                !HostEvidenceStaticMutexHandle()->held,
            "ESP evidence mutex is released after the guarded operation");
}

void TestEspCueHeapMinimumSamplesEveryEvidenceBoundary() {
    HostHilLifetimeInternalHeapMinimum() = 12000;
    HostHilLifetimePsramHeapMinimum() = 4200000;
    HostHilCurrentInternalHeapFree() = 60000;
    HostHilCurrentPsramHeapFree() = 4100000;

    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceBoot();
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 100);

    HostHilCurrentInternalHeapFree() = 48000;
    HostHilCurrentPsramHeapFree() = 4000000;
    tbot::LessonCinematicEvidenceRecordRead(10);
    HostHilCurrentInternalHeapFree() = 52000;
    HostHilCurrentPsramHeapFree() = 4050000;
    tbot::LessonCinematicEvidenceRecordQueueError();
    HostHilCurrentInternalHeapFree() = 47000;
    HostHilCurrentPsramHeapFree() = 3900000;

    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 200, line, sizeof(line)),
            "sampled ESP cue heap line formats");
    Require(std::string(line).find("internal_heap_min=47000") != std::string::npos,
            "cue reports the lowest internal heap sampled at evidence boundaries");
    Require(std::string(line).find("psram_heap_min=3900000") != std::string::npos,
            "cue reports the lowest PSRAM heap sampled at evidence boundaries");
    Require(std::string(line).find("lifetime_internal_heap_min=12000") != std::string::npos,
            "cue preserves the allocator lifetime minimum as a separate diagnostic");

    HostHilCurrentInternalHeapFree() = 52000;
    HostHilCurrentPsramHeapFree() = 4050000;
    tbot::LessonCinematicEvidenceBeginCue("barn-teach", 2, 300);
    char second[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 400, second, sizeof(second)),
            "second sampled ESP cue heap line formats");
    Require(std::string(second).find("internal_heap_min=52000") != std::string::npos,
            "each cue starts a fresh sampled internal heap minimum");
    Require(std::string(second).find("psram_heap_min=4050000") != std::string::npos,
            "each cue starts a fresh sampled PSRAM heap minimum");
}

void TestEspZeroHeapSampleRemainsVisibleAsTheCueMinimum() {
    HostHilCurrentInternalHeapFree() = 60000;
    HostHilCurrentPsramHeapFree() = 4100000;
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceBoot();
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 100);

    HostHilCurrentInternalHeapFree() = 0;
    tbot::LessonCinematicEvidenceRecordQueueError();
    HostHilCurrentInternalHeapFree() = 50000;

    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kFailure,
                tbot::LessonCinematicFault::kIo, 200, line, sizeof(line)),
            "zero-heap sampled ESP cue line formats");
    Require(std::string(line).find("internal_heap_min=0") != std::string::npos,
            "a sampled zero heap value cannot be replaced by a later recovery sample");
}

void TestEspBootRepairsAnAllZeroRandomNonceWithOneBoundedRetry() {
    HostEspRandomValue() = 0;
    HostEspRandomCalls() = 0;
    HostEspRomOutputReset();

    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceBoot();
    const std::string boot_line = HostEspRomOutput();
    Require(boot_line.rfind("CINE_EVIDENCE event=boot ", 0) == 0,
            "zero-RNG repair remains visible in the emitted boot schema");
    Require(boot_line.find("boot_nonce=0x0 ") == std::string::npos,
            "emitted boot evidence never contains a zero nonce");
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 100);
    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 200, line, sizeof(line)),
            "zero-RNG ESP cue line formats");
    Require(std::string(line).find("boot_nonce=0x0 ") == std::string::npos,
            "zero RNG cannot produce a zero boot nonce");
    Require(HostEspRandomCalls() == 4,
            "zero RNG receives exactly one bounded two-word retry");

    HostEspRandomValue() = 0x1234abcdU;
}

void TestEspBootEmitsExactReleaseEvidenceSchema() {
    HostHilLifetimeInternalHeapMinimum() = 12000;
    HostHilLifetimePsramHeapMinimum() = 4200000;
    HostEspRandomValue() = 0x1234abcdU;
    HostEspRandomCalls() = 0;
    HostEspRomOutputReset();

    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceBoot();

    Require(std::string(HostEspRomOutput()) ==
                "CINE_EVIDENCE event=boot boot_nonce=0x1234abcd1234abcd "
                "reset_reason=poweron lifetime_internal_heap_min=12000 "
                "psram_heap_min=4200000\n",
            "ESP boot output matches the exact release evidence schema");
    Require(HostEspRandomCalls() == 2,
            "nonzero boot nonce consumes exactly one two-word RNG sample");
}

#else

void TestEspDisabledEntryPointsAreNoOps() {
    Require(!tbot::LessonCinematicEvidenceEnabled(),
            "ESP telemetry remains disabled when the Kconfig option is off");
    tbot::LessonCinematicEvidenceBoot();
    tbot::LessonCinematicEvidenceBeginCue(
        reinterpret_cast<const char*>(static_cast<std::uintptr_t>(1)), 1, 1);
    tbot::LessonCinematicEvidenceRecordFrameQueued(2);
    tbot::LessonCinematicEvidenceRecordRead(3);
    tbot::LessonCinematicEvidenceRecordPanelCompletion(4);
    tbot::LessonCinematicEvidenceRecordQueueError();
    tbot::LessonCinematicEvidenceRecordQueueTimeout();
    tbot::LessonCinematicEvidenceRecordDmaError();
    tbot::LessonCinematicEvidenceRecordParserFailure();
    tbot::LessonCinematicEvidenceRecordHeaderCrcError();
    tbot::LessonCinematicEvidenceRecordFrameCrcError();
    tbot::LessonCinematicEvidenceRecordIoError();
    tbot::LessonCinematicEvidenceRecordLateTick(5);
    Require(!tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 6,
                reinterpret_cast<char*>(static_cast<std::uintptr_t>(1)),
                tbot::kLessonCinematicEvidenceLineCapacity),
            "disabled ESP formatting returns before touching caller storage");
    tbot::LessonCinematicEvidenceEmitCueEnd(
        tbot::LessonCinematicCueEndReason::kNatural,
        tbot::LessonCinematicFault::kNone, 6);
}

#endif

#else

std::string EmitLine(tbot::LessonCinematicCueEndReason reason) {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1234abcdULL, "poweron", 61000, 4200000);
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 7, 1000);
    tbot::LessonCinematicEvidenceSetCueHeapMinimaForTest(59000, 56000, 4100000);
    tbot::LessonCinematicEvidenceRecordRead(25);
    tbot::LessonCinematicEvidenceRecordRead(71);
    tbot::LessonCinematicEvidenceRecordRead(130);
    tbot::LessonCinematicEvidenceRecordPanelCompletion(1095);
    tbot::LessonCinematicEvidenceRecordQueueError();
    tbot::LessonCinematicEvidenceRecordLateTick(3);
    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                reason, tbot::LessonCinematicFault::kNone, 1120, line, sizeof(line)),
            "cue_end line formats into bounded caller storage");
    return line;
}

void TestProductionOffByDefault() {
    Require(!tbot::LessonCinematicEvidenceEnabled(),
            "cinematic release evidence is production-off unless compile enabled");
}

void TestCueEndLineContainsRequiredCountersAndTerminator() {
    const std::string line = EmitLine(tbot::LessonCinematicCueEndReason::kNatural);
    Require(line.rfind("CINE_EVIDENCE ", 0) == 0, "line starts with CINE_EVIDENCE prefix");
    Require(line.find("event=cue_end") != std::string::npos, "line has cue_end event");
    Require(line.find("cue=barn-opening") != std::string::npos, "line has canonical cue id");
    Require(line.find("reason=natural") != std::string::npos, "line has natural reason");
    Require(line.find("seq=7") != std::string::npos, "line has sequence");
    Require(line.find("read_count=3") != std::string::npos, "line has read count");
    Require(line.find("read_ge70ms=2") != std::string::npos, "line counts slow reads");
    Require(line.find("read_max_ms=130") != std::string::npos, "line has max read");
    Require(line.find("read_hist_ms=0:1,70:1,100:1") != std::string::npos,
            "line has fixed read histogram");
    Require(line.find("panel_latency_ms=95") != std::string::npos,
            "panel latency is measured at color transfer completion");
    Require(line.find("queue_errors=1") != std::string::npos, "line has queue errors");
    Require(line.find("late_ticks=1") != std::string::npos, "line has late tick count");
    Require(line.find("missed_periods=3") != std::string::npos,
            "line has actual missed periods");
    Require(line.find("internal_heap_min=56000") != std::string::npos,
            "line has cue-scoped internal heap minimum");
    Require(line.find("lifetime_internal_heap_min=59000") != std::string::npos,
            "line keeps lifetime boot/sync heap minimum as a separate diagnostic");
    Require(line.find("psram_heap_min=4100000") != std::string::npos,
            "line has PSRAM heap minimum");
    Require(line.find("boot_nonce=0x1234abcd") != std::string::npos, "line has boot nonce");
    Require(line.find("reset_reason=poweron") != std::string::npos, "line has reset reason");
    Require(line.size() >= 7 && line.compare(line.size() - 7, 7, "cue_end") == 0,
            "line terminates with cue_end token");
}

void TestMaximumWidthCanonicalCueFitsDocumentedLineCapacityWithMargin() {
    static_assert(tbot::kLessonCinematicEvidenceLineCapacity >= 1024);
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(
        std::numeric_limits<std::uint64_t>::max(), "unexpected_reset_reason",
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max());
    tbot::LessonCinematicEvidenceBeginCue(
        "barn-to-hay-word-transition", std::numeric_limits<std::uint64_t>::max(), 0);
    tbot::LessonCinematicEvidenceSetCueHeapMinimaForTest(
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max());
    tbot::LessonCinematicEvidenceRecordRead(std::numeric_limits<std::uint64_t>::max());
    tbot::LessonCinematicEvidenceRecordLateTick(std::numeric_limits<std::uint32_t>::max());

    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kReplacement,
                tbot::LessonCinematicFault::kUnexpectedReset,
                std::numeric_limits<std::uint64_t>::max(), line, sizeof(line)),
            "maximum-width canonical cue evidence fits bounded line storage");
    const std::string text = line;
    Require(text.find("cue=barn-to-hay-word-transition") != std::string::npos,
            "maximum-width test uses the longest canonical cue id");
    Require(text.size() + 256 <= tbot::kLessonCinematicEvidenceLineCapacity,
            "bounded evidence line retains at least 256 bytes of format margin");
}

void TestPanelCompletionLatencyIsNotOverwrittenWithoutNewQueue() {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1234abcdULL, "poweron", 61000, 4200000);
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 7, 1000);
    tbot::LessonCinematicEvidenceRecordFrameQueued(1050);
    tbot::LessonCinematicEvidenceRecordPanelCompletion(1062);
    tbot::LessonCinematicEvidenceRecordPanelCompletion(1900);
    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 2000, line, sizeof(line)),
            "callback latency line formats");
    Require(std::string(line).find("panel_latency_ms=12") != std::string::npos,
            "wait-task latency cannot overwrite callback-derived panel latency");
}

void TestCueHeapMinimumResetsForEachCue() {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1ULL, "poweron", 12000, 4200000);

    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 100);
    tbot::LessonCinematicEvidenceSetCueHeapMinimaForTest(12000, 21000, 4000000);
    char first[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 200, first, sizeof(first)),
            "first cue heap line formats");
    Require(std::string(first).find("internal_heap_min=21000") != std::string::npos,
            "first cue reports its local minimum despite a lower lifetime minimum");

    tbot::LessonCinematicEvidenceBeginCue("barn-teach", 2, 300);
    tbot::LessonCinematicEvidenceSetCueHeapMinimaForTest(12000, 52000, 4000000);
    char second[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 400, second, sizeof(second)),
            "second cue heap line formats");
    Require(std::string(second).find("internal_heap_min=52000") != std::string::npos,
            "second cue resets the local minimum instead of inheriting the first cue");
    Require(std::string(second).find("lifetime_internal_heap_min=12000") != std::string::npos,
            "lifetime boot/sync minimum remains diagnostic-only across cues");
}

void TestReadAt100msIncrementsExactlyOneHistogramBucket() {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1ULL, "poweron", 61000, 4200000);
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 0);
    tbot::LessonCinematicEvidenceRecordRead(100);
    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 1, line, sizeof(line)),
            "100ms read histogram line formats");
    const std::string text = line;
    Require(text.find("read_count=1") != std::string::npos, "one read increments total once");
    Require(text.find("read_ge70ms=1") != std::string::npos,
            "100ms read increments the slow-read total once");
    Require(text.find("read_hist_ms=0:0,70:0,100:1") != std::string::npos,
            "100ms read increments exactly the 100ms histogram bucket");
}

void TestHostActivePathRemainsTestableWhenProductionDisabled() {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1ULL, "poweron", 1, 1);
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 0);
    tbot::LessonCinematicEvidenceRecordRead(1);
    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(!tbot::LessonCinematicEvidenceEnabled(), "host build reports production disabled");
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 1, line, sizeof(line)),
            "host-only telemetry test path remains active");
}

void TestFaultReasonsAreParseable() {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1ULL, "watchdog", 1, 2);
    tbot::LessonCinematicEvidenceBeginCue("hay-thinking", 99, 2000);
    tbot::LessonCinematicEvidenceRecordParserFailure();
    char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kFailure,
                tbot::LessonCinematicFault::kParser, 2010, line, sizeof(line)),
            "parser failure line formats");
    const std::string text = line;
    Require(text.find("cue=hay-thinking") != std::string::npos, "parser failure keeps cue");
    Require(text.find("reason=failure") != std::string::npos, "parser failure has reason");
    Require(text.find("fault=parser") != std::string::npos, "parser failure has fault");
    Require(text.find("parser_errors=1") != std::string::npos, "parser failure is counted");
}

void TestReplacementReasonIsParseable() {
    const std::string replacement = EmitLine(tbot::LessonCinematicCueEndReason::kReplacement);
    Require(replacement.find("reason=replacement") != std::string::npos,
            "replacement terminal reason is emitted as verifier-parseable text");
}

void TestStopReasonIsParseable() {
    const std::string stop = EmitLine(tbot::LessonCinematicCueEndReason::kStop);
    Require(stop.find("reason=stop") != std::string::npos,
            "stop terminal reason is emitted as verifier-parseable text");
}

void TestCueEndClaimIsExactlyOnceUnderConcurrentFormatters() {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1ULL, "poweron", 61000, 4200000);
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 100);

    std::vector<std::thread> threads;
    bool results[16] = {};
    for (std::size_t i = 0; i < std::size(results); ++i) {
        threads.emplace_back([i, &results] {
            char line[tbot::kLessonCinematicEvidenceLineCapacity] = {};
            results[i] = tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 200 + i, line, sizeof(line));
        });
    }
    for (auto& thread : threads) thread.join();

    int formatted = 0;
    for (bool result : results) {
        if (result) ++formatted;
    }
    Require(formatted == 1, "exactly one concurrent terminal formatter claims the cue");
}

#endif

}  // namespace

int main() {
#if defined(ESP_PLATFORM)
#if defined(CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE) && \
    CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE
    TestEspEvidenceMutexUsesOneStaticSemaphore();
    TestEspCueHeapMinimumSamplesEveryEvidenceBoundary();
    TestEspZeroHeapSampleRemainsVisibleAsTheCueMinimum();
    TestEspBootRepairsAnAllZeroRandomNonceWithOneBoundedRetry();
    TestEspBootEmitsExactReleaseEvidenceSchema();
#else
    TestEspDisabledEntryPointsAreNoOps();
#endif
#else
    TestProductionOffByDefault();
    TestCueEndLineContainsRequiredCountersAndTerminator();
    TestMaximumWidthCanonicalCueFitsDocumentedLineCapacityWithMargin();
    TestPanelCompletionLatencyIsNotOverwrittenWithoutNewQueue();
    TestCueHeapMinimumResetsForEachCue();
    TestReadAt100msIncrementsExactlyOneHistogramBucket();
    TestHostActivePathRemainsTestableWhenProductionDisabled();
    TestFaultReasonsAreParseable();
    TestReplacementReasonIsParseable();
    TestStopReasonIsParseable();
    TestCueEndClaimIsExactlyOnceUnderConcurrentFormatters();
#endif
    std::cout << "lesson cinematic evidence tests passed\n";
    return 0;
}
