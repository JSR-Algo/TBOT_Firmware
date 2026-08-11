#ifndef LESSON_LAYERED_CINEMATIC_RENDERER_H
#define LESSON_LAYERED_CINEMATIC_RENDERER_H

#include "lesson_cinematic_renderer.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace tbot {

inline constexpr char kLessonRendererV5[] = "teebot-lesson-renderer.v5";
inline constexpr char kLessonLayeredCinematicTemplate[] = "layeredCinematic";

enum class LessonLayeredPlaybackMode : std::uint8_t { kOnce, kLoop };

struct LessonLayeredImageConfig {
    const char* sd_path = nullptr;
    LessonCinematicRect rect{};
};

struct LessonLayeredCinematicPhaseConfig {
    const char* renderer_id = nullptr;
    const char* template_id = nullptr;
    const char* phase_id = nullptr;
    std::uint64_t command_sequence_id = 0;
    std::uint32_t duration_ms = 0;
    std::uint16_t fps = 0;
    std::uint32_t frame_count = 0;
    LessonLayeredPlaybackMode playback_mode = LessonLayeredPlaybackMode::kOnce;
    LessonLayeredImageConfig background{};
    LessonLayeredImageConfig teaching_object{};
    LessonCinematicLayerConfig robot{};
};

struct LessonLayeredCinematicRendererOps {
    void* context = nullptr;
    void* (*allocate)(void*, std::size_t) = nullptr;
    void (*free)(void*, void*) = nullptr;
    bool (*decode_jpeg)(void*, const char*, std::uint16_t*, std::size_t,
                        std::uint16_t*, std::uint16_t*, std::size_t*) = nullptr;
    bool (*decode_png)(void*, const char*, std::uint8_t*, std::size_t,
                       std::uint16_t*, std::uint16_t*, std::size_t*) = nullptr;
    bool (*open_video)(void*, const char*, LessonCinematicStreamMetadata*, void**) = nullptr;
    void (*close_video)(void*, void*) = nullptr;
    bool (*decode_video)(void*, void*, std::size_t, std::uint8_t*, std::size_t,
                         std::uint16_t*, std::uint16_t*, std::size_t*) = nullptr;
    bool (*present)(void*, const std::uint16_t*, std::uint16_t, std::uint16_t,
                    std::size_t) = nullptr;
    LessonCinematicError (*last_error)(void*) = nullptr;
    std::uint64_t (*monotonic_ms)(void*) = nullptr;
};

class LessonLayeredCinematicRenderer {
public:
    explicit LessonLayeredCinematicRenderer(LessonLayeredCinematicRendererOps ops);
    ~LessonLayeredCinematicRenderer();

    LessonCinematicResponse Prepare(const LessonLayeredCinematicPhaseConfig& config,
                                    std::uint64_t now_ms);
    LessonCinematicResponse Start(std::uint64_t sequence, const char* phase_id,
                                  std::uint64_t now_ms);
    LessonCinematicResponse Pause(std::uint64_t sequence, const char* phase_id,
                                  std::uint64_t now_ms);
    LessonCinematicResponse Resume(std::uint64_t sequence, const char* phase_id,
                                   std::uint64_t now_ms);
    LessonCinematicResponse Stop(std::uint64_t sequence, const char* phase_id);
    LessonCinematicResponse Cancel(std::uint64_t sequence, const char* phase_id);
    LessonCinematicResponse Tick(std::uint64_t now_ms);
    void DiscardSession();
    bool initialized() const;
    bool prepared() const;

private:
    enum class State : std::uint8_t { kIdle, kPrepared, kRunning, kPaused, kFailed };

    LessonCinematicResponse Failure(std::uint64_t sequence, LessonCinematicError error) const;
    LessonCinematicResponse Applied(LessonCinematicResponseType type,
                                    std::uint64_t sequence) const;
    LessonCinematicResponse ValidateControl(std::uint64_t sequence, const char* phase_id,
                                            const char* command) const;
    LessonCinematicError RenderFrame(std::size_t frame_index);
    LessonCinematicError OperationError(LessonCinematicError fallback) const;
    void Release();

    LessonLayeredCinematicRendererOps ops_{};
    mutable std::mutex mutex_;
    State state_ = State::kIdle;
    std::uint16_t* background_ = nullptr;
    std::uint16_t* framebuffer_ = nullptr;
    std::uint8_t* object_rgba_ = nullptr;
    std::uint8_t* robot_scratch_ = nullptr;
    std::uint16_t object_width_ = 0;
    std::uint16_t object_height_ = 0;
    std::size_t object_stride_ = 0;
    void* robot_stream_ = nullptr;
    LessonCinematicStreamMetadata robot_metadata_{};
    LessonCinematicRect object_rect_{};
    LessonCinematicLayerConfig robot_config_{};
    LessonLayeredPlaybackMode playback_mode_ = LessonLayeredPlaybackMode::kOnce;
    std::string phase_id_;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t clock_origin_ms_ = 0;
    std::uint64_t paused_at_ms_ = 0;
    std::size_t displayed_frame_ = 0;
    LessonCinematicResponse last_response_{};
    std::string last_command_;
};

bool LessonLayeredCinematicRendererCapabilityReady();
void SetActiveLessonLayeredCinematicRenderer(LessonLayeredCinematicRenderer* renderer);
LessonLayeredCinematicRenderer* ActiveLessonLayeredCinematicRenderer();
LessonCinematicResponse TickActiveLessonLayeredCinematicRenderer(std::uint64_t now_ms);
void SetLessonCinematicTimerRouteV5(bool enabled);
bool LessonCinematicTimerRoutesV5();
bool InitializeProductionLessonLayeredCinematicRenderer();
void ConfigureProductionLessonLayeredCinematicSession(const std::string& assignment_id,
                                                       const std::string& session_id,
                                                       std::uint64_t generation);
void ShutdownProductionLessonLayeredCinematicRenderer();

}  // namespace tbot

#endif  // LESSON_LAYERED_CINEMATIC_RENDERER_H
