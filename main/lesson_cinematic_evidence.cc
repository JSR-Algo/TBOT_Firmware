#include "lesson_cinematic_evidence.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <mutex>

#if defined(ESP_PLATFORM) && defined(CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE) && \
    CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE
#define TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED 1
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#else
#define TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED 0
#endif

namespace tbot {
namespace {

struct CueCounters {
    bool active = false;
    char cue_id[40] = {};
    std::uint64_t sequence = 0;
    std::uint64_t started_ms = 0;
    std::uint64_t last_queue_ms = 0;
    bool panel_completion_recorded = false;
    std::uint32_t read_count = 0;
    std::uint32_t read_ge70ms = 0;
    std::uint32_t read_hist[3] = {};
    std::uint32_t read_max_ms = 0;
    std::uint32_t panel_latency_ms = 0;
    std::uint32_t queue_errors = 0;
    std::uint32_t queue_timeouts = 0;
    std::uint32_t dma_errors = 0;
    std::uint32_t parser_errors = 0;
    std::uint32_t header_crc_errors = 0;
    std::uint32_t frame_crc_errors = 0;
    std::uint32_t io_errors = 0;
    std::uint32_t watchdog_faults = 0;
    std::uint32_t unexpected_reset_faults = 0;
    std::uint32_t late_ticks = 0;
    std::uint32_t missed_periods = 0;
    std::uint32_t lifetime_internal_heap_min = 0;
    std::uint32_t internal_heap_min = 0;
    std::uint32_t psram_heap_min = 0;
    bool heap_minima_recorded = false;
    bool heap_monitor_active = false;
};

struct BootCounters {
    std::uint64_t boot_nonce = 0;
    char reset_reason[24] = "unknown";
    std::uint32_t lifetime_internal_heap_min = 0;
    std::uint32_t psram_heap_min = 0;
};

std::mutex g_mutex;
CueCounters g_cue;
BootCounters g_boot;

const char* ReasonText(LessonCinematicCueEndReason reason) {
    switch (reason) {
        case LessonCinematicCueEndReason::kNatural:
            return "natural";
        case LessonCinematicCueEndReason::kStop:
            return "stop";
        case LessonCinematicCueEndReason::kCancel:
            return "cancel";
        case LessonCinematicCueEndReason::kReplacement:
            return "replacement";
        case LessonCinematicCueEndReason::kFailure:
            return "failure";
        case LessonCinematicCueEndReason::kDiscard:
            return "discard";
    }
    return "failure";
}

const char* FaultText(LessonCinematicFault fault) {
    switch (fault) {
        case LessonCinematicFault::kNone:
            return "none";
        case LessonCinematicFault::kParser:
            return "parser";
        case LessonCinematicFault::kHeaderCrc:
            return "header_crc";
        case LessonCinematicFault::kFrameCrc:
            return "frame_crc";
        case LessonCinematicFault::kIo:
            return "io";
        case LessonCinematicFault::kDma:
            return "dma";
        case LessonCinematicFault::kQueueTimeout:
            return "queue_timeout";
        case LessonCinematicFault::kWatchdog:
            return "watchdog";
        case LessonCinematicFault::kUnexpectedReset:
            return "unexpected_reset";
    }
    return "io";
}

#if TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED
std::uint64_t GenerateBootNonce() {
    std::uint64_t nonce = (static_cast<std::uint64_t>(esp_random()) << 32) | esp_random();
    if (nonce == 0) {
        nonce = (static_cast<std::uint64_t>(esp_random()) << 32) | esp_random();
    }
    return nonce != 0 ? nonce : 1;
}

const char* ResetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "unknown";
    }
}
#endif

void CopyToken(char* destination, std::size_t destination_size, const char* source) {
    if (destination_size == 0) return;
    const char* value = source != nullptr && source[0] != '\0' ? source : "unknown";
    std::snprintf(destination, destination_size, "%s", value);
}

void StopHeapMonitorLocked() {
#if TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED
    if (g_cue.heap_monitor_active) {
        heap_caps_monitor_local_minimum_free_size_stop();
        g_cue.heap_monitor_active = false;
    }
#endif
}

void RefreshCueHeapMinimaLocked() {
    if (g_cue.heap_minima_recorded) return;
#if TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED
    g_cue.internal_heap_min = static_cast<std::uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    g_cue.psram_heap_min = static_cast<std::uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    StopHeapMonitorLocked();
#endif
    g_cue.heap_minima_recorded = true;
}

bool EvidenceRuntimeEnabled() {
#ifdef ESP_PLATFORM
    return TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED;
#else
    return true;
#endif
}

}  // namespace

