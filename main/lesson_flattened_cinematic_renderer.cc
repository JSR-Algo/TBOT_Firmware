#include "lesson_flattened_cinematic_renderer.h"

#include "lesson_cinematic_hil_telemetry.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <string_view>

#ifdef ESP_PLATFORM
#include "display/lcd_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "lesson_mjpeg_mp4.h"
#include "lesson_rgb565_stream.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#endif

namespace tbot {
namespace {

std::atomic<bool> g_capability_ready{false};
std::atomic<LessonFlattenedCinematicRenderer*> g_active_renderer{nullptr};
std::atomic<bool> g_timer_route_v4{false};
std::mutex g_active_renderer_mutex;

void AppendFingerprint(std::string& destination, const char* value) {
    const std::string_view text = value != nullptr ? std::string_view(value) : std::string_view();
    destination += std::to_string(text.size());
    destination += ':';
    destination.append(text.data(), text.size());
    destination += ';';
}

std::string ControlFingerprint(const char* command, const char* phase_id) {
    std::string value;
    AppendFingerprint(value, command);
    AppendFingerprint(value, phase_id);
    return value;
}

std::string PrepareFingerprint(const LessonFlattenedCinematicPhaseConfig& config) {
    std::string value = ControlFingerprint("prepare", config.phase_id);
    AppendFingerprint(value, config.renderer_id);
    AppendFingerprint(value, config.template_id);
    AppendFingerprint(value, config.asset.derivative_id);
    AppendFingerprint(value, config.asset.phase_id);
    AppendFingerprint(value, config.asset.sd_path);
    AppendFingerprint(value, config.asset.sha256);
    AppendFingerprint(value, config.asset.media_type);
    value += std::to_string(config.template_version) + ':';
    value += std::to_string(config.duration_ms) + ':';
    value += std::to_string(config.fps) + ':';
    value += std::to_string(config.frame_count) + ':';
    value += std::to_string(config.asset.bytes) + ':';
    value += std::to_string(config.asset.width) + ':';
    value += std::to_string(config.asset.height) + ':';
    value += std::to_string(config.asset.container_version) + ':';
    value += std::to_string(config.asset.stored_width) + ':';
    value += std::to_string(config.asset.stored_height) + ':';
    AppendFingerprint(value, config.asset.orientation);
    value += std::to_string(config.asset.frame_bytes) + ':';
    value += config.loop ? '1' : '0';
    return value;
}

bool LowerHexSha256(const char* value) {
    if (value == nullptr || std::strlen(value) != 64) return false;
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        if (!((*cursor >= '0' && *cursor <= '9') || (*cursor >= 'a' && *cursor <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool LocalPath(const char* path) {
    return path != nullptr && path[0] == '/' && std::strstr(path, "://") == nullptr &&
           std::strstr(path, "..") == nullptr && std::strchr(path, '?') == nullptr &&
           std::strchr(path, '#') == nullptr && std::strchr(path, '@') == nullptr;
}

bool ExactContract(const LessonFlattenedCinematicPhaseConfig& config) {
    return config.renderer_id != nullptr && config.template_id != nullptr &&
           std::strcmp(config.renderer_id, kLessonRendererV4) == 0 &&
           std::strcmp(config.template_id, kLessonFlattenedMjpegCinematicTemplate) == 0 &&
           (config.template_version == 1 || config.template_version == 2);
}

bool ValidMetadata(const LessonFlattenedCinematicPhaseConfig& config) {
    const bool common = config.phase_id != nullptr && config.phase_id[0] != '\0' &&
           config.asset.phase_id != nullptr &&
           std::strcmp(config.asset.phase_id, config.phase_id) == 0 &&
           LowerHexSha256(config.asset.derivative_id) && LowerHexSha256(config.asset.sha256) &&
           config.asset.bytes > 0 && config.asset.media_type != nullptr && config.fps == 10 &&
           config.frame_count > 0 && config.duration_ms > 0 &&
           static_cast<std::uint64_t>(config.duration_ms) * config.fps ==
               static_cast<std::uint64_t>(config.frame_count) * 1000;
    if (!common) return false;
    if (std::strcmp(config.asset.media_type, "video/mp4") == 0) {
        return config.asset.width == kLessonCinematicWidth &&
               config.asset.height == kLessonCinematicHeight &&
               config.asset.container_version == 0 && config.asset.stored_width == 0 &&
               config.asset.stored_height == 0 && config.asset.orientation == nullptr &&
               config.asset.frame_bytes == 0 && !config.loop;
    }
    return std::strcmp(config.asset.media_type,
                       "application/vnd.tbot.rgb565-indexed") == 0 &&
           config.template_version == 2 && config.asset.width == 0 && config.asset.height == 0 &&
           config.asset.container_version == 1 && config.asset.stored_width == 320 &&
           config.asset.stored_height == 480 && config.asset.orientation != nullptr &&
           std::strcmp(config.asset.orientation, "panelNativeClockwise") == 0 &&
           config.asset.frame_bytes == 307200;
}

LessonCinematicHilFault HilFaultForError(LessonCinematicError error) {
    switch (error) {
        case LessonCinematicError::kParserFailed:
            return LessonCinematicHilFault::kParser;
        case LessonCinematicError::kFileOpen:
        case LessonCinematicError::kFileRead:
        case LessonCinematicError::kSessionMismatch:
            return LessonCinematicHilFault::kIo;
        case LessonCinematicError::kDecodeTimeout:
            return LessonCinematicHilFault::kQueueTimeout;
        case LessonCinematicError::kPresentFailed:
            return LessonCinematicHilFault::kDma;
        default:
            return LessonCinematicHilFault::kNone;
    }
}

void RecordHilFault(LessonCinematicError error) {
    switch (HilFaultForError(error)) {
        case LessonCinematicHilFault::kParser:
            LessonCinematicHilTelemetryRecordParserFailure();
            break;
        case LessonCinematicHilFault::kIo:
            LessonCinematicHilTelemetryRecordIoError();
            break;
        case LessonCinematicHilFault::kDma:
            LessonCinematicHilTelemetryRecordDmaError();
            break;
        case LessonCinematicHilFault::kQueueTimeout:
            LessonCinematicHilTelemetryRecordQueueTimeout();
            break;
        default:
            break;
    }
}

}  // namespace

bool LessonFlattenedCinematicRendererCapabilityReady() {
    return g_capability_ready.load(std::memory_order_acquire);
}

void SetLessonFlattenedCinematicRendererCapabilityReady(bool ready) {
    g_capability_ready.store(ready, std::memory_order_release);
}

void SetActiveLessonFlattenedCinematicRenderer(LessonFlattenedCinematicRenderer* renderer) {
    std::lock_guard<std::mutex> lock(g_active_renderer_mutex);
    g_active_renderer.store(renderer, std::memory_order_release);
    SetLessonFlattenedCinematicRendererCapabilityReady(renderer != nullptr && renderer->initialized());
}

LessonFlattenedCinematicRenderer* ActiveLessonFlattenedCinematicRenderer() {
    return g_active_renderer.load(std::memory_order_acquire);
}

LessonCinematicResponse TickActiveLessonFlattenedCinematicRenderer(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(g_active_renderer_mutex);
    auto* renderer = g_active_renderer.load(std::memory_order_acquire);
    if (renderer == nullptr) {
        return {LessonCinematicResponseType::kFailure, false, 0, "",
                LessonCinematicError::kInvalidState};
    }
    return renderer->Tick(now_ms);
}

void SetLessonCinematicTimerRouteV4(bool route_v4) {
    g_timer_route_v4.store(route_v4, std::memory_order_release);
}

bool LessonCinematicTimerRoutesV4() {
    return g_timer_route_v4.load(std::memory_order_acquire);
}

LessonFlattenedCinematicRenderer::LessonFlattenedCinematicRenderer(
    LessonCinematicRendererOps ops) : ops_(ops) {
    flattened_ops_.legacy = ops;
}

LessonFlattenedCinematicRenderer::LessonFlattenedCinematicRenderer(
    LessonFlattenedCinematicRendererOps ops) : ops_(ops.legacy), flattened_ops_(ops) {}

LessonFlattenedCinematicRenderer::~LessonFlattenedCinematicRenderer() { Reset(); }

bool LessonFlattenedCinematicRenderer::initialized() const {
    return ops_.allocate != nullptr && ops_.free != nullptr && ops_.open != nullptr &&
           ops_.close != nullptr && ops_.decode != nullptr && ops_.present != nullptr;
}

bool LessonFlattenedCinematicRenderer::prepared() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kPrepared || state_ == State::kRunning || state_ == State::kPaused;
}

void LessonFlattenedCinematicRenderer::DiscardSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    LessonCinematicHilTelemetryEmitCueEnd(
        LessonCinematicHilCueEndReason::kDiscard, LessonCinematicHilFault::kNone,
        ops_.monotonic_ms != nullptr ? ops_.monotonic_ms(ops_.context) : 0);
    Reset();
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Failure(
    std::uint64_t sequence, LessonCinematicError error) const {
    RecordHilFault(error);
    LessonCinematicHilTelemetryEmitCueEnd(
        LessonCinematicHilCueEndReason::kFailure, HilFaultForError(error),
        ops_.monotonic_ms != nullptr ? ops_.monotonic_ms(ops_.context) : 0);
    return {LessonCinematicResponseType::kFailure, false, sequence, phase_id_, error};
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Applied(
    LessonCinematicResponseType type, std::uint64_t sequence) const {
    return {type, true, sequence, phase_id_, LessonCinematicError::kNone};
}

LessonCinematicError LessonFlattenedCinematicRenderer::OperationError(
    LessonCinematicError fallback) const {
    if (ops_.last_error == nullptr) return fallback;
    const auto error = ops_.last_error(ops_.context);
    return error == LessonCinematicError::kNone ? fallback : error;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Prepare(
    const LessonFlattenedCinematicPhaseConfig& config, std::uint64_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string fingerprint = PrepareFingerprint(config);
    if (config.command_sequence_id == last_sequence_ && last_response_.accepted) {
        if (last_command_ == "prepare" && last_fingerprint_ == fingerprint) return last_response_;
        return Failure(config.command_sequence_id, LessonCinematicError::kStaleCommand);
    }
    if (state_ == State::kPrepared || state_ == State::kRunning || state_ == State::kPaused) {
        LessonCinematicHilTelemetryEmitCueEnd(
            LessonCinematicHilCueEndReason::kReplacement, LessonCinematicHilFault::kNone,
            ops_.monotonic_ms != nullptr ? ops_.monotonic_ms(ops_.context) : 0);
    }
    LessonCinematicHilTelemetryBeginCue(
        config.phase_id, config.command_sequence_id,
        ops_.monotonic_ms != nullptr ? ops_.monotonic_ms(ops_.context) : 0);
    if (!initialized() || !ExactContract(config)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kUnsupportedContract);
    }
    if (!LocalPath(config.asset.sd_path)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kInvalidPath);
    }
    if (!ValidMetadata(config)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }

    const bool native = std::strcmp(config.asset.media_type,
        "application/vnd.tbot.rgb565-indexed") == 0;
    if (native && (flattened_ops_.begin_cinematic == nullptr ||
                   flattened_ops_.queue_native == nullptr ||
                   flattened_ops_.wait_native == nullptr ||
                   flattened_ops_.end_cinematic == nullptr)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kUnsupportedContract);
    }

    if (native_mode_ && !CleanupNative(100)) {
        state_ = State::kFailed;
        return Failure(config.command_sequence_id, LessonCinematicError::kDecodeTimeout);
    }

    constexpr std::size_t kFramebufferBytes =
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2;
    const std::size_t allocation_bytes = native ? 307200 : kFramebufferBytes;
    std::uint16_t* candidate_framebuffer =
        static_cast<std::uint16_t*>(ops_.allocate(ops_.context, allocation_bytes));
    if (candidate_framebuffer == nullptr) {
        return Failure(config.command_sequence_id, LessonCinematicError::kInsufficientPsram);
    }
    std::uint16_t* candidate_second = nullptr;
    if (native) {
        candidate_second = static_cast<std::uint16_t*>(ops_.allocate(ops_.context, allocation_bytes));
        if (candidate_second == nullptr) {
            ops_.free(ops_.context, candidate_framebuffer);
            return Failure(config.command_sequence_id, LessonCinematicError::kInsufficientPsram);
        }
    }
    void* candidate_stream = nullptr;
    LessonCinematicStreamMetadata candidate_metadata{};
    auto open = native && flattened_ops_.open_native != nullptr
        ? flattened_ops_.open_native : ops_.open;
    if (!open(ops_.context, config.asset.sd_path, &candidate_metadata, &candidate_stream)) {
        if (candidate_second != nullptr) ops_.free(ops_.context, candidate_second);
        ops_.free(ops_.context, candidate_framebuffer);
        return Failure(config.command_sequence_id, OperationError(LessonCinematicError::kFileOpen));
    }
    const std::uint16_t expected_width = native ? config.asset.stored_width : config.asset.width;
    const std::uint16_t expected_height = native ? config.asset.stored_height : config.asset.height;
    const bool metadata_ok = candidate_metadata.width == expected_width &&
        candidate_metadata.height == expected_height && candidate_metadata.fps == config.fps &&
        candidate_metadata.frame_count == config.frame_count &&
        candidate_metadata.duration_ms == config.duration_ms &&
        (!native || candidate_metadata.max_frame_bytes == config.asset.frame_bytes);
    if (!metadata_ok) {
        ops_.close(ops_.context, candidate_stream);
        if (candidate_second != nullptr) ops_.free(ops_.context, candidate_second);
        ops_.free(ops_.context, candidate_framebuffer);
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }
    if (native) {
        std::uint64_t parsed_bytes = 0;
        if (flattened_ops_.stream_bytes == nullptr ||
            !flattened_ops_.stream_bytes(ops_.context, candidate_stream, &parsed_bytes) ||
            parsed_bytes != config.asset.bytes) {
            ops_.close(ops_.context, candidate_stream);
            ops_.free(ops_.context, candidate_second);
            ops_.free(ops_.context, candidate_framebuffer);
            return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
        }
    }
    const auto frame_zero_error = native
        ? PrepareNative(config, candidate_stream, candidate_metadata,
                        candidate_framebuffer, candidate_second)
        : RenderFrameOn(candidate_stream, candidate_framebuffer, 0);
    if (frame_zero_error != LessonCinematicError::kNone) {
        if (native && native_in_flight_) {
            stream_ = candidate_stream;
            native_buffers_[0] = candidate_framebuffer;
            native_buffers_[1] = candidate_second;
            native_mode_ = true;
            CleanupNative(100);
            if (native_mode_) state_ = State::kFailed;
        } else {
            ops_.close(ops_.context, candidate_stream);
            if (candidate_second != nullptr) ops_.free(ops_.context, candidate_second);
            ops_.free(ops_.context, candidate_framebuffer);
        }
        return Failure(config.command_sequence_id, frame_zero_error);
    }
    if (!native_mode_) {
        CloseStream();
        ReleaseBuffer();
    }
    stream_ = candidate_stream;
    metadata_ = candidate_metadata;
    native_mode_ = native;
    if (native) {
        native_buffers_[0] = candidate_framebuffer;
        native_buffers_[1] = candidate_second;
        framebuffer_ = nullptr;
        loop_ = config.loop;
    } else {
        framebuffer_ = candidate_framebuffer;
        loop_ = false;
    }
    phase_id_ = config.phase_id;
    state_ = State::kPrepared;
    displayed_frame_ = 0;
    last_sequence_ = config.command_sequence_id;
    last_response_ = Applied(LessonCinematicResponseType::kFrameZeroReady, last_sequence_);
    last_command_ = "prepare";
    last_fingerprint_ = fingerprint;
    return last_response_;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::ValidateControl(
    std::uint64_t sequence, const char* phase_id, const char* command) const {
    if (sequence == last_sequence_ && last_response_.accepted) {
        if (last_command_ == command && last_fingerprint_ == ControlFingerprint(command, phase_id)) {
            return last_response_;
        }
        return Failure(sequence, LessonCinematicError::kStaleCommand);
    }
    if (sequence < last_sequence_ || phase_id == nullptr || phase_id_ != phase_id) {
        return Failure(sequence, LessonCinematicError::kStaleCommand);
    }
    return Applied(LessonCinematicResponseType::kCommandApplied, sequence);
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Start(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "start");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kPrepared) return Failure(sequence, LessonCinematicError::kInvalidState);
    if (native_mode_ && !native_owned_) {
        const auto error = StartNativePlayback();
        if (error != LessonCinematicError::kNone) {
            state_ = State::kFailed;
            return Failure(sequence, error);
        }
    }
    if (displayed_frame_ != 0) {
        const auto error = native_mode_ ? LessonCinematicError::kNone : RenderFrame(0);
        if (error != LessonCinematicError::kNone) {
            CloseStream();
            ReleaseBuffer();
            state_ = State::kFailed;
            return Failure(sequence, error);
        }
        displayed_frame_ = 0;
    }
    state_ = State::kRunning;
    clock_origin_ms_ = now_ms;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kPhaseReady, sequence);
    last_command_ = "start";
    last_fingerprint_ = ControlFingerprint("start", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Pause(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "pause");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kRunning) return Failure(sequence, LessonCinematicError::kInvalidState);
    state_ = State::kPaused;
    paused_at_ms_ = now_ms;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "pause";
    last_fingerprint_ = ControlFingerprint("pause", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Resume(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "resume");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kPaused) return Failure(sequence, LessonCinematicError::kInvalidState);
    clock_origin_ms_ += now_ms - paused_at_ms_;
    state_ = State::kRunning;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "resume";
    last_fingerprint_ = ControlFingerprint("resume", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Stop(
    std::uint64_t sequence, const char* phase_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "stop");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (native_mode_ && !CleanupNative(100)) {
        state_ = State::kFailed;
        return Failure(sequence, LessonCinematicError::kDecodeTimeout);
    }
    CloseStream();
    ReleaseBuffer();
    LessonCinematicHilTelemetryEmitCueEnd(
        LessonCinematicHilCueEndReason::kStop, LessonCinematicHilFault::kNone,
        ops_.monotonic_ms != nullptr ? ops_.monotonic_ms(ops_.context) : 0);
    state_ = State::kIdle;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "stop";
    last_fingerprint_ = ControlFingerprint("stop", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Cancel(
    std::uint64_t sequence, const char* phase_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "cancel");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (native_mode_ && !CleanupNative(100)) {
        state_ = State::kFailed;
        return Failure(sequence, LessonCinematicError::kDecodeTimeout);
    }
    CloseStream();
    ReleaseBuffer();
    LessonCinematicHilTelemetryEmitCueEnd(
        LessonCinematicHilCueEndReason::kCancel, LessonCinematicHilFault::kNone,
        ops_.monotonic_ms != nullptr ? ops_.monotonic_ms(ops_.context) : 0);
    state_ = State::kIdle;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "cancel";
    last_fingerprint_ = ControlFingerprint("cancel", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Tick(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kPaused) {
        return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
    }
    if (state_ != State::kRunning) return Failure(last_sequence_, LessonCinematicError::kInvalidState);
    const std::uint64_t elapsed = now_ms >= clock_origin_ms_ ? now_ms - clock_origin_ms_ : 0;
    const std::uint64_t absolute_frame = elapsed * metadata_.fps / 1000;
    const std::uint64_t frame = loop_ ? absolute_frame % metadata_.frame_count : absolute_frame;
    if (native_mode_ && absolute_frame > displayed_clock_frame_ + 1) {
        if (last_tick_dropped_ ||
            (has_timing_drop_ && successful_frames_since_drop_ < 300)) {
#ifdef ESP_PLATFORM
            ESP_LOGE("LessonCinematic",
                     "TRGB timing degradation threshold exceeded after %u recovered frames",
                     static_cast<unsigned>(successful_frames_since_drop_));
#endif
            CleanupNative(100);
            state_ = State::kFailed;
            return Failure(last_sequence_, LessonCinematicError::kDecodeTimeout);
        }
        ++native_drops_;
        has_timing_drop_ = true;
        last_tick_dropped_ = true;
        successful_frames_since_drop_ = 0;
        const std::uint64_t dropped_periods = absolute_frame - displayed_clock_frame_;
#ifdef ESP_PLATFORM
        ESP_LOGW("LessonCinematic",
                 "TRGB timing degraded; retaining frame and rebasing %" PRIu64 " periods",
                 dropped_periods);
#endif
        clock_origin_ms_ += dropped_periods * 1000 / metadata_.fps;
        LessonCinematicHilTelemetryRecordLateTick(
            dropped_periods > UINT32_MAX ? UINT32_MAX
                                         : static_cast<std::uint32_t>(dropped_periods));
        return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
    }
    if (!loop_ && frame >= metadata_.frame_count) {
        if (native_mode_) {
            if (!FinishNativeTransfer(100)) {
                state_ = State::kFailed;
                return Failure(last_sequence_, LessonCinematicError::kDecodeTimeout);
            }
            if (native_owned_) {
                flattened_ops_.end_cinematic(ops_.context);
                native_owned_ = false;
            }
        }
        state_ = State::kPrepared;
        displayed_frame_ = metadata_.frame_count - 1;
        LessonCinematicHilTelemetryEmitCueEnd(
            LessonCinematicHilCueEndReason::kNatural, LessonCinematicHilFault::kNone,
            ops_.monotonic_ms != nullptr ? ops_.monotonic_ms(ops_.context) : now_ms);
        return Applied(LessonCinematicResponseType::kPhaseComplete, last_sequence_);
    }
    if (absolute_frame == displayed_clock_frame_) {
        return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
    }
    const auto error = native_mode_ ? AdvanceNative(static_cast<std::size_t>(frame))
                                    : RenderFrame(static_cast<std::size_t>(frame));
    if (error != LessonCinematicError::kNone) {
        if (native_mode_) {
            if (error != LessonCinematicError::kDecodeTimeout) CleanupNative(100);
        } else {
            CloseStream();
            ReleaseBuffer();
        }
        state_ = State::kFailed;
        return Failure(last_sequence_, error);
    }
    displayed_frame_ = static_cast<std::size_t>(frame);
    displayed_clock_frame_ = absolute_frame;
    if (native_mode_) {
        last_tick_dropped_ = false;
        if (has_timing_drop_ && successful_frames_since_drop_ < UINT32_MAX) {
            ++successful_frames_since_drop_;
        }
    }
    return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
}

LessonCinematicError LessonFlattenedCinematicRenderer::RenderFrame(std::size_t frame_index) {
    return RenderFrameOn(stream_, framebuffer_, frame_index);
}

LessonCinematicError LessonFlattenedCinematicRenderer::PrepareNative(
    const LessonFlattenedCinematicPhaseConfig&, void* stream,
    const LessonCinematicStreamMetadata& metadata, std::uint16_t* first, std::uint16_t* second) {
    LessonCinematicError error = ReadNativeFrame(stream, 0, first, 100);
    if (error != LessonCinematicError::kNone) return error;
    if (!flattened_ops_.begin_cinematic(ops_.context)) {
        return LessonCinematicError::kPresentFailed;
    }
    if (!flattened_ops_.queue_native(ops_.context, first, 320, 480)) {
        flattened_ops_.end_cinematic(ops_.context);
        LessonCinematicHilTelemetryRecordQueueError();
        return LessonCinematicError::kPresentFailed;
    }
    native_owned_ = true;
    native_in_flight_ = true;
    native_queued_buffer_ = 0;
    native_ready_buffer_ = 1;
    native_ready_frame_ = metadata.frame_count > 1 ? 1 : 0;
    native_next_read_frame_ = metadata.frame_count > 1 ? 2 : 1;
    if (metadata.frame_count > 1) {
        error = ReadNativeFrame(stream, 1, second, 100);
        if (error != LessonCinematicError::kNone) {
            if (FinishNativeTransfer(100)) {
                flattened_ops_.end_cinematic(ops_.context);
                native_owned_ = false;
            }
            return error;
        }
    }
    return LessonCinematicError::kNone;
}

LessonCinematicError LessonFlattenedCinematicRenderer::ReadNativeFrame(
    void* stream, std::size_t frame, std::uint16_t* destination,
    std::uint32_t deadline_ms) {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t stride = 0;
    const std::uint64_t started = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : 0;
    if (!ops_.decode(ops_.context, stream, frame,
                     reinterpret_cast<std::uint8_t*>(destination), 307200,
                     &width, &height, &stride)) {
        return OperationError(LessonCinematicError::kFileRead);
    }
    if (ops_.monotonic_ms != nullptr) {
        const std::uint64_t finished = ops_.monotonic_ms(ops_.context);
        if (finished >= started) {
            const std::uint64_t elapsed = finished - started;
            LessonCinematicHilTelemetryRecordRead(elapsed);
            if (elapsed > deadline_ms) {
                return LessonCinematicError::kDecodeTimeout;
            }
        }
    }
    return width == 320 && height == 480 && stride == 640
        ? LessonCinematicError::kNone : LessonCinematicError::kMetadataMismatch;
}

bool LessonFlattenedCinematicRenderer::FinishNativeTransfer(std::uint32_t timeout_ms) {
    if (!native_in_flight_) return true;
    if (!flattened_ops_.wait_native(ops_.context, timeout_ms)) {
        LessonCinematicHilTelemetryRecordQueueTimeout();
        return false;
    }
    native_in_flight_ = false;
    return true;
}

LessonCinematicError LessonFlattenedCinematicRenderer::AdvanceNative(std::size_t target_frame) {
    if (!FinishNativeTransfer(100)) return LessonCinematicError::kDecodeTimeout;

    while (native_ready_frame_ != target_frame) {
        std::size_t next = native_next_read_frame_;
        if (next >= metadata_.frame_count) {
            if (!loop_) return LessonCinematicError::kFileRead;
            next = 0;
        }
        const std::size_t destination = native_queued_buffer_;
        const LessonCinematicError error =
            ReadNativeFrame(stream_, next, native_buffers_[destination], 100);
        if (error != LessonCinematicError::kNone) return error;
        native_ready_buffer_ = destination;
        native_ready_frame_ = next;
        native_queued_buffer_ = 1 - destination;
        native_next_read_frame_ = next + 1;
    }

    if (!flattened_ops_.queue_native(ops_.context, native_buffers_[native_ready_buffer_], 320, 480)) {
        LessonCinematicHilTelemetryRecordQueueError();
        return LessonCinematicError::kPresentFailed;
    }
    native_in_flight_ = true;
    native_queued_buffer_ = native_ready_buffer_;

    std::size_t next = native_next_read_frame_;
    if (next >= metadata_.frame_count && loop_) next = 0;
    if (next < metadata_.frame_count) {
        const std::size_t destination = 1 - native_queued_buffer_;
        const LessonCinematicError error =
            ReadNativeFrame(stream_, next, native_buffers_[destination], 100);
        if (error != LessonCinematicError::kNone) return error;
        native_ready_buffer_ = destination;
        native_ready_frame_ = next;
        native_next_read_frame_ = next + 1;
    }
    return LessonCinematicError::kNone;
}

LessonCinematicError LessonFlattenedCinematicRenderer::StartNativePlayback() {
    LessonCinematicError error = ReadNativeFrame(stream_, 0, native_buffers_[0], 100);
    if (error != LessonCinematicError::kNone) return error;
    const bool began = flattened_ops_.begin_cinematic(ops_.context);
    if (!began || !flattened_ops_.queue_native(
            ops_.context, native_buffers_[0], 320, 480)) {
        if (began) flattened_ops_.end_cinematic(ops_.context);
        LessonCinematicHilTelemetryRecordQueueError();
        return LessonCinematicError::kPresentFailed;
    }
    native_owned_ = true;
    native_in_flight_ = true;
    native_queued_buffer_ = 0;
    native_ready_buffer_ = 1;
    native_ready_frame_ = metadata_.frame_count > 1 ? 1 : 0;
    native_next_read_frame_ = metadata_.frame_count > 1 ? 2 : 1;
    if (metadata_.frame_count > 1) {
        error = ReadNativeFrame(stream_, 1, native_buffers_[1], 100);
        if (error != LessonCinematicError::kNone) return error;
    }
    displayed_frame_ = 0;
    displayed_clock_frame_ = 0;
    return LessonCinematicError::kNone;
}

bool LessonFlattenedCinematicRenderer::CleanupNative(std::uint32_t timeout_ms) {
    if (!native_mode_ && native_buffers_[0] == nullptr && native_buffers_[1] == nullptr) {
        return true;
    }
    if (!FinishNativeTransfer(timeout_ms)) return false;
    if (native_owned_) {
        flattened_ops_.end_cinematic(ops_.context);
        native_owned_ = false;
    }
#ifdef ESP_PLATFORM
    ESP_LOGI("LessonCinematic", "aggregate dropped_frames=%u",
             static_cast<unsigned>(native_drops_));
#endif
    CloseStream();
    for (auto*& buffer : native_buffers_) {
        if (buffer != nullptr) ops_.free(ops_.context, buffer);
        buffer = nullptr;
    }
    native_mode_ = false;
    native_drops_ = 0;
    successful_frames_since_drop_ = 0;
    has_timing_drop_ = false;
    last_tick_dropped_ = false;
    return true;
}

LessonCinematicError LessonFlattenedCinematicRenderer::RenderFrameOn(
    void* stream, std::uint16_t* framebuffer, std::size_t frame_index) {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t stride = 0;
    constexpr std::size_t kFramebufferBytes =
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2;
    constexpr std::uint64_t kPlaybackReadDecodeDeadlineMs = 100;
    // Frame zero includes a cold SD read and cold ESP32-S3 ROM JPEG decode.
    constexpr std::uint64_t kFrameZeroReadDecodeDeadlineMs = 500;
    const std::uint64_t read_decode_deadline_ms = frame_index == 0
        ? kFrameZeroReadDecodeDeadlineMs : kPlaybackReadDecodeDeadlineMs;
    const std::uint64_t started = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : 0;
    if (!ops_.decode(ops_.context, stream, frame_index,
                     reinterpret_cast<std::uint8_t*>(framebuffer), kFramebufferBytes,
                     &width, &height, &stride)) {
        return OperationError(LessonCinematicError::kDecodeFailed);
    }
    if (ops_.monotonic_ms != nullptr) {
        const std::uint64_t finished = ops_.monotonic_ms(ops_.context);
        if (finished >= started) {
            LessonCinematicHilTelemetryRecordRead(finished - started);
        }
            if (finished >= started && finished - started > read_decode_deadline_ms) {
            return LessonCinematicError::kDecodeTimeout;
        }
    }
    if (width != kLessonCinematicWidth || height != kLessonCinematicHeight ||
        stride != static_cast<std::size_t>(width) * 2) {
        return LessonCinematicError::kDecodeFailed;
    }
    return ops_.present(ops_.context, framebuffer, width, height, frame_index)
        ? LessonCinematicError::kNone : LessonCinematicError::kPresentFailed;
}

void LessonFlattenedCinematicRenderer::CloseStream() {
    if (stream_ != nullptr) {
        ops_.close(ops_.context, stream_);
        stream_ = nullptr;
    }
}

void LessonFlattenedCinematicRenderer::ReleaseBuffer() {
    if (framebuffer_ != nullptr) ops_.free(ops_.context, framebuffer_);
    framebuffer_ = nullptr;
}

void LessonFlattenedCinematicRenderer::Reset() {
    if (native_mode_ || native_buffers_[0] != nullptr || native_buffers_[1] != nullptr) {
        if (!CleanupNative(100)) {
            state_ = State::kFailed;
            return;
        }
    }
    CloseStream();
    ReleaseBuffer();
    state_ = State::kIdle;
    displayed_frame_ = 0;
    displayed_clock_frame_ = 0;
    clock_origin_ms_ = 0;
    paused_at_ms_ = 0;
}

#ifdef ESP_PLATFORM
namespace {

struct ProductionFlattenedRendererContext {
    ::LcdDisplay* display = nullptr;
    std::string assignment_id;
    std::string session_id;
    std::uint64_t generation = 0;
    std::array<LessonMjpegMp4File, 2> files;
    std::array<LessonRgb565File, 2> rgb_files;
    struct Handle {
        LessonMjpegMp4File* mp4 = nullptr;
        LessonRgb565File* rgb = nullptr;
        bool open() const { return mp4 != nullptr || rgb != nullptr; }
    };
    std::array<Handle, 2> handles;
    jpeg_reusable_decoder_t decoder{};
    std::uint8_t* jpeg_input = nullptr;
    std::size_t jpeg_input_capacity = kLessonMjpegMp4MaxSampleBytes;
    LessonCinematicError last_error = LessonCinematicError::kNone;
    std::uint64_t aggregate_read_ms = 0;
    std::uint64_t aggregate_queue_ms = 0;
    std::uint32_t frames_read = 0;
    std::uint32_t frames_queued = 0;
    bool force_rgb_open = false;
};

std::unique_ptr<ProductionFlattenedRendererContext> g_production_context;
std::unique_ptr<LessonFlattenedCinematicRenderer> g_production_renderer;

void ReleaseProductionDecodeWorkspaceIfIdle(ProductionFlattenedRendererContext* context) {
    if (context == nullptr) return;
    for (const auto& file : context->files) {
        if (file.is_open()) return;
    }
    jpeg_reusable_decoder_destroy(&context->decoder);
    if (context->jpeg_input != nullptr) heap_caps_free(context->jpeg_input);
    context->jpeg_input = nullptr;
}

void* ProductionAllocate(void*, std::size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void ProductionFree(void*, void* pointer) { heap_caps_free(pointer); }

bool ProductionOpen(void* raw, const char* path, LessonCinematicStreamMetadata* metadata,
                    void** handle) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    if (context == nullptr || context->generation == 0 || metadata == nullptr || handle == nullptr) {
        return false;
    }
    context->last_error = LessonCinematicError::kNone;
    const std::string_view path_view(path != nullptr ? path : "");
    const bool trgb = context->force_rgb_open ||
        (path_view.size() >= 5 && path_view.substr(path_view.size() - 5) == ".trgb");
    if (!trgb && context->jpeg_input == nullptr) {
        context->jpeg_input = static_cast<std::uint8_t*>(heap_caps_malloc(
            context->jpeg_input_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (context->jpeg_input == nullptr ||
            jpeg_reusable_decoder_prepare_workspace(
                &context->decoder, kLessonCinematicWidth, kLessonCinematicHeight,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != ESP_OK) {
            if (context->jpeg_input != nullptr) heap_caps_free(context->jpeg_input);
            context->jpeg_input = nullptr;
            jpeg_reusable_decoder_destroy(&context->decoder);
            context->last_error = LessonCinematicError::kInsufficientPsram;
            return false;
        }
    }
    ProductionFlattenedRendererContext::Handle* production_handle = nullptr;
    std::size_t slot = 0;
    for (; slot < context->handles.size(); ++slot) {
        if (!context->handles[slot].open()) {
            production_handle = &context->handles[slot];
            break;
        }
    }
    if (production_handle == nullptr) {
        context->last_error = LessonCinematicError::kFileOpen;
        return false;
    }
    if (trgb) {
        LessonRgb565File* file = &context->rgb_files[slot];
        const LessonRgb565Status status = file->OpenUnderLessonSession(
            path, context->assignment_id, context->session_id, context->generation);
        if (status != LessonRgb565Status::kOk) {
            context->last_error = status == LessonRgb565Status::kLeaseUnavailable
                ? LessonCinematicError::kSessionMismatch
                : status == LessonRgb565Status::kIoError || status == LessonRgb565Status::kUnsafePath
                    ? LessonCinematicError::kFileOpen : LessonCinematicError::kParserFailed;
            return false;
        }
        const LessonRgb565Metadata parsed = file->metadata();
        *metadata = {parsed.stored_width, parsed.stored_height, parsed.fps,
                     parsed.frame_count, parsed.duration_ms, parsed.frame_bytes};
        production_handle->rgb = file;
        *handle = production_handle;
        return true;
    }
    LessonMjpegMp4File* file = &context->files[slot];
    const LessonMjpegMp4Status open_status = file->OpenUnderLessonSession(
        path, context->assignment_id, context->session_id, context->generation);
    if (open_status != LessonMjpegMp4Status::kOk) {
        context->last_error = open_status == LessonMjpegMp4Status::kLeaseUnavailable
            ? LessonCinematicError::kSessionMismatch
            : open_status == LessonMjpegMp4Status::kIoError
                ? LessonCinematicError::kFileOpen : LessonCinematicError::kParserFailed;
        ReleaseProductionDecodeWorkspaceIfIdle(context);
        return false;
    }
    const LessonMjpegMp4Metadata parsed = file->metadata();
    if (parsed.timescale == 0 || parsed.fps_milli % 1000 != 0 ||
        parsed.duration_ticks > UINT64_MAX / 1000 || parsed.frame_count > UINT32_MAX) {
        file->Close();
        context->last_error = LessonCinematicError::kMetadataMismatch;
        ReleaseProductionDecodeWorkspaceIfIdle(context);
        return false;
    }
    *metadata = {parsed.width, parsed.height,
                 static_cast<std::uint16_t>(parsed.fps_milli / 1000),
                 static_cast<std::uint32_t>(parsed.frame_count),
                 static_cast<std::uint32_t>(parsed.duration_ticks * 1000 / parsed.timescale),
                 kLessonMjpegMp4MaxSampleBytes};
    production_handle->mp4 = file;
    *handle = production_handle;
    return true;
}

bool ProductionOpenNative(void* raw, const char* path,
                          LessonCinematicStreamMetadata* metadata, void** handle) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    if (context == nullptr) return false;
    context->force_rgb_open = true;
    const bool opened = ProductionOpen(raw, path, metadata, handle);
    context->force_rgb_open = false;
    return opened;
}

bool ProductionStreamBytes(void*, void* handle, std::uint64_t* bytes) {
    if (handle == nullptr || bytes == nullptr) return false;
    auto* production_handle =
        static_cast<ProductionFlattenedRendererContext::Handle*>(handle);
    if (production_handle->rgb == nullptr) return false;
    *bytes = production_handle->rgb->metadata().file_bytes;
    return true;
}

void ProductionClose(void* raw, void* handle) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    if (context == nullptr) return;
    if (handle != nullptr) {
        auto* production_handle =
            static_cast<ProductionFlattenedRendererContext::Handle*>(handle);
        if (production_handle->mp4 != nullptr) production_handle->mp4->Close();
        if (production_handle->rgb != nullptr) production_handle->rgb->Close();
        *production_handle = {};
    }
    if (context->frames_read != 0 || context->frames_queued != 0) {
        ESP_LOGI("LessonCinematic",
                 "aggregate frames_read=%u read_crc_ms=%" PRIu64
                 " frames_queued=%u queue_ms=%" PRIu64 " psram_free=%u",
                 static_cast<unsigned>(context->frames_read), context->aggregate_read_ms,
                 static_cast<unsigned>(context->frames_queued), context->aggregate_queue_ms,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        context->aggregate_read_ms = 0;
        context->aggregate_queue_ms = 0;
        context->frames_read = 0;
        context->frames_queued = 0;
    }
    ReleaseProductionDecodeWorkspaceIfIdle(context);
}

bool ProductionDecode(void* raw, void* handle, std::size_t index,
                      std::uint8_t* destination, std::size_t capacity,
                      std::uint16_t* width, std::uint16_t* height, std::size_t* stride) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    if (context != nullptr) context->last_error = LessonCinematicError::kNone;
    const std::uint64_t started_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    auto* production_handle =
        static_cast<ProductionFlattenedRendererContext::Handle*>(handle);
    if (context != nullptr && production_handle != nullptr && production_handle->rgb != nullptr) {
        const LessonRgb565Status status = production_handle->rgb->ReadFrame(
            static_cast<std::uint32_t>(index), destination, capacity);
        const std::uint64_t finished_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        context->aggregate_read_ms += finished_ms - started_ms;
        ++context->frames_read;
        if (status != LessonRgb565Status::kOk) {
            context->last_error = status == LessonRgb565Status::kLeaseUnavailable
                ? LessonCinematicError::kSessionMismatch : LessonCinematicError::kFileRead;
            return false;
        }
        *width = 320;
        *height = 480;
        *stride = 640;
        return true;
    }
    std::size_t jpeg_size = 0;
    if (context == nullptr || handle == nullptr || context->jpeg_input == nullptr ||
        production_handle->mp4->ReadFrame(
            index, context->jpeg_input, context->jpeg_input_capacity, &jpeg_size) !=
            LessonMjpegMp4Status::kOk) {
        if (context != nullptr) context->last_error = LessonCinematicError::kFileRead;
        return false;
    }
    const std::uint64_t read_done_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    std::size_t decoded_size = 0, decoded_width = 0, decoded_height = 0;
    if (jpeg_reusable_decoder_decode_into(
            &context->decoder, context->jpeg_input, jpeg_size, destination, capacity,
            &decoded_size, &decoded_width, &decoded_height, stride) != ESP_OK ||
        decoded_width > UINT16_MAX || decoded_height > UINT16_MAX) {
        context->last_error = LessonCinematicError::kDecodeFailed;
        return false;
    }
    context->aggregate_read_ms += read_done_ms - started_ms;
    ++context->frames_read;
    *width = static_cast<std::uint16_t>(decoded_width);
    *height = static_cast<std::uint16_t>(decoded_height);
    return decoded_size == decoded_height * *stride;
}

bool ProductionPresent(void* raw, const std::uint16_t* pixels, std::uint16_t width,
                       std::uint16_t height, std::size_t) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    return context != nullptr && context->display != nullptr &&
           context->display->PresentLessonFramebuffer(pixels, width, height);
}

bool ProductionBeginNative(void* raw) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    return context != nullptr && context->display != nullptr &&
           context->display->BeginLessonCinematic();
}

bool ProductionQueueNative(void* raw, const std::uint16_t* pixels,
                           std::uint16_t width, std::uint16_t height) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    const std::uint64_t started = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    const bool queued = context != nullptr && context->display != nullptr &&
        context->display->QueueLessonCinematicFrame(pixels, width, height);
    if (context != nullptr) {
        context->aggregate_queue_ms +=
            static_cast<std::uint64_t>(esp_timer_get_time() / 1000) - started;
        if (queued) ++context->frames_queued;
    }
    return queued;
}

bool ProductionWaitNative(void* raw, std::uint32_t timeout_ms) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    return context != nullptr && context->display != nullptr &&
           context->display->WaitLessonCinematicFrame(timeout_ms);
}

void ProductionEndNative(void* raw) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    if (context != nullptr && context->display != nullptr) {
        context->display->EndLessonCinematic();
    }
}

LessonCinematicError ProductionLastError(void* raw) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    return context != nullptr ? context->last_error : LessonCinematicError::kNone;
}

std::uint64_t ProductionMonotonicMs(void*) {
    return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace
#endif

bool InitializeProductionLessonFlattenedCinematicRenderer(::LcdDisplay* display) {
#ifdef ESP_PLATFORM
    constexpr std::size_t kRequiredPsram =
        2 * static_cast<std::size_t>(kLessonRgb565FrameBytes) +
        kLessonMjpegMp4MaxSampleBytes + 128 * 1024;
    if (display == nullptr || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kRequiredPsram) {
        return false;
    }
    void* framebuffer_probe = heap_caps_malloc(
        kLessonRgb565FrameBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* second_framebuffer_probe = heap_caps_malloc(
        kLessonRgb565FrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* jpeg_probe = heap_caps_malloc(kLessonMjpegMp4MaxSampleBytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpeg_reusable_decoder_t decoder_probe{};
    const bool decoder_ready = jpeg_reusable_decoder_prepare_workspace(
        &decoder_probe, kLessonCinematicWidth, kLessonCinematicHeight,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == ESP_OK;
    const bool ready = framebuffer_probe != nullptr && second_framebuffer_probe != nullptr &&
        jpeg_probe != nullptr && decoder_ready;
    if (framebuffer_probe != nullptr) heap_caps_free(framebuffer_probe);
    if (second_framebuffer_probe != nullptr) heap_caps_free(second_framebuffer_probe);
    if (jpeg_probe != nullptr) heap_caps_free(jpeg_probe);
    jpeg_reusable_decoder_destroy(&decoder_probe);
    if (!ready) return false;
    g_production_context = std::make_unique<ProductionFlattenedRendererContext>();
    g_production_context->display = display;
    g_production_renderer = std::make_unique<LessonFlattenedCinematicRenderer>(
        LessonFlattenedCinematicRendererOps{
            LessonCinematicRendererOps{g_production_context.get(), ProductionAllocate,
                ProductionFree, ProductionOpen, ProductionClose, ProductionDecode,
                ProductionPresent, ProductionLastError, ProductionMonotonicMs},
            ProductionBeginNative, ProductionQueueNative, ProductionWaitNative,
            ProductionEndNative, ProductionOpenNative, ProductionStreamBytes});
    SetActiveLessonFlattenedCinematicRenderer(g_production_renderer.get());
    return LessonFlattenedCinematicRendererCapabilityReady();
#else
    (void)display;
    return false;
#endif
}

void ConfigureProductionLessonFlattenedCinematicSession(const std::string& assignment_id,
                                                        const std::string& session_id,
                                                        std::uint64_t generation) {
#ifdef ESP_PLATFORM
    if (g_production_context != nullptr) {
        g_production_context->assignment_id = assignment_id;
        g_production_context->session_id = session_id;
        g_production_context->generation = generation;
    }
#else
    (void)assignment_id;
    (void)session_id;
    (void)generation;
#endif
}

void ShutdownProductionLessonFlattenedCinematicRenderer() {
#ifdef ESP_PLATFORM
    SetActiveLessonFlattenedCinematicRenderer(nullptr);
    g_production_renderer.reset();
    g_production_context.reset();
#endif
    SetLessonCinematicTimerRouteV4(false);
    SetLessonFlattenedCinematicRendererCapabilityReady(false);
}

}  // namespace tbot
