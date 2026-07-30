#ifndef LESSON_CINEMATIC_RENDERER_H
#define LESSON_CINEMATIC_RENDERER_H

#include "lesson_chroma_compositor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <mutex>

class LcdDisplay;

namespace tbot {

inline constexpr char kLessonRendererV3[] = "teebot-lesson-renderer.v3";
inline constexpr char kLessonDirectMp4CinematicTemplate[] = "directMp4Cinematic";
inline constexpr std::uint16_t kLessonCinematicWidth = 480;
inline constexpr std::uint16_t kLessonCinematicHeight = 320;

bool LessonCinematicRendererCapabilityReady();
void SetLessonCinematicRendererCapabilityReady(bool ready);

enum class LessonCinematicError : std::uint8_t {
    kNone,
    kUnsupportedContract,
    kInvalidPhase,
    kInvalidPath,
    kFileOpen,
    kMetadataMismatch,
    kInsufficientPsram,
    kDecodeFailed,
    kPresentFailed,
    kStaleCommand,
    kInvalidState,
};

enum class LessonCinematicResponseType : std::uint8_t {
    kFrameZeroReady,
    kPhaseReady,
    kCommandApplied,
    kPhaseComplete,
    kFailure,
};

struct LessonCinematicResponse {
    LessonCinematicResponseType type = LessonCinematicResponseType::kFailure;
    bool accepted = false;
    std::uint64_t command_sequence_id = 0;
    std::string phase_id;
    LessonCinematicError error = LessonCinematicError::kNone;
};

struct LessonCinematicStreamMetadata {
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t fps;
    std::uint32_t frame_count;
    std::uint32_t duration_ms;
    std::uint32_t max_frame_bytes;
};

struct LessonCinematicLayerConfig {
    const char* sd_path = nullptr;
    LessonCinematicRect rect{};
    LessonChromaKey chroma{};
};

struct LessonCinematicPhaseConfig {
    const char* renderer_id = nullptr;
    const char* template_id = nullptr;
    const char* phase_id = nullptr;
    std::uint64_t command_sequence_id = 0;
    std::array<LessonCinematicLayerConfig, 3> layers{};
};

struct LessonCinematicRendererOps {
    void* context = nullptr;
    void* (*allocate)(void*, std::size_t) = nullptr;
    void (*free)(void*, void*) = nullptr;
    bool (*open)(void*, const char*, LessonCinematicStreamMetadata*, void**) = nullptr;
    void (*close)(void*, void*) = nullptr;
    bool (*decode)(void*, void*, std::size_t, std::uint8_t*, std::size_t,
                   std::uint16_t*, std::uint16_t*, std::size_t*) = nullptr;
    bool (*present)(void*, const std::uint16_t*, std::uint16_t, std::uint16_t,
                    std::size_t) = nullptr;
};

class LessonCinematicRenderer {
public:
    explicit LessonCinematicRenderer(LessonCinematicRendererOps ops);
    ~LessonCinematicRenderer();

    LessonCinematicResponse Prepare(const LessonCinematicPhaseConfig& config,
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

private:
    enum class State : std::uint8_t { kIdle, kPrepared, kRunning, kPaused, kFailed };

    LessonCinematicResponse Failure(std::uint64_t sequence, LessonCinematicError error) const;
    LessonCinematicResponse Applied(LessonCinematicResponseType type,
                                    std::uint64_t sequence) const;
    LessonCinematicResponse ValidateControl(std::uint64_t sequence, const char* phase_id) const;
    bool RenderFrame(std::size_t frame_index);
    void CloseStreams();
    void ReleaseBuffers();
    void Reset();

    LessonCinematicRendererOps ops_{};
    mutable std::mutex mutex_;
    State state_ = State::kIdle;
    std::array<void*, 3> streams_{};
    std::array<LessonCinematicStreamMetadata, 3> metadata_{};
    std::array<LessonCinematicLayerConfig, 3> layers_{};
    std::uint16_t* framebuffer_ = nullptr;
    std::uint8_t* foreground_scratch_ = nullptr;
    std::size_t foreground_capacity_ = 0;
    std::string phase_id_;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t clock_origin_ms_ = 0;
    std::uint64_t paused_at_ms_ = 0;
    std::size_t displayed_frame_ = 0;
    LessonCinematicResponse last_response_{};
};

void SetActiveLessonCinematicRenderer(LessonCinematicRenderer* renderer);
LessonCinematicRenderer* ActiveLessonCinematicRenderer();
bool InitializeProductionLessonCinematicRenderer(::LcdDisplay* display);
void ConfigureProductionLessonCinematicSession(const std::string& assignment_id,
                                                const std::string& session_id,
                                                std::uint64_t generation);
void ShutdownProductionLessonCinematicRenderer();

}  // namespace tbot

#endif  // LESSON_CINEMATIC_RENDERER_H
