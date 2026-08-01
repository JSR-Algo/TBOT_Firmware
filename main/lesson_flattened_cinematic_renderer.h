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

enum class LessonCinematicPlaybackMode : std::uint8_t { kOnce, kLoop };

struct LessonFlattenedCinematicAssetConfig {
    const char* derivative_id = nullptr;
    const char* phase_id = nullptr;
    const char* cue_id = nullptr;
    const char* sd_path = nullptr;
    const char* sha256 = nullptr;
    std::uint64_t bytes = 0;
    const char* media_type = nullptr;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct LessonFlattenedCinematicPhaseConfig {
    const char* renderer_id = nullptr;
    const char* template_id = nullptr;
    std::uint16_t template_version = 0;
    const char* phase_id = nullptr;
    const char* cue_id = nullptr;
    const char* effect = nullptr;
    const char* step_key = nullptr;
    LessonCinematicPlaybackMode playback_mode = LessonCinematicPlaybackMode::kOnce;
    // A fresh owner may restart sequencing, but only commits it after frame zero succeeds.
    bool new_session = false;
    std::uint64_t command_sequence_id = 0;
    std::uint32_t duration_ms = 0;
    std::uint16_t fps = 0;
    std::uint32_t frame_count = 0;
    LessonFlattenedCinematicAssetConfig asset{};
};

class LessonFlattenedCinematicRenderer {
public:
    explicit LessonFlattenedCinematicRenderer(LessonCinematicRendererOps ops);
    ~LessonFlattenedCinematicRenderer();

    LessonCinematicResponse Prepare(const LessonFlattenedCinematicPhaseConfig& config,
                                    std::uint64_t now_ms);
    LessonCinematicResponse Start(std::uint64_t command_sequence_id, const char* identity,
                                  std::uint64_t now_ms);
    LessonCinematicResponse Pause(std::uint64_t command_sequence_id, const char* identity,
                                  std::uint64_t now_ms);
    LessonCinematicResponse Resume(std::uint64_t command_sequence_id, const char* identity,
                                   std::uint64_t now_ms);
    LessonCinematicResponse Stop(std::uint64_t command_sequence_id, const char* identity);
    LessonCinematicResponse Cancel(std::uint64_t command_sequence_id, const char* identity);
    LessonCinematicResponse Tick(std::uint64_t now_ms);

    bool initialized() const;
    bool prepared() const;
    void DiscardSession();

private:
    enum class State : std::uint8_t { kIdle, kPrepared, kRunning, kPaused, kFailed };

    LessonCinematicResponse Failure(std::uint64_t sequence, LessonCinematicError error) const;
    LessonCinematicResponse Applied(LessonCinematicResponseType type,
                                    std::uint64_t sequence) const;
    LessonCinematicResponse TickApplied(LessonCinematicResponseType type,
                                        std::uint64_t sequence) const;
    LessonCinematicResponse ValidateControl(std::uint64_t sequence, const char* identity,
                                            const char* command) const;
    LessonCinematicError RenderFrame(std::size_t frame_index);
    LessonCinematicError RenderFrameOn(void* stream, std::uint16_t* framebuffer,
                                       std::size_t frame_index);
    LessonCinematicError OperationError(LessonCinematicError fallback) const;
    void CloseStream();
    void ReleaseBuffer();
    void Reset();

    LessonCinematicRendererOps ops_{};
    mutable std::mutex mutex_;
    State state_ = State::kIdle;
    void* stream_ = nullptr;
    LessonCinematicStreamMetadata metadata_{};
    std::uint16_t* framebuffer_ = nullptr;
    std::string phase_id_;
    std::string cue_id_;
    std::string effect_;
    std::string step_key_;
    std::uint16_t template_version_ = 0;
    LessonCinematicPlaybackMode playback_mode_ = LessonCinematicPlaybackMode::kOnce;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t clock_origin_ms_ = 0;
    std::uint64_t paused_at_ms_ = 0;
    std::size_t displayed_frame_ = 0;
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
