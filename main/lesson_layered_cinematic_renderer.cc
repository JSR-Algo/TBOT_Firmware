#include "lesson_layered_cinematic_renderer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <inttypes.h>
#include <memory>
#include <string_view>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

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

bool LessonLayeredCinematicRenderer::last_apply_degraded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_apply_degraded_;
}

bool LessonLayeredCinematicRenderer::last_apply_presented() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_apply_presented_;
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
        (config.has_teaching_object && !LocalPath(config.teaching_object.sd_path)) ||
        !LocalPath(config.robot.sd_path) ||
        config.background.rect.x != 0 || config.background.rect.y != 0 ||
        config.background.rect.width != kLessonCinematicWidth ||
        config.background.rect.height != kLessonCinematicHeight ||
        (config.has_teaching_object && !ForegroundRect(config.teaching_object.rect)) ||
        !ForegroundRect(config.robot.rect)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }
    auto* old_background = background_;
    auto* old_framebuffer = framebuffer_;
    auto* old_object = object_rgba_;
    auto* old_robot_scratch = robot_scratch_;
    void* old_robot_stream = robot_stream_;
    const auto old_robot_metadata = robot_metadata_;
    const auto old_object_width = object_width_;
    const auto old_object_height = object_height_;
    const auto old_object_stride = object_stride_;
    const auto old_object_rect = object_rect_;
    const auto old_has_object = has_teaching_object_;
    const auto old_robot_config = robot_config_;
    const auto old_playback_mode = playback_mode_;
    const auto old_state = state_;
    const auto old_phase_id = phase_id_;
    const auto old_background_identity = background_identity_;
    const auto old_object_identity = object_identity_;
    const auto old_last_sequence = last_sequence_;
    const auto old_last_response = last_response_;
    const auto old_last_command = last_command_;
    const auto old_degraded = last_apply_degraded_;
    const auto old_presented = last_apply_presented_;
    const auto old_degraded_error = last_degraded_error_;
    bool committed = false;
    auto transaction = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1), [&, this](void*) {
            if (committed) {
                if (old_robot_stream != nullptr && old_robot_stream != robot_stream_) {
                    ops_.close_video(ops_.context, old_robot_stream);
                }
                if (old_robot_scratch != nullptr && old_robot_scratch != robot_scratch_) {
                    ops_.free(ops_.context, old_robot_scratch);
                }
                if (old_background != nullptr && old_background != background_) {
                    ops_.free(ops_.context, old_background);
                }
                if (old_object != nullptr && old_object != object_rgba_) {
                    ops_.free(ops_.context, old_object);
                }
                return;
            }
            if (background_ == old_background) background_ = nullptr;
            if (framebuffer_ == old_framebuffer) framebuffer_ = nullptr;
            if (object_rgba_ == old_object) object_rgba_ = nullptr;
            Release();
            background_ = old_background;
            framebuffer_ = old_framebuffer;
            object_rgba_ = old_object;
            robot_scratch_ = old_robot_scratch;
            robot_stream_ = old_robot_stream;
            robot_metadata_ = old_robot_metadata;
            object_width_ = old_object_width;
            object_height_ = old_object_height;
            object_stride_ = old_object_stride;
            object_rect_ = old_object_rect;
            has_teaching_object_ = old_has_object;
            robot_config_ = old_robot_config;
            playback_mode_ = old_playback_mode;
            state_ = old_state;
            phase_id_ = old_phase_id;
            background_identity_ = old_background_identity;
            object_identity_ = old_object_identity;
            last_sequence_ = old_last_sequence;
            last_response_ = old_last_response;
            last_command_ = old_last_command;
            last_apply_degraded_ = old_degraded;
            last_apply_presented_ = old_presented;
            last_degraded_error_ = old_degraded_error;
        });
    robot_stream_ = nullptr;
    robot_scratch_ = nullptr;
    robot_metadata_ = {};
    const std::string background_identity = config.background.identity != nullptr
        ? config.background.identity : config.background.sd_path;
    const std::string object_identity = config.has_teaching_object
        ? (config.teaching_object.identity != nullptr
               ? config.teaching_object.identity : config.teaching_object.sd_path)
        : std::string();
    const bool static_fallback_allowed = config.retain_static_layers;
    const bool keep_background = config.retain_static_layers && background_ != nullptr &&
        background_identity_ == background_identity;
    const bool keep_object = config.retain_static_layers &&
        config.has_teaching_object == has_teaching_object_ &&
        (!config.has_teaching_object ||
         (object_rgba_ != nullptr && object_identity_ == object_identity));

    phase_id_ = config.phase_id;
    std::uint16_t* new_background = keep_background ? background_ :
        static_cast<std::uint16_t*>(ops_.allocate(ops_.context, kScreenPixels * 2));
    std::uint8_t* new_object = keep_object ? object_rgba_ : config.has_teaching_object
        ? static_cast<std::uint8_t*>(ops_.allocate(ops_.context, kObjectCapacity)) : nullptr;
    if (framebuffer_ == nullptr) {
        framebuffer_ = static_cast<std::uint16_t*>(ops_.allocate(
            ops_.context, kScreenPixels * 2));
    }
    robot_scratch_ = static_cast<std::uint8_t*>(ops_.allocate(ops_.context, kRobotCapacity));
    if (new_background == nullptr || framebuffer_ == nullptr ||
        (config.has_teaching_object && new_object == nullptr) ||
        robot_scratch_ == nullptr) {
        if (!keep_background && new_background != nullptr) ops_.free(ops_.context, new_background);
        if (!keep_object && new_object != nullptr) ops_.free(ops_.context, new_object);
        ReleaseRobot();
        return Failure(config.command_sequence_id, LessonCinematicError::kInsufficientPsram);
    }
    std::uint16_t background_width = kLessonCinematicWidth;
    std::uint16_t background_height = kLessonCinematicHeight;
    std::size_t background_stride = kLessonCinematicWidth;
    [[maybe_unused]] const std::uint64_t background_started = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : 0;
    const bool background_decoded = keep_background ||
        ops_.decode_jpeg(ops_.context, config.background.sd_path, new_background,
                         kScreenPixels * 2, &background_width, &background_height,
                         &background_stride);
    if (!background_decoded || background_width != kLessonCinematicWidth ||
        background_height != kLessonCinematicHeight || background_stride != kLessonCinematicWidth) {
        if (!keep_background) ops_.free(ops_.context, new_background);
        if (!keep_object && new_object != nullptr) ops_.free(ops_.context, new_object);
        ReleaseRobot();
        return Failure(config.command_sequence_id,
                       OperationError(LessonCinematicError::kDecodeFailed));
    }
    [[maybe_unused]] const std::uint64_t background_finished = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : background_started;