bool LessonCinematicEvidenceEnabled() {
#if defined(CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE) && CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE
    return CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE;
#else
    return false;
#endif
}

void LessonCinematicEvidenceBoot() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
#if TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED
    g_boot.boot_nonce = GenerateBootNonce();
    CopyToken(g_boot.reset_reason, sizeof(g_boot.reset_reason),
              ResetReasonText(esp_reset_reason()));
    g_boot.lifetime_internal_heap_min = static_cast<std::uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    g_boot.psram_heap_min = static_cast<std::uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    if (LessonCinematicEvidenceEnabled()) {
        esp_rom_printf("CINE_EVIDENCE event=boot boot_nonce=0x%" PRIx64 " reset_reason=%s "
                       "lifetime_internal_heap_min=%u psram_heap_min=%u\n",
                       static_cast<std::uint64_t>(g_boot.boot_nonce),
                       g_boot.reset_reason,
                       static_cast<unsigned>(g_boot.lifetime_internal_heap_min),
                       static_cast<unsigned>(g_boot.psram_heap_min));
    }
#else
    g_boot.boot_nonce = 0;
    CopyToken(g_boot.reset_reason, sizeof(g_boot.reset_reason), "host");
    g_boot.lifetime_internal_heap_min = 0;
    g_boot.psram_heap_min = 0;
#endif
}

void LessonCinematicEvidenceBeginCue(const char* cue_id, std::uint64_t sequence,
                                     std::uint64_t now_ms) {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    StopHeapMonitorLocked();
    g_cue = {};
    g_cue.active = true;
    CopyToken(g_cue.cue_id, sizeof(g_cue.cue_id), cue_id);
    g_cue.sequence = sequence;
    g_cue.started_ms = now_ms;
    g_cue.last_queue_ms = now_ms;
#if TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED
    g_cue.lifetime_internal_heap_min = static_cast<std::uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    heap_caps_monitor_local_minimum_free_size_start();
    g_cue.heap_monitor_active = true;
#else
    g_cue.lifetime_internal_heap_min = g_boot.lifetime_internal_heap_min;
    g_cue.internal_heap_min = g_boot.lifetime_internal_heap_min;
    g_cue.psram_heap_min = g_boot.psram_heap_min;
#endif
}

void LessonCinematicEvidenceRecordFrameQueued(std::uint64_t now_ms) {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) {
        g_cue.last_queue_ms = now_ms;
        g_cue.panel_completion_recorded = false;
    }
}

void LessonCinematicEvidenceRecordRead(std::uint64_t read_ms) {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cue.active) return;
    ++g_cue.read_count;
    if (read_ms >= 70) ++g_cue.read_ge70ms;
    if (read_ms < 70) {
        ++g_cue.read_hist[0];
    } else if (read_ms < 100) {
        ++g_cue.read_hist[1];
    } else {
        ++g_cue.read_hist[2];
    }
    if (read_ms > g_cue.read_max_ms) {
        g_cue.read_max_ms = read_ms > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(read_ms);
    }
}

void LessonCinematicEvidenceRecordPanelCompletion(std::uint64_t now_ms) {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cue.active || g_cue.panel_completion_recorded) return;
    const std::uint64_t basis = g_cue.last_queue_ms != 0 ? g_cue.last_queue_ms : g_cue.started_ms;
    const std::uint64_t latency = now_ms >= basis ? now_ms - basis : 0;
    g_cue.panel_latency_ms =
        latency > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(latency);
    g_cue.panel_completion_recorded = true;
}

void LessonCinematicEvidenceRecordQueueError() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) ++g_cue.queue_errors;
}

void LessonCinematicEvidenceRecordQueueTimeout() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) ++g_cue.queue_timeouts;
}

void LessonCinematicEvidenceRecordDmaError() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) ++g_cue.dma_errors;
}

void LessonCinematicEvidenceRecordParserFailure() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) ++g_cue.parser_errors;
}

void LessonCinematicEvidenceRecordHeaderCrcError() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) ++g_cue.header_crc_errors;
}

void LessonCinematicEvidenceRecordFrameCrcError() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) ++g_cue.frame_crc_errors;
}

void LessonCinematicEvidenceRecordIoError() {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cue.active) ++g_cue.io_errors;
}

void LessonCinematicEvidenceRecordLateTick(std::uint32_t missed_periods) {
    if (!EvidenceRuntimeEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cue.active) return;
    ++g_cue.late_ticks;
    g_cue.missed_periods += missed_periods;
}

