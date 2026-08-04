#ifndef LESSON_CINEMATIC_HIL_TELEMETRY_H
#define LESSON_CINEMATIC_HIL_TELEMETRY_H

#include <cstddef>
#include <cstdint>

namespace tbot {

enum class LessonCinematicHilCueEndReason : std::uint8_t {
    kNatural,
    kStop,
    kCancel,
    kReplacement,
    kFailure,
    kDiscard,
};

enum class LessonCinematicHilFault : std::uint8_t {
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

bool LessonCinematicHilTelemetryEnabled();
void LessonCinematicHilTelemetryBoot();
void LessonCinematicHilTelemetryBeginCue(const char* cue_id, std::uint64_t sequence,
                                         std::uint64_t now_ms);
void LessonCinematicHilTelemetryRecordFrameQueued(std::uint64_t now_ms);
void LessonCinematicHilTelemetryRecordRead(std::uint64_t read_ms);
void LessonCinematicHilTelemetryRecordPanelCompletion(std::uint64_t now_ms);
void LessonCinematicHilTelemetryRecordQueueError();
void LessonCinematicHilTelemetryRecordQueueTimeout();
void LessonCinematicHilTelemetryRecordDmaError();
void LessonCinematicHilTelemetryRecordParserFailure();
void LessonCinematicHilTelemetryRecordHeaderCrcError();
void LessonCinematicHilTelemetryRecordFrameCrcError();
void LessonCinematicHilTelemetryRecordIoError();
void LessonCinematicHilTelemetryRecordLateTick(std::uint32_t missed_periods);
bool LessonCinematicHilTelemetryFormatCueEnd(LessonCinematicHilCueEndReason reason,
                                             LessonCinematicHilFault fault,
                                             std::uint64_t now_ms, char* out,
                                             std::size_t out_size);
void LessonCinematicHilTelemetryEmitCueEnd(LessonCinematicHilCueEndReason reason,
                                           LessonCinematicHilFault fault,
                                           std::uint64_t now_ms);

void LessonCinematicHilTelemetryResetForTest();
void LessonCinematicHilTelemetrySetBootForTest(std::uint64_t boot_nonce,
                                               const char* reset_reason,
                                               std::uint32_t internal_heap_min,
                                               std::uint32_t psram_heap_min);
void LessonCinematicHilTelemetrySetCueHeapMinimaForTest(
    std::uint32_t lifetime_internal_heap_min, std::uint32_t internal_heap_min,
    std::uint32_t psram_heap_min);

}  // namespace tbot

#endif  // LESSON_CINEMATIC_HIL_TELEMETRY_H
