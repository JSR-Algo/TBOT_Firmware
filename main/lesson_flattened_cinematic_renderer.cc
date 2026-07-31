#include "lesson_flattened_cinematic_renderer.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <string_view>

#ifdef ESP_PLATFORM
#include "display/lcd_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "lesson_mjpeg_mp4.h"
#include <esp_heap_caps.h>
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
    value += std::to_string(config.asset.height);
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
           config.template_version == 1;
}

bool ValidMetadata(const LessonFlattenedCinematicPhaseConfig& config) {
    return config.phase_id != nullptr && config.phase_id[0] != '\0' &&
           config.asset.phase_id != nullptr &&
           std::strcmp(config.asset.phase_id, config.phase_id) == 0 &&
           LowerHexSha256(config.asset.derivative_id) && LowerHexSha256(config.asset.sha256) &&
           config.asset.bytes > 0 &&
           config.asset.media_type != nullptr &&
           std::strcmp(config.asset.media_type, "video/mp4") == 0 &&
           config.asset.width == kLessonCinematicWidth &&
           config.asset.height == kLessonCinematicHeight && config.fps == 10 &&
           config.frame_count > 0 && config.duration_ms > 0 &&
           static_cast<std::uint64_t>(config.duration_ms) * config.fps ==
               static_cast<std::uint64_t>(config.frame_count) * 1000;
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
    LessonCinematicRendererOps ops) : ops_(ops) {}

LessonFlattenedCinematicRenderer::~LessonFlattenedCinematicRenderer() { Reset(); }

bool LessonFlattenedCinematicRenderer::initialized() const {
    return ops_.allocate != nullptr && ops_.free != nullptr && ops_.open != nullptr &&
           ops_.close != nullptr && ops_.decode != nullptr && ops_.present != nullptr;
}

bool LessonFlattenedCinematicRenderer::prepared() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kPrepared || state_ == State::kRunning || state_ == State::kPaused;
}

LessonCinematicResponse LessonFlattenedCinematicRenderer::Failure(
    std::uint64_t sequence, LessonCinematicError error) const {
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
    if (!initialized() || !ExactContract(config)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kUnsupportedContract);
    }
    if (!LocalPath(config.asset.sd_path)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kInvalidPath);
    }
    if (!ValidMetadata(config)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }

    Reset();
    phase_id_ = config.phase_id;
    constexpr std::size_t kFramebufferBytes =
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2;
    framebuffer_ = static_cast<std::uint16_t*>(ops_.allocate(ops_.context, kFramebufferBytes));
    if (framebuffer_ == nullptr) {
        return Failure(config.command_sequence_id, LessonCinematicError::kInsufficientPsram);
    }
    if (!ops_.open(ops_.context, config.asset.sd_path, &metadata_, &stream_)) {
        ReleaseBuffer();
        return Failure(config.command_sequence_id, OperationError(LessonCinematicError::kFileOpen));
    }
    const bool metadata_ok = metadata_.width == config.asset.width &&
        metadata_.height == config.asset.height && metadata_.fps == config.fps &&
        metadata_.frame_count == config.frame_count && metadata_.duration_ms == config.duration_ms;
    if (!metadata_ok) {
        CloseStream();
        ReleaseBuffer();
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }
    const auto frame_zero_error = RenderFrame(0);
    if (frame_zero_error != LessonCinematicError::kNone) {
        CloseStream();
        ReleaseBuffer();
        state_ = State::kFailed;
        return Failure(config.command_sequence_id, frame_zero_error);
    }
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
    if (displayed_frame_ != 0) {
        const auto error = RenderFrame(0);
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
    CloseStream();
    ReleaseBuffer();
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
    CloseStream();
    ReleaseBuffer();
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
    const std::uint64_t frame = elapsed * metadata_.fps / 1000;
    if (frame >= metadata_.frame_count) {
        state_ = State::kPrepared;
        displayed_frame_ = metadata_.frame_count - 1;
        return Applied(LessonCinematicResponseType::kPhaseComplete, last_sequence_);
    }
    if (frame == displayed_frame_) {
        return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
    }
    const auto error = RenderFrame(static_cast<std::size_t>(frame));
    if (error != LessonCinematicError::kNone) {
        CloseStream();
        ReleaseBuffer();
        state_ = State::kFailed;
        return Failure(last_sequence_, error);
    }
    displayed_frame_ = static_cast<std::size_t>(frame);
    return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
}

LessonCinematicError LessonFlattenedCinematicRenderer::RenderFrame(std::size_t frame_index) {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t stride = 0;
    constexpr std::size_t kFramebufferBytes =
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2;
    constexpr std::uint64_t kReadDecodeDeadlineMs = 100;
    const std::uint64_t started = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : 0;
    if (!ops_.decode(ops_.context, stream_, frame_index,
                     reinterpret_cast<std::uint8_t*>(framebuffer_), kFramebufferBytes,
                     &width, &height, &stride)) {
        return OperationError(LessonCinematicError::kDecodeFailed);
    }
    if (ops_.monotonic_ms != nullptr) {
        const std::uint64_t finished = ops_.monotonic_ms(ops_.context);
        if (finished >= started && finished - started > kReadDecodeDeadlineMs) {
            return LessonCinematicError::kDecodeTimeout;
        }
    }
    if (width != kLessonCinematicWidth || height != kLessonCinematicHeight ||
        stride != static_cast<std::size_t>(width) * 2) {
        return LessonCinematicError::kDecodeFailed;
    }
    return ops_.present(ops_.context, framebuffer_, width, height, frame_index)
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
    CloseStream();
    ReleaseBuffer();
    state_ = State::kIdle;
    displayed_frame_ = 0;
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
    LessonMjpegMp4File file;
    jpeg_reusable_decoder_t decoder{};
    std::uint8_t* jpeg_input = nullptr;
    std::size_t jpeg_input_capacity = kLessonMjpegMp4MaxSampleBytes;
    LessonCinematicError last_error = LessonCinematicError::kNone;
};