bool LessonCinematicEvidenceFormatCueEnd(LessonCinematicCueEndReason reason,
                                         LessonCinematicFault fault,
                                         std::uint64_t now_ms, char* out,
                                         std::size_t out_size) {
    if (!EvidenceRuntimeEnabled()) return false;
    if (out == nullptr || out_size == 0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cue.active) return false;
    RefreshCueHeapMinimaLocked();
    const std::uint64_t latency = now_ms >= g_cue.started_ms ? now_ms - g_cue.started_ms : 0;
    const int written = std::snprintf(
        out, out_size,
        "CINE_EVIDENCE event=cue_end cue=%s reason=%s fault=%s seq=%" PRIu64 " latency_ms=%" PRIu64 " "
        "read_count=%u read_ge70ms=%u read_max_ms=%u read_hist_ms=0:%u,70:%u,100:%u "
        "panel_latency_ms=%u queue_errors=%u queue_timeouts=%u dma_errors=%u "
        "parser_errors=%u header_crc_errors=%u frame_crc_errors=%u io_errors=%u "
        "watchdog_faults=%u unexpected_reset_faults=%u "
        "late_ticks=%u missed_periods=%u internal_heap_min=%u "
        "lifetime_internal_heap_min=%u psram_heap_min=%u "
        "boot_nonce=0x%" PRIx64 " reset_reason=%s cue_end",
        g_cue.cue_id, ReasonText(reason), FaultText(fault),
        static_cast<std::uint64_t>(g_cue.sequence),
        static_cast<std::uint64_t>(latency), static_cast<unsigned>(g_cue.read_count),
        static_cast<unsigned>(g_cue.read_ge70ms), static_cast<unsigned>(g_cue.read_max_ms),
        static_cast<unsigned>(g_cue.read_hist[0]), static_cast<unsigned>(g_cue.read_hist[1]),
        static_cast<unsigned>(g_cue.read_hist[2]), static_cast<unsigned>(g_cue.panel_latency_ms),
        static_cast<unsigned>(g_cue.queue_errors), static_cast<unsigned>(g_cue.queue_timeouts),
        static_cast<unsigned>(g_cue.dma_errors), static_cast<unsigned>(g_cue.parser_errors),
        static_cast<unsigned>(g_cue.header_crc_errors),
        static_cast<unsigned>(g_cue.frame_crc_errors), static_cast<unsigned>(g_cue.io_errors),
        static_cast<unsigned>(g_cue.watchdog_faults),
        static_cast<unsigned>(g_cue.unexpected_reset_faults),
        static_cast<unsigned>(g_cue.late_ticks), static_cast<unsigned>(g_cue.missed_periods),
        static_cast<unsigned>(g_cue.internal_heap_min),
        static_cast<unsigned>(g_cue.lifetime_internal_heap_min),
        static_cast<unsigned>(g_cue.psram_heap_min),
        static_cast<std::uint64_t>(g_boot.boot_nonce), g_boot.reset_reason);
    if (written > 0 && static_cast<std::size_t>(written) < out_size) {
        g_cue.active = false;
        return true;
    }
    return false;
}

void LessonCinematicEvidenceEmitCueEnd(LessonCinematicCueEndReason reason,
                                       LessonCinematicFault fault,
                                       std::uint64_t now_ms) {
    if (!EvidenceRuntimeEnabled()) return;
    char line[768] = {};
    if (!LessonCinematicEvidenceFormatCueEnd(reason, fault, now_ms, line, sizeof(line))) {
        return;
    }
#if TBOT_ESP_RELEASE_CINEMATIC_EVIDENCE_ENABLED
    esp_rom_printf("%s\n", line);
#endif
}

void LessonCinematicEvidenceResetForTest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    StopHeapMonitorLocked();
    g_cue = {};
    g_boot = {};
    CopyToken(g_boot.reset_reason, sizeof(g_boot.reset_reason), "host");
}

void LessonCinematicEvidenceSetBootForTest(std::uint64_t boot_nonce,
                                           const char* reset_reason,
                                           std::uint32_t internal_heap_min,
                                           std::uint32_t psram_heap_min) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_boot.boot_nonce = boot_nonce;
    CopyToken(g_boot.reset_reason, sizeof(g_boot.reset_reason), reset_reason);
    g_boot.lifetime_internal_heap_min = internal_heap_min;
    g_boot.psram_heap_min = psram_heap_min;
}

void LessonCinematicEvidenceSetCueHeapMinimaForTest(
    std::uint32_t lifetime_internal_heap_min, std::uint32_t internal_heap_min,
    std::uint32_t psram_heap_min) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cue.lifetime_internal_heap_min = lifetime_internal_heap_min;
    g_cue.internal_heap_min = internal_heap_min;
    g_cue.psram_heap_min = psram_heap_min;
    g_cue.heap_minima_recorded = true;
}

}  // namespace tbot