#ifdef ESP_PLATFORM
    ESP_LOGI("LessonCinematic",
             "prepare decode layer=background frame=static elapsed_ms=%" PRIu64
             " deadline_ms=0 phase=%s path=%s",
             background_finished >= background_started ? background_finished - background_started : 0,
             config.phase_id, config.background.sd_path);
#endif
    [[maybe_unused]] const std::uint64_t object_started = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : 0;
    std::uint16_t new_object_width = keep_object ? object_width_ : 0;
    std::uint16_t new_object_height = keep_object ? object_height_ : 0;
    std::size_t new_object_stride = keep_object ? object_stride_ : 0;
    const bool object_decoded = !config.has_teaching_object || keep_object ||
        ops_.decode_png(ops_.context, config.teaching_object.sd_path, new_object,
                        kObjectCapacity, &new_object_width, &new_object_height,
                        &new_object_stride);
    if (!object_decoded || (config.has_teaching_object &&
        (new_object_width == 0 || new_object_height == 0 || new_object_width > 240 ||
         new_object_height > 240 ||
         new_object_stride < static_cast<std::size_t>(new_object_width) * 4))) {
        if (!keep_background) ops_.free(ops_.context, new_background);
        if (!keep_object && new_object != nullptr) ops_.free(ops_.context, new_object);
        ReleaseRobot();
        return Failure(config.command_sequence_id,
                       OperationError(LessonCinematicError::kDecodeFailed));
    }
    [[maybe_unused]] const std::uint64_t object_finished = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : object_started;
#ifdef ESP_PLATFORM
    if (config.has_teaching_object) {
        ESP_LOGI("LessonCinematic",
                 "prepare decode layer=teachingObject frame=static elapsed_ms=%" PRIu64
                 " deadline_ms=0 phase=%s path=%s",
                 object_finished >= object_started ? object_finished - object_started : 0,
                 config.phase_id, config.teaching_object.sd_path);
    }
