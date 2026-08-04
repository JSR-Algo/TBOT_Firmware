#ifndef LESSON_CINEMATIC_EVIDENCE_H
#define LESSON_CINEMATIC_EVIDENCE_H

#include <cstddef>
#include <cstdint>

namespace tbot {

// Covers the longest canonical cue and maximum-width numeric fields with safety margin.
inline constexpr std::size_t kLessonCinematicEvidenceLineCapacity = 1024;

// cue_end heap minima are the lowest current-heap samples observed at cue begin,
// each Record* boundary, and cue end. lifetime_internal_heap_min remains the
// allocator's lifetime minimum and is reported separately.

enum class LessonCinematicCueEndReason : std::uint8_t {
    kNatural,
    kStop,
    kCancel,
    kReplacement,
    kFailure,
    kDiscard,
};

enum class LessonCinematicFault : std::uint8_t {
    kNone,
    kParser,
    kHeaderCrc,
    kFrameCrc,
    kIo,
    kDma,
    kQueueTimeout,
    kWatchdog,
    kUnexpectedReset,
};

bool LessonCinematicEvidenceEnabled();
void LessonCinematicEvidenceBoot();
void LessonCinematicEvidenceBeginCue(const char* cue_id, std::uint64_t sequence,
                                     std::uint64_t now_ms);
void LessonCinematicEvidenceRecordFrameQueued(std::uint64_t now_ms);
void LessonCinematicEvidenceRecordRead(std::uint64_t read_ms);
void LessonCinematicEvidenceRecordPanelCompletion(std::uint64_t now_ms);
void LessonCinematicEvidenceRecordQueueError();
void LessonCinematicEvidenceRecordQueueTimeout();
void LessonCinematicEvidenceRecordDmaError();
void LessonCinematicEvidenceRecordParserFailure();
void LessonCinematicEvidenceRecordHeaderCrcError();
void LessonCinematicEvidenceRecordFrameCrcError();
void LessonCinematicEvidenceRecordIoError();
void LessonCinematicEvidenceRecordLateTick(std::uint32_t missed_periods);
bool LessonCinematicEvidenceFormatCueEnd(LessonCinematicCueEndReason reason,
                                         LessonCinematicFault fault,
                                         std::uint64_t now_ms, char* out,
                                         std::size_t out_size);
void LessonCinematicEvidenceEmitCueEnd(LessonCinematicCueEndReason reason,
                                       LessonCinematicFault fault,
                                       std::uint64_t now_ms);

void LessonCinematicEvidenceResetForTest();
void LessonCinematicEvidenceSetBootForTest(std::uint64_t boot_nonce,
                                           const char* reset_reason,
                                           std::uint32_t internal_heap_min,
                                           std::uint32_t psram_heap_min);
void LessonCinematicEvidenceSetCueHeapMinimaForTest(
    std::uint32_t lifetime_internal_heap_min, std::uint32_t internal_heap_min,
    std::uint32_t psram_heap_min);

}  // namespace tbot

#endif  // LESSON_CINEMATIC_EVIDENCE_H