std::unique_ptr<ProductionFlattenedRendererContext> g_production_context;
std::unique_ptr<LessonFlattenedCinematicRenderer> g_production_renderer;

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
    context->jpeg_input = static_cast<std::uint8_t*>(heap_caps_malloc(
        context->jpeg_input_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (context->jpeg_input == nullptr ||
        jpeg_reusable_decoder_prepare_workspace(
            &context->decoder, kLessonCinematicWidth, kLessonCinematicHeight,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != ESP_OK) {
        if (context->jpeg_input != nullptr) heap_caps_free(context->jpeg_input);
        context->jpeg_input = nullptr;
        jpeg_reusable_decoder_destroy(&context->decoder);
        context->last_error = LessonCinematicError::kInsufficientPsram;
        return false;
    }
    const LessonMjpegMp4Status open_status = context->file.OpenUnderLessonSession(
        path, context->assignment_id, context->session_id, context->generation);
    if (open_status != LessonMjpegMp4Status::kOk) {
        jpeg_reusable_decoder_destroy(&context->decoder);
        heap_caps_free(context->jpeg_input);
        context->jpeg_input = nullptr;
        context->last_error = open_status == LessonMjpegMp4Status::kLeaseUnavailable
            ? LessonCinematicError::kSessionMismatch
            : open_status == LessonMjpegMp4Status::kIoError
                ? LessonCinematicError::kFileOpen : LessonCinematicError::kParserFailed;
        return false;
    }
    const LessonMjpegMp4Metadata parsed = context->file.metadata();
    if (parsed.timescale == 0 || parsed.fps_milli % 1000 != 0 ||
        parsed.duration_ticks > UINT64_MAX / 1000 || parsed.frame_count > UINT32_MAX) {
        context->file.Close();
        jpeg_reusable_decoder_destroy(&context->decoder);
        heap_caps_free(context->jpeg_input);
        context->jpeg_input = nullptr;
        context->last_error = LessonCinematicError::kMetadataMismatch;
        return false;
    }
    *metadata = {parsed.width, parsed.height,
                 static_cast<std::uint16_t>(parsed.fps_milli / 1000),
                 static_cast<std::uint32_t>(parsed.frame_count),
                 static_cast<std::uint32_t>(parsed.duration_ticks * 1000 / parsed.timescale),
                 kLessonMjpegMp4MaxSampleBytes};
    *handle = &context->file;
    return true;
}

void ProductionClose(void* raw, void* handle) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    if (context == nullptr) return;
    if (handle != nullptr) static_cast<LessonMjpegMp4File*>(handle)->Close();
    jpeg_reusable_decoder_destroy(&context->decoder);
    if (context->jpeg_input != nullptr) heap_caps_free(context->jpeg_input);
    context->jpeg_input = nullptr;
}

bool ProductionDecode(void* raw, void* handle, std::size_t index,
                      std::uint8_t* destination, std::size_t capacity,
                      std::uint16_t* width, std::uint16_t* height, std::size_t* stride) {
    auto* context = static_cast<ProductionFlattenedRendererContext*>(raw);
    if (context != nullptr) context->last_error = LessonCinematicError::kNone;
    std::size_t jpeg_size = 0;
    if (context == nullptr || handle == nullptr || context->jpeg_input == nullptr ||
        static_cast<LessonMjpegMp4File*>(handle)->ReadFrame(
            index, context->jpeg_input, context->jpeg_input_capacity, &jpeg_size) !=
            LessonMjpegMp4Status::kOk) {
        if (context != nullptr) context->last_error = LessonCinematicError::kFileRead;
        return false;
    }
    std::size_t decoded_size = 0, decoded_width = 0, decoded_height = 0;
    if (jpeg_reusable_decoder_decode_into(
            &context->decoder, context->jpeg_input, jpeg_size, destination, capacity,
            &decoded_size, &decoded_width, &decoded_height, stride) != ESP_OK ||
        decoded_width > UINT16_MAX || decoded_height > UINT16_MAX) {
        context->last_error = LessonCinematicError::kDecodeFailed;
        return false;
    }
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
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2 +
        kLessonMjpegMp4MaxSampleBytes + 128 * 1024;
    if (display == nullptr || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kRequiredPsram) {
        return false;
    }
    void* framebuffer_probe = heap_caps_malloc(
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* jpeg_probe = heap_caps_malloc(kLessonMjpegMp4MaxSampleBytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpeg_reusable_decoder_t decoder_probe{};
    const bool decoder_ready = jpeg_reusable_decoder_prepare_workspace(
        &decoder_probe, kLessonCinematicWidth, kLessonCinematicHeight,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == ESP_OK;
    const bool ready = framebuffer_probe != nullptr && jpeg_probe != nullptr && decoder_ready;
    if (framebuffer_probe != nullptr) heap_caps_free(framebuffer_probe);
    if (jpeg_probe != nullptr) heap_caps_free(jpeg_probe);
    jpeg_reusable_decoder_destroy(&decoder_probe);
    if (!ready) return false;
    g_production_context = std::make_unique<ProductionFlattenedRendererContext>();
    g_production_context->display = display;
    g_production_renderer = std::make_unique<LessonFlattenedCinematicRenderer>(
        LessonCinematicRendererOps{g_production_context.get(), ProductionAllocate,
            ProductionFree, ProductionOpen, ProductionClose, ProductionDecode,
            ProductionPresent, ProductionLastError, ProductionMonotonicMs});
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