#endif
    if (!keep_background) {
        background_ = new_background;
        background_identity_ = background_identity;
    }
    if (!keep_object) {
        object_rgba_ = new_object;
        object_identity_ = object_identity;
    }
    object_width_ = new_object_width;
    object_height_ = new_object_height;
    object_stride_ = new_object_stride;
    has_teaching_object_ = config.has_teaching_object;
    object_rect_ = config.teaching_object.rect;
    robot_config_ = config.robot;
    playback_mode_ = config.playback_mode;

    const bool robot_opened =
        ops_.open_video(ops_.context, config.robot.sd_path, &robot_metadata_, &robot_stream_) &&
        robot_stream_ != nullptr;
    if (!robot_opened) {
        if (!static_fallback_allowed) {
            return Failure(config.command_sequence_id,
                           OperationError(LessonCinematicError::kFileOpen));
        }
        ReleaseRobot();
        last_apply_degraded_ = true;
        last_degraded_error_ = OperationError(LessonCinematicError::kFileOpen);
        if (PresentStaticFrame(0) != LessonCinematicError::kNone) {
            return Failure(config.command_sequence_id, OperationError(LessonCinematicError::kFileOpen));
        }
        state_ = State::kPrepared;
        displayed_frame_ = 0;
        last_sequence_ = config.command_sequence_id;
        last_response_ = Applied(LessonCinematicResponseType::kFrameZeroReady, last_sequence_);
        last_command_ = "prepare";
        committed = true;
        return last_response_;
    }
    if (robot_metadata_.width == 0 || robot_metadata_.height == 0 ||
        robot_metadata_.width > 240 || robot_metadata_.height > 240 ||
        (robot_metadata_.fps != 10 && robot_metadata_.fps != 15) ||
        robot_metadata_.frame_count == 0 || robot_metadata_.duration_ms == 0 ||
        robot_metadata_.fps != config.fps ||
        robot_metadata_.frame_count != config.frame_count ||
        robot_metadata_.duration_ms != config.duration_ms) {
        ReleaseRobot();
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }
    const auto frame_error = RenderFrame(0);
    if (frame_error != LessonCinematicError::kNone) {
        if (!static_fallback_allowed) {
            state_ = State::kFailed;
            return Failure(config.command_sequence_id, frame_error);
        }
        ReleaseRobot();
        last_apply_degraded_ = true;
        last_degraded_error_ = frame_error;
        if (PresentStaticFrame(0) != LessonCinematicError::kNone) {
            state_ = State::kFailed;
            return Failure(config.command_sequence_id, frame_error);
        }
    } else {
        last_apply_degraded_ = false;
        last_degraded_error_ = LessonCinematicError::kNone;
    }
    state_ = State::kPrepared;
    displayed_frame_ = 0;
    last_sequence_ = config.command_sequence_id;
    last_response_ = Applied(LessonCinematicResponseType::kFrameZeroReady, last_sequence_);
    last_command_ = "prepare";
    committed = true;
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

LessonCinematicResponse LessonLayeredCinematicRenderer::ApplyVisualState(
    const LessonLayeredVisualState& visual_state, std::uint64_t sequence,
    std::uint64_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (visual_state.activity_id == nullptr || visual_state.activity_id[0] == '\0' ||
        !KnownPhase(visual_state.phase_id) ||
        !visual_state.retain_static_layers ||
        background_ == nullptr || framebuffer_ == nullptr) {
        return Failure(sequence, LessonCinematicError::kInvalidState);
    }
    const auto old_activity_id = activity_id_;
    const auto old_phase_id = phase_id_;
    const auto old_phase_variant = phase_variant_;
    const auto old_state = state_;
    const auto old_displayed_frame = displayed_frame_;
    const auto old_degraded = last_apply_degraded_;
    const auto old_presented = last_apply_presented_;
    const auto old_degraded_error = last_degraded_error_;
    const auto rollback = [&]() {
        activity_id_ = old_activity_id;
        phase_id_ = old_phase_id;
        phase_variant_ = old_phase_variant;
        state_ = old_state;
        displayed_frame_ = old_displayed_frame;
        last_apply_degraded_ = old_degraded;
        last_apply_presented_ = old_presented;
        last_degraded_error_ = old_degraded_error;
    };
    activity_id_ = visual_state.activity_id;
    phase_id_ = visual_state.phase_id;
    phase_variant_ = visual_state.phase_variant != nullptr ? visual_state.phase_variant : "";
    last_apply_degraded_ = false;
    last_apply_presented_ = false;
    last_degraded_error_ = LessonCinematicError::kNone;
    if (visual_state.replay_entrance) {
        const auto error = robot_stream_ != nullptr ? RenderFrame(0)
                                                    : LessonCinematicError::kFileOpen;
        if (error != LessonCinematicError::kNone) {
            last_apply_degraded_ = true;
            last_degraded_error_ = error;
            const auto static_error = PresentStaticFrame(0);
            if (static_error != LessonCinematicError::kNone) {
                rollback();
                return Failure(sequence, static_error);
            }
            last_apply_presented_ = true;
        } else {
            last_apply_presented_ = true;
        }
    } else {
        const auto static_error = PresentStaticFrame(0);
        if (static_error != LessonCinematicError::kNone) {
            rollback();
            return Failure(sequence, static_error);
        }
        last_apply_presented_ = true;
    }
    state_ = State::kPrepared;
    displayed_frame_ = 0;
    return Applied(LessonCinematicResponseType::kCommandApplied, sequence);
}

