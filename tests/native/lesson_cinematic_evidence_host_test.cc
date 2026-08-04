#include "lesson_cinematic_evidence.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(ESP_PLATFORM) && defined(CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE) && \
    CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE
#include <esp_heap_caps.h>
#include <esp_random.h>
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

void TestEspCueHeapMonitorResetsForEveryCue() {
    HostHilLifetimeInternalHeapMinimum() = 12000;
    HostHilLifetimePsramHeapMinimum() = 4200000;
    HostHilCurrentInternalHeapFree() = 60000;
    HostHilCurrentPsramHeapFree() = 4100000;
    HostHilHeapMonitorActive() = false;
    HostHilHeapMonitorStartCalls() = 0;
    HostHilHeapMonitorStopCalls() = 0;

    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceBoot();
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 100);
    Require(HostHilHeapMonitorActive(), "first cue starts a local heap minimum monitor");
    HostHilLocalInternalHeapMinimum() = 21000;
    char first[768] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 200, first, sizeof(first)),
            "first ESP cue heap line formats");
    Require(std::string(first).find("internal_heap_min=21000") != std::string::npos,
            "first ESP cue reports the local heap minimum");
    Require(std::string(first).find("lifetime_internal_heap_min=12000") != std::string::npos,
            "first ESP cue keeps the boot/sync lifetime minimum diagnostic separate");
    Require(!HostHilHeapMonitorActive(), "terminal formatting stops the first cue monitor");

    HostHilCurrentInternalHeapFree() = 52000;
    tbot::LessonCinematicEvidenceBeginCue("barn-teach", 2, 300);
    char second[768] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 400, second, sizeof(second)),
            "second ESP cue heap line formats");
    Require(std::string(second).find("internal_heap_min=52000") != std::string::npos,
            "second ESP cue resets its minimum instead of inheriting the first cue");
    Require(HostHilHeapMonitorStartCalls() == 2, "each ESP cue starts one local monitor");
    Require(HostHilHeapMonitorStopCalls() == 2, "each ESP cue stops one local monitor");
}

void TestEspBootRepairsAnAllZeroRandomNonceWithOneBoundedRetry() {
    HostEspRandomValue() = 0;
    HostEspRandomCalls() = 0;

    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceBoot();
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 1, 100);
    char line[768] = {};
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
                reinterpret_cast<char*>(static_cast<std::uintptr_t>(1)), 768),
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
    char line[768] = {};
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

void TestPanelCompletionLatencyIsNotOverwrittenWithoutNewQueue() {
    tbot::LessonCinematicEvidenceResetForTest();
    tbot::LessonCinematicEvidenceSetBootForTest(0x1234abcdULL, "poweron", 61000, 4200000);
    tbot::LessonCinematicEvidenceBeginCue("barn-opening", 7, 1000);
    tbot::LessonCinematicEvidenceRecordFrameQueued(1050);
    tbot::LessonCinematicEvidenceRecordPanelCompletion(1062);
    tbot::LessonCinematicEvidenceRecordPanelCompletion(1900);
    char line[768] = {};
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
    char first[768] = {};
    Require(tbot::LessonCinematicEvidenceFormatCueEnd(
                tbot::LessonCinematicCueEndReason::kNatural,
                tbot::LessonCinematicFault::kNone, 200, first, sizeof(first)),
            "first cue heap line formats");
    Require(std::string(first).find("internal_heap_min=21000") != std::string::npos,
            "first cue reports its local minimum despite a lower lifetime minimum");

    tbot::LessonCinematicEvidenceBeginCue("barn-teach", 2, 300);
    tbot::LessonCinematicEvidenceSetCueHeapMinimaForTest(12000, 52000, 4000000);
    char second[768] = {};
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
    char line[768] = {};
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
    char line[768] = {};
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
    char line[768] = {};
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
            char line[768] = {};
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
    TestEspCueHeapMonitorResetsForEveryCue();
    TestEspBootRepairsAnAllZeroRandomNonceWithOneBoundedRetry();
#else
    TestEspDisabledEntryPointsAreNoOps();
#endif
#else
    TestProductionOffByDefault();
    TestCueEndLineContainsRequiredCountersAndTerminator();
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
