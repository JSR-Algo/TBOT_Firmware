#ifndef LESSON_FLATTENED_CINEMATIC_RENDERER_H
#define LESSON_FLATTENED_CINEMATIC_RENDERER_H

#include "lesson_cinematic_renderer.h"

#include <cstdint>
#include <mutex>
#include <string>

class LcdDisplay;

namespace tbot {

inline constexpr char kLessonRendererV4[] = "teebot-lesson-renderer.v4";
inline constexpr char kLessonFlattenedMjpegCinematicTemplate[] =
    "flattenedMjpegCinematic";

bool LessonFlattenedCinematicRendererCapabilityReady();
void SetLessonFlattenedCinematicRendererCapabilityReady(bool ready);

struct LessonFlattenedCinematicAssetConfig {
    const char* derivative_id = nullptr;
    const char* phase_id = nullptr;
    const char* sd_path = nullptr;
    const char* sha256 = nullptr;
    std::uint64_t bytes = 0;
    const char* media_type = nullptr;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t container_version = 0;
    std::uint16_t stored_width = 0;
    std::uint16_t stored_height = 0;
    const char* orientation = nullptr;
    std::uint32_t frame_bytes = 0;
};

struct LessonFlattenedCinematicPhaseConfig {
    const char* renderer_id = nullptr;
    const char* template_id = nullptr;
    std::uint16_t template_version = 0;
    const char* phase_id = nullptr;
    std::uint64_t command_sequence_id = 0;
    std::uint32_t duration_ms = 0;
    std::uint16_t fps = 0;
    std::uint32_t frame_count = 0;
    bool loop = false;
    LessonFlattenedCinematicAssetConfig asset{};
};

struct LessonFlattenedCinematicRendererOps {
    LessonFlattenedCinematicRendererOps() = default;
    LessonFlattenedCinematicRendererOps(
        LessonCinematicRendererOps legacy_ops,
        bool (*begin)(void*),
        bool (*queue)(void*, const std::uint16_t*, std::uint16_t, std::uint16_t),
        bool (*wait)(void*, std::uint32_t),
        void (*end)(void*),
        bool (*native_open)(void*, const char*, LessonCinematicStreamMetadata*, void**) = nullptr,
        bool (*bytes)(void*, void*, std::uint64_t*) = nullptr)
        : legacy(legacy_ops), begin_cinematic(begin), queue_native(queue),
          wait_native(wait), end_cinematic(end), open_native(native_open), stream_bytes(bytes) {}

    LessonCinematicRendererOps legacy{};
    bool (*begin_cinematic)(void*) = nullptr;
    bool (*queue_native)(void*, const std::uint16_t*, std::uint16_t, std::uint16_t) = nullptr;
    bool (*wait_native)(void*, std::uint32_t) = nullptr;
    void (*end_cinematic)(void*) = nullptr;
    bool (*open_native)(void*, const char*, LessonCinematicStreamMetadata*, void**) = nullptr;
    bool (*stream_bytes)(void*, void*, std::uint64_t*) = nullptr;
};

class LessonFlattenedCinematicRenderer {
public:
    explicit LessonFlattenedCinematicRenderer(LessonCinematicRendererOps ops);
    explicit LessonFlattenedCinematicRenderer(LessonFlattenedCinematicRendererOps ops);
    ~LessonFlattenedCinematicRenderer();

    LessonCinematicResponse Prepare(const LessonFlattenedCinematicPhaseConfig& config,
                                    std::uint64_t now_ms);
    LessonCinematicResponse Start(std::uint64_t command_sequence_id, const char* phase_id,
                                  std::uint64_t now_ms);
    LessonCinematicResponse Pause(std::uint64_t command_sequence_id, const char* phase_id,
                                  std::uint64_t now_ms);
    LessonCinematicResponse Resume(std::uint64_t command_sequence_id, const char* phase_id,
                                   std::uint64_t now_ms);
    LessonCinematicResponse Stop(std::uint64_t command_sequence_id, const char* phase_id);
    LessonCinematicResponse Cancel(std::uint64_t command_sequence_id, const char* phase_id);
    LessonCinematicResponse Tick(std::uint64_t now_ms);

