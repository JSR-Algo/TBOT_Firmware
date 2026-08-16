#include "lesson_layered_cinematic_renderer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string_view>

namespace tbot {
#ifdef ESP_PLATFORM
bool DecodeLessonLayeredJpeg(const char* path, std::uint16_t* destination,
                             std::size_t capacity, std::uint16_t* width,
                             std::uint16_t* height, std::size_t* stride_pixels);
bool DecodeLessonLayeredPng(const char* path, std::uint8_t* destination,
                            std::size_t capacity, std::uint16_t* width,
                            std::uint16_t* height, std::size_t* stride);
#endif
namespace {

std::atomic<bool> g_layered_capability_ready{false};
std::atomic<bool> g_timer_routes_v5{false};
std::atomic<LessonLayeredCinematicRenderer*> g_active_layered_renderer{nullptr};
std::mutex g_active_layered_mutex;
#ifdef ESP_PLATFORM
std::unique_ptr<LessonLayeredCinematicRenderer> g_production_layered_renderer;

bool ProductionDecodeJpeg(void*, const char* path, std::uint16_t* destination,
                          std::size_t capacity, std::uint16_t* width,
                          std::uint16_t* height, std::size_t* stride_pixels) {
    return DecodeLessonLayeredJpeg(path, destination, capacity, width, height, stride_pixels);
}

bool ProductionDecodePng(void*, const char* path, std::uint8_t* destination,
                         std::size_t capacity, std::uint16_t* width,
                         std::uint16_t* height, std::size_t* stride) {
    return DecodeLessonLayeredPng(path, destination, capacity, width, height, stride);
}
#endif

constexpr std::size_t kScreenPixels =
    static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight;
constexpr std::size_t kObjectCapacity = 240u * 240u * 4u;
constexpr std::size_t kRobotCapacity = 240u * 240u * 2u;
constexpr std::uint64_t kDecodeDeadlineMs = 100;

bool LocalPath(const char* path) {
    return path != nullptr && path[0] == '/' && std::strstr(path, "..") == nullptr &&
           std::strstr(path, "://") == nullptr;
}

bool ForegroundRect(const LessonCinematicRect& rect) {
    return rect.width != 0 && rect.height != 0 && rect.width <= 240 && rect.height <= 240 &&
           rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= kLessonCinematicWidth &&
           rect.y + rect.height <= kLessonCinematicHeight;
}

bool KnownPhase(const char* phase) {
    if (phase == nullptr) return false;
    for (const char* value : {"flyIn", "walk", "teach", "listen", "thinking", "celebrate", "exit"}) {
        if (std::string_view(phase) == value) return true;
    }
    return false;
}

std::uint16_t ToRgb565(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return static_cast<std::uint16_t>(((red & 0xf8u) << 8) | ((green & 0xfcu) << 3) |
                                      (blue >> 3));
}

std::uint8_t Red565(std::uint16_t pixel) {
    const auto value = static_cast<std::uint8_t>((pixel >> 11) & 0x1f);
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

std::uint8_t Green565(std::uint16_t pixel) {
    const auto value = static_cast<std::uint8_t>((pixel >> 5) & 0x3f);
    return static_cast<std::uint8_t>((value << 2) | (value >> 4));
}

std::uint8_t Blue565(std::uint16_t pixel) {
    const auto value = static_cast<std::uint8_t>(pixel & 0x1f);
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

std::uint16_t Blend(std::uint16_t background, const std::uint8_t* rgba) {
    const unsigned alpha = rgba[3];
    const unsigned inverse = 255u - alpha;
    const auto channel = [alpha, inverse](unsigned foreground, unsigned back) {
        return static_cast<std::uint8_t>((foreground * alpha + back * inverse + 127u) / 255u);
    };
    return ToRgb565(channel(rgba[0], Red565(background)),
                    channel(rgba[1], Green565(background)),
                    channel(rgba[2], Blue565(background)));
}

bool CompositeObject(const std::uint8_t* source, std::uint16_t width, std::uint16_t height,
                     std::size_t stride, std::uint16_t* destination,
                     const LessonCinematicRect& rect) {
    if (source == nullptr || destination == nullptr || width == 0 || height == 0 ||
        stride < static_cast<std::size_t>(width) * 4 || !ForegroundRect(rect)) {
        return false;
    }
    for (std::size_t y = 0; y < rect.height; ++y) {
        const std::size_t source_y = y * height / rect.height;
        auto* output = destination + (static_cast<std::size_t>(rect.y) + y) *
            kLessonCinematicWidth + rect.x;
        for (std::size_t x = 0; x < rect.width; ++x) {
            const std::size_t source_x = x * width / rect.width;
            const std::uint8_t* rgba = source + source_y * stride + source_x * 4;
            if (rgba[3] == 0) continue;
            output[x] = rgba[3] == 255 ? ToRgb565(rgba[0], rgba[1], rgba[2])
                                       : Blend(output[x], rgba);
        }
    }
    return true;
}

}  // namespace

LessonLayeredCinematicRenderer::LessonLayeredCinematicRenderer(
    LessonLayeredCinematicRendererOps ops) : ops_(ops) {}

LessonLayeredCinematicRenderer::~LessonLayeredCinematicRenderer() {
    DiscardSession();
}

bool LessonLayeredCinematicRenderer::initialized() const {
    return ops_.allocate != nullptr && ops_.free != nullptr && ops_.decode_jpeg != nullptr &&
           ops_.decode_png != nullptr && ops_.open_video != nullptr &&
           ops_.close_video != nullptr && ops_.decode_video != nullptr &&
           ops_.present != nullptr;
}

bool LessonLayeredCinematicRenderer::prepared() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kPrepared || state_ == State::kRunning ||
           state_ == State::kPaused;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Failure(
    std::uint64_t sequence, LessonCinematicError error) const {
    return {LessonCinematicResponseType::kFailure, false, sequence, phase_id_, error};
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Applied(
    LessonCinematicResponseType type, std::uint64_t sequence) const {
    return {type, true, sequence, phase_id_, LessonCinematicError::kNone};
}

LessonCinematicError LessonLayeredCinematicRenderer::OperationError(
    LessonCinematicError fallback) const {
    if (ops_.last_error == nullptr) return fallback;
    const auto error = ops_.last_error(ops_.context);
    return error == LessonCinematicError::kNone ? fallback : error;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Prepare(
    const LessonLayeredCinematicPhaseConfig& config, std::uint64_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized() || config.renderer_id == nullptr || config.template_id == nullptr ||
        std::string_view(config.renderer_id) != kLessonRendererV5 ||
        std::string_view(config.template_id) != kLessonLayeredCinematicTemplate) {
        return Failure(config.command_sequence_id, LessonCinematicError::kUnsupportedContract);
    }
    if (config.command_sequence_id < last_sequence_) {
        return Failure(config.command_sequence_id, LessonCinematicError::kStaleCommand);
    }
    if (!KnownPhase(config.phase_id) || !LocalPath(config.background.sd_path) ||
        !LocalPath(config.teaching_object.sd_path) || !LocalPath(config.robot.sd_path) ||
        config.background.rect.x != 0 || config.background.rect.y != 0 ||
        config.background.rect.width != kLessonCinematicWidth ||
        config.background.rect.height != kLessonCinematicHeight ||
        !ForegroundRect(config.teaching_object.rect) || !ForegroundRect(config.robot.rect)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }
    Release();
    phase_id_ = config.phase_id;
    background_ = static_cast<std::uint16_t*>(ops_.allocate(ops_.context, kScreenPixels * 2));
    framebuffer_ = static_cast<std::uint16_t*>(ops_.allocate(ops_.context, kScreenPixels * 2));
    object_rgba_ = static_cast<std::uint8_t*>(ops_.allocate(ops_.context, kObjectCapacity));
    robot_scratch_ = static_cast<std::uint8_t*>(ops_.allocate(ops_.context, kRobotCapacity));
    if (background_ == nullptr || framebuffer_ == nullptr || object_rgba_ == nullptr ||
        robot_scratch_ == nullptr) {
        Release();
        return Failure(config.command_sequence_id, LessonCinematicError::kInsufficientPsram);
    }
    std::uint16_t background_width = 0;
    std::uint16_t background_height = 0;
    std::size_t background_stride = 0;
    if (!ops_.decode_jpeg(ops_.context, config.background.sd_path, background_,
                          kScreenPixels * 2, &background_width, &background_height,
                          &background_stride) || background_width != kLessonCinematicWidth ||
        background_height != kLessonCinematicHeight ||
        background_stride != kLessonCinematicWidth) {
        Release();
        return Failure(config.command_sequence_id,
                       OperationError(LessonCinematicError::kDecodeFailed));
    }
    if (!ops_.decode_png(ops_.context, config.teaching_object.sd_path, object_rgba_,
                         kObjectCapacity, &object_width_, &object_height_, &object_stride_) ||
        object_width_ == 0 || object_height_ == 0 || object_width_ > 240 ||
        object_height_ > 240 || object_stride_ < static_cast<std::size_t>(object_width_) * 4) {
        Release();
        return Failure(config.command_sequence_id,
                       OperationError(LessonCinematicError::kDecodeFailed));
    }
    if (!ops_.open_video(ops_.context, config.robot.sd_path, &robot_metadata_, &robot_stream_) ||
        robot_stream_ == nullptr) {
        Release();
        return Failure(config.command_sequence_id, OperationError(LessonCinematicError::kFileOpen));
    }
    if (robot_metadata_.width == 0 || robot_metadata_.height == 0 ||
        robot_metadata_.width > 240 || robot_metadata_.height > 240 ||
        (robot_metadata_.fps != 10 && robot_metadata_.fps != 15) ||
        robot_metadata_.frame_count == 0 || robot_metadata_.duration_ms == 0 ||
        robot_metadata_.fps != config.fps ||
        robot_metadata_.frame_count != config.frame_count ||
        robot_metadata_.duration_ms != config.duration_ms) {
        Release();
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }
    object_rect_ = config.teaching_object.rect;
    robot_config_ = config.robot;
    playback_mode_ = config.playback_mode;
    const auto frame_error = RenderFrame(0);
    if (frame_error != LessonCinematicError::kNone) {
        Release();
        state_ = State::kFailed;
        return Failure(config.command_sequence_id, frame_error);
    }
    state_ = State::kPrepared;
    displayed_frame_ = 0;
    last_sequence_ = config.command_sequence_id;
    last_response_ = Applied(LessonCinematicResponseType::kFrameZeroReady, last_sequence_);
    last_command_ = "prepare";
    return last_response_;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::ValidateControl(
    std::uint64_t sequence, const char* phase_id, const char* command) const {
    if (sequence == last_sequence_ && last_response_.accepted && last_command_ == command) {
        return last_response_;
    }
    if (sequence <= last_sequence_ || phase_id == nullptr || phase_id_ != phase_id) {
        return Failure(sequence, LessonCinematicError::kStaleCommand);
    }
    return Applied(LessonCinematicResponseType::kCommandApplied, sequence);
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Start(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "start");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kPrepared) return Failure(sequence, LessonCinematicError::kInvalidState);
    state_ = State::kRunning;
    clock_origin_ms_ = now_ms;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kPhaseReady, sequence);
    last_command_ = "start";
    return last_response_;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Pause(
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
    return last_response_;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Resume(
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
    return last_response_;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Stop(
    std::uint64_t sequence, const char* phase_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "stop");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    Release();
    state_ = State::kIdle;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "stop";
    return last_response_;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Cancel(
    std::uint64_t sequence, const char* phase_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "cancel");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    Release();
    state_ = State::kIdle;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "cancel";
    return last_response_;
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Tick(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kPaused) {
        return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
    }
    if (state_ != State::kRunning) {
        return Failure(last_sequence_, LessonCinematicError::kInvalidState);
    }
    const std::uint64_t elapsed = now_ms >= clock_origin_ms_ ? now_ms - clock_origin_ms_ : 0;
    std::uint64_t frame = elapsed * robot_metadata_.fps / 1000;
    if (playback_mode_ == LessonLayeredPlaybackMode::kLoop) {
        frame %= robot_metadata_.frame_count;
    } else if (frame >= robot_metadata_.frame_count) {
        state_ = State::kPrepared;
        displayed_frame_ = robot_metadata_.frame_count - 1;
        return Applied(LessonCinematicResponseType::kPhaseComplete, last_sequence_);
    }
    if (frame == displayed_frame_) {
        return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
    }
    const auto error = RenderFrame(static_cast<std::size_t>(frame));
    if (error != LessonCinematicError::kNone) {
        state_ = State::kFailed;
        return Failure(last_sequence_, error);
    }
    displayed_frame_ = static_cast<std::size_t>(frame);
    return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
}

LessonCinematicError LessonLayeredCinematicRenderer::RenderFrame(std::size_t frame_index) {
    std::memcpy(framebuffer_, background_, kScreenPixels * 2);
    if (!CompositeObject(object_rgba_, object_width_, object_height_, object_stride_,
                         framebuffer_, object_rect_)) {
        return LessonCinematicError::kDecodeFailed;
    }
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t stride = 0;
    const std::uint64_t started = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : 0;
    if (!ops_.decode_video(ops_.context, robot_stream_, frame_index, robot_scratch_,
                           kRobotCapacity, &width, &height, &stride)) {
        return OperationError(LessonCinematicError::kDecodeFailed);
    }
    if (ops_.monotonic_ms != nullptr) {
        const std::uint64_t finished = ops_.monotonic_ms(ops_.context);
        if (finished >= started && finished - started > kDecodeDeadlineMs) {
            return LessonCinematicError::kDecodeTimeout;
        }
    }
    if (width == 0 || height == 0 || width > 240 || height > 240 ||
        stride != static_cast<std::size_t>(width) * 2 ||
        !LessonCompositeRgb565(
            {reinterpret_cast<const std::uint16_t*>(robot_scratch_), width, height, stride / 2},
            {framebuffer_, kLessonCinematicWidth, kLessonCinematicHeight,
             kLessonCinematicWidth}, robot_config_.rect, robot_config_.chroma)) {
        return LessonCinematicError::kDecodeFailed;
    }
    return ops_.present(ops_.context, framebuffer_, kLessonCinematicWidth,
                        kLessonCinematicHeight, frame_index)
        ? LessonCinematicError::kNone : LessonCinematicError::kPresentFailed;
}

void LessonLayeredCinematicRenderer::Release() {
    if (robot_stream_ != nullptr) {
        ops_.close_video(ops_.context, robot_stream_);
        robot_stream_ = nullptr;
    }
    for (void* pointer : {static_cast<void*>(robot_scratch_), static_cast<void*>(object_rgba_),
                          static_cast<void*>(framebuffer_), static_cast<void*>(background_)}) {
        if (pointer != nullptr) ops_.free(ops_.context, pointer);
    }
    robot_scratch_ = nullptr;
    object_rgba_ = nullptr;
    framebuffer_ = nullptr;
    background_ = nullptr;
    object_width_ = 0;
    object_height_ = 0;
    object_stride_ = 0;
}

void LessonLayeredCinematicRenderer::DiscardSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    Release();
    state_ = State::kIdle;
    phase_id_.clear();
    last_sequence_ = 0;
    clock_origin_ms_ = 0;
    paused_at_ms_ = 0;
    displayed_frame_ = 0;
    last_response_ = {};
    last_command_.clear();
}

bool LessonLayeredCinematicRendererCapabilityReady() {
    return g_layered_capability_ready.load(std::memory_order_acquire);
}

void SetActiveLessonLayeredCinematicRenderer(LessonLayeredCinematicRenderer* renderer) {
    std::lock_guard<std::mutex> lock(g_active_layered_mutex);
    g_active_layered_renderer.store(renderer, std::memory_order_release);
    g_layered_capability_ready.store(
        renderer != nullptr && renderer->initialized(), std::memory_order_release);
}

LessonLayeredCinematicRenderer* ActiveLessonLayeredCinematicRenderer() {
    return g_active_layered_renderer.load(std::memory_order_acquire);
}

LessonCinematicResponse TickActiveLessonLayeredCinematicRenderer(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(g_active_layered_mutex);
    auto* renderer = g_active_layered_renderer.load(std::memory_order_acquire);
    if (renderer == nullptr) {
        return {LessonCinematicResponseType::kFailure, false, 0, "",
                LessonCinematicError::kInvalidState};
    }
    return renderer->Tick(now_ms);
}

void SetLessonCinematicTimerRouteV5(bool enabled) {
    g_timer_routes_v5.store(enabled, std::memory_order_release);
}

bool LessonCinematicTimerRoutesV5() {
    return g_timer_routes_v5.load(std::memory_order_acquire);
}

bool InitializeProductionLessonLayeredCinematicRenderer() {
#ifdef ESP_PLATFORM
    const auto legacy = ProductionLessonCinematicRendererOps();
    if (legacy.allocate == nullptr || legacy.free == nullptr || legacy.open == nullptr ||
        legacy.close == nullptr || legacy.decode == nullptr || legacy.present == nullptr) {
        return false;
    }
    g_production_layered_renderer = std::make_unique<LessonLayeredCinematicRenderer>(
        LessonLayeredCinematicRendererOps{
            legacy.context, legacy.allocate, legacy.free, ProductionDecodeJpeg,
            ProductionDecodePng, legacy.open, legacy.close, legacy.decode, legacy.present,
            legacy.last_error, legacy.monotonic_ms});
    SetActiveLessonLayeredCinematicRenderer(g_production_layered_renderer.get());
    return LessonLayeredCinematicRendererCapabilityReady();
#else
    return false;
#endif
}

void ConfigureProductionLessonLayeredCinematicSession(const std::string& assignment_id,
                                                       const std::string& session_id,
                                                       std::uint64_t generation) {
#ifdef ESP_PLATFORM
    ConfigureProductionLessonCinematicSession(assignment_id, session_id, generation);
#else
    (void)assignment_id;
    (void)session_id;
    (void)generation;
#endif
}

void ShutdownProductionLessonLayeredCinematicRenderer() {
    SetActiveLessonLayeredCinematicRenderer(nullptr);
    g_timer_routes_v5.store(false, std::memory_order_release);
#ifdef ESP_PLATFORM
    g_production_layered_renderer.reset();
#endif
}

}  // namespace tbot
