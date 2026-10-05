#ifndef LESSON_ORIGINAL_SOURCE_RUNTIME_H
#define LESSON_ORIGINAL_SOURCE_RUNTIME_H

#include "lesson_cinematic_renderer.h"
#include "lesson_layered_cinematic_renderer.h"
#include "lesson_original_source_player.h"

#include <cJSON.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Renderer-v6 runtime owned by the lesson handler: serializes the scene player
// between the lesson worker (control frames) and the shared cinematic frame task
// (ticks), and reports a playback failure through the same runtime-error interface
// as the v5 layered renderer (generation, pending error, release, acknowledge).
namespace tbot {

// Capabilities the R01 decoder qualified on the device FFmpeg configuration.
inline constexpr const char* kOriginalSourceDeviceCapabilities[] = {
    "decode.h264.high.yuv420p8",
    "decode.png.rgba8",
    "decode.vp9.profile0.yuv420p8.alpha-blockadditional",
    "scene.tvideo-journey.v1",
};

// Wire error code of a refused control frame or a playback failure reason.
LessonCinematicError OriginalSourceErrorCode(const std::string& reason);

class OriginalSourceRuntime {
public:
    explicit OriginalSourceRuntime(std::unique_ptr<OriginalSourceScenePlayer> player);

    OriginalSourceControlResult Handle(const char* frame_type, const cJSON* body, std::uint64_t now_ms);
    // Presents the frame at `now_ms`. The first failure stops playback and becomes
    // the pending runtime error of the current generation; false when nothing ran.
    bool Tick(std::uint64_t now_ms);
    void DiscardSession();

    std::uint64_t RuntimeGeneration() const;
    std::optional<LessonLayeredRuntimeError> PendingRuntimeError() const;
    bool AcknowledgeRuntimeError(std::uint64_t generation, std::uint64_t sequence);
    bool ReleaseFailedRuntimeResources(std::uint64_t generation, std::uint64_t sequence);
    bool WithRuntimeGeneration(std::uint64_t generation, const std::function<void()>& operation);

private:
    void AdvanceRuntimeGeneration();

    mutable std::mutex mutex_;
    std::unique_ptr<OriginalSourceScenePlayer> player_;
    std::uint64_t runtime_generation_ = 1;
    bool failed_ = false;
    std::optional<LessonLayeredRuntimeError> pending_runtime_error_;
};

void SetActiveOriginalSourceRuntime(OriginalSourceRuntime* runtime);
OriginalSourceRuntime* ActiveOriginalSourceRuntime();
// The device advertises v6 only when a runtime is active and advertisement is
// enabled (CONFIG_TBOT_LESSON_RENDERER_V6_ADVERTISE after on-device qualification).
void SetOriginalSourceRendererAdvertised(bool advertised);
bool OriginalSourceRendererCapabilityReady();
// Adds renderer v6 to `renderers` and features.lessonRendererV6 when ready.
void AddOriginalSourceRendererFeatures(cJSON* features, cJSON* renderers);
bool TickActiveOriginalSourceRuntime(std::uint64_t now_ms);
void SetLessonCinematicTimerRouteV6(bool enabled);
bool LessonCinematicTimerRoutesV6();

// Production wiring (ESP_PLATFORM): routed decoder allocator, SD pack media and
// scene loader bound to the active lesson storage session, LVGL text, panel present.
class LcdDisplayPresenter {
public:
    virtual ~LcdDisplayPresenter() = default;
    virtual bool Present(const std::uint16_t* rgb565, int width, int height) = 0;
};
bool InitializeProductionOriginalSourceRuntime(LcdDisplayPresenter* panel, TVideoTextRenderer* text);
void ConfigureProductionOriginalSourceSession(const std::string& assignment_id, const std::string& session_id,
                                              std::uint64_t generation);
void ShutdownProductionOriginalSourceRuntime();

}  // namespace tbot

#endif