LessonCinematicResponse LessonLayeredCinematicRenderer::Start(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "start");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kPrepared) return Failure(sequence, LessonCinematicError::kInvalidState);
    last_apply_degraded_ = false;
    last_degraded_error_ = LessonCinematicError::kNone;
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
    if (state_ == State::kPrepared && last_apply_degraded_) {
        return Failure(last_sequence_, last_degraded_error_);
    }
    if (state_ != State::kRunning) {
        return Failure(last_sequence_, LessonCinematicError::kInvalidState);
    }
    if (robot_stream_ == nullptr || robot_metadata_.frame_count == 0) {
        state_ = State::kPrepared;
        return Applied(LessonCinematicResponseType::kPhaseComplete, last_sequence_);
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
        ReleaseRobot();
        last_apply_degraded_ = true;
        const auto static_error = PresentStaticFrame(displayed_frame_);
        if (static_error != LessonCinematicError::kNone) {
            last_degraded_error_ = static_error;
            state_ = State::kFailed;
            return Failure(last_sequence_, static_error);
        }
        last_degraded_error_ = error;
        state_ = State::kPrepared;
        return Failure(last_sequence_, error);
    }
    displayed_frame_ = static_cast<std::size_t>(frame);
    return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
}

LessonCinematicError LessonLayeredCinematicRenderer::RenderFrame(std::size_t frame_index) {
    std::memcpy(framebuffer_, background_, kScreenPixels * 2);
    if (has_teaching_object_ &&
        !CompositeObject(object_rgba_, object_width_, object_height_, object_stride_,
                         framebuffer_, object_rect_)) {
        return LessonCinematicError::kDecodeFailed;
    }
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t stride = 0;
    const std::uint64_t started = ops_.monotonic_ms != nullptr
        ? ops_.monotonic_ms(ops_.context) : 0;
    const bool decoded = ops_.decode_video(ops_.context, robot_stream_, frame_index,
                                           robot_scratch_, kRobotCapacity, &width,
                                           &height, &stride);
    if (ops_.monotonic_ms != nullptr) {
        const std::uint64_t finished = ops_.monotonic_ms(ops_.context);
#ifdef ESP_PLATFORM
        ESP_LOGI("LessonCinematic",
                 "prepare decode layer=robotOverlay frame=%u elapsed_ms=%" PRIu64
                 " deadline_ms=%" PRIu64 " phase=%s path=%s",
                 static_cast<unsigned>(frame_index),
                 finished >= started ? finished - started : 0, kDecodeDeadlineMs,
                 phase_id_.c_str(), robot_config_.sd_path);
#endif
        if (finished >= started && finished - started > kDecodeDeadlineMs) {
            return LessonCinematicError::kDecodeTimeout;
        }
    }
    if (!decoded) return OperationError(LessonCinematicError::kDecodeFailed);
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

LessonCinematicError LessonLayeredCinematicRenderer::PresentStaticFrame(
    std::size_t frame_index) {
    if (background_ == nullptr || framebuffer_ == nullptr) {
        return LessonCinematicError::kInvalidState;
    }
    std::memcpy(framebuffer_, background_, kScreenPixels * 2);
    if (has_teaching_object_ &&
        !CompositeObject(object_rgba_, object_width_, object_height_, object_stride_,
                         framebuffer_, object_rect_)) {
        return LessonCinematicError::kDecodeFailed;
    }
    return ops_.present(ops_.context, framebuffer_, kLessonCinematicWidth,
                        kLessonCinematicHeight, frame_index)
        ? LessonCinematicError::kNone : LessonCinematicError::kPresentFailed;
}

void LessonLayeredCinematicRenderer::ReleaseRobot() {
    if (robot_stream_ != nullptr) {
        ops_.close_video(ops_.context, robot_stream_);
        robot_stream_ = nullptr;
    }
    if (robot_scratch_ != nullptr) {
        ops_.free(ops_.context, robot_scratch_);
        robot_scratch_ = nullptr;
    }
    robot_metadata_ = {};
}

void LessonLayeredCinematicRenderer::Release() {
    ReleaseRobot();
    for (void* pointer : {static_cast<void*>(object_rgba_),
                          static_cast<void*>(framebuffer_), static_cast<void*>(background_)}) {
        if (pointer != nullptr) ops_.free(ops_.context, pointer);
    }
    object_rgba_ = nullptr;
    framebuffer_ = nullptr;
    background_ = nullptr;
    object_width_ = 0;
    object_height_ = 0;
    object_stride_ = 0;
    has_teaching_object_ = true;
    background_identity_.clear();
    object_identity_.clear();
    last_apply_degraded_ = false;
    last_apply_presented_ = false;
    last_degraded_error_ = LessonCinematicError::kNone;
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
    activity_id_.clear();
    phase_variant_.clear();
    last_apply_degraded_ = false;
    last_apply_presented_ = false;
    last_degraded_error_ = LessonCinematicError::kNone;
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