    bool initialized() const;
    bool prepared() const;
    void DiscardSession();

private:
    enum class State : std::uint8_t { kIdle, kPrepared, kRunning, kPaused, kFailed };

    LessonCinematicResponse Failure(std::uint64_t sequence, LessonCinematicError error) const;
    LessonCinematicResponse Applied(LessonCinematicResponseType type,
                                    std::uint64_t sequence) const;
    LessonCinematicResponse ValidateControl(std::uint64_t sequence, const char* phase_id,
                                            const char* command) const;
    LessonCinematicError RenderFrame(std::size_t frame_index);
    LessonCinematicError RenderFrameOn(void* stream, std::uint16_t* framebuffer,
                                       std::size_t frame_index);
    LessonCinematicError OperationError(LessonCinematicError fallback) const;
    LessonCinematicError PrepareNative(const LessonFlattenedCinematicPhaseConfig& config,
                                       void* stream,
                                       const LessonCinematicStreamMetadata& metadata,
                                       std::uint16_t* first,
                                       std::uint16_t* second);
    LessonCinematicError ReadNativeFrame(void* stream, std::size_t frame,
                                         std::uint16_t* destination,
                                         std::uint32_t deadline_ms);
    LessonCinematicError AdvanceNative(std::size_t target_frame);
    LessonCinematicError StartNativePlayback();
    bool FinishNativeTransfer(std::uint32_t timeout_ms);
    bool CleanupNative(std::uint32_t timeout_ms);
    void CloseStream();
    void ReleaseBuffer();
    void Reset();

    LessonCinematicRendererOps ops_{};
    LessonFlattenedCinematicRendererOps flattened_ops_{};
    mutable std::mutex mutex_;
    State state_ = State::kIdle;
    void* stream_ = nullptr;
    LessonCinematicStreamMetadata metadata_{};
    std::uint16_t* framebuffer_ = nullptr;
    std::uint16_t* native_buffers_[2] = {nullptr, nullptr};
    std::size_t native_queued_buffer_ = 0;
    std::size_t native_ready_buffer_ = 1;
    std::size_t native_ready_frame_ = 0;
    std::size_t native_next_read_frame_ = 0;
    bool native_mode_ = false;
    bool native_owned_ = false;
    bool native_in_flight_ = false;
    bool loop_ = false;
    std::uint32_t native_drops_ = 0;
    std::uint32_t successful_frames_since_drop_ = 0;
    bool has_timing_drop_ = false;
    bool last_tick_dropped_ = false;
    std::string phase_id_;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t clock_origin_ms_ = 0;
    std::uint64_t paused_at_ms_ = 0;
    std::size_t displayed_frame_ = 0;
    std::uint64_t displayed_clock_frame_ = 0;
    LessonCinematicResponse last_response_{};
    std::string last_command_;
    std::string last_fingerprint_;
};

void SetActiveLessonFlattenedCinematicRenderer(LessonFlattenedCinematicRenderer* renderer);
LessonFlattenedCinematicRenderer* ActiveLessonFlattenedCinematicRenderer();
LessonCinematicResponse TickActiveLessonFlattenedCinematicRenderer(std::uint64_t now_ms);
void SetLessonCinematicTimerRouteV4(bool route_v4);
bool LessonCinematicTimerRoutesV4();
bool InitializeProductionLessonFlattenedCinematicRenderer(::LcdDisplay* display);
void ConfigureProductionLessonFlattenedCinematicSession(const std::string& assignment_id,
                                                        const std::string& session_id,
                                                        std::uint64_t generation);
void ShutdownProductionLessonFlattenedCinematicRenderer();

}  // namespace tbot

#endif  // LESSON_FLATTENED_CINEMATIC_RENDERER_H
