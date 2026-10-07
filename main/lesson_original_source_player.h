#ifndef LESSON_ORIGINAL_SOURCE_PLAYER_H
#define LESSON_ORIGINAL_SOURCE_PLAYER_H

#include "lesson_original_source_compositor.h"
#include "lesson_original_source_scene_controller.h"
#include "lesson_original_source_session.h"
#include "lesson_tvideo_raster_canvas.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Renderer-v6 scene player: drives the scene controller, keeps one decode stream
// per media layer (background, teaching object, robot clip), shows for each layer
// the frame a browser shows at the canonical media time (last pts not after it),
// paints the canonical frame and presents RGB565. Pure logic over injected media,
// text and panel interfaces; the lesson handler supplies the SD pack and clock.
namespace tbot {

// Decoded frames of one pinned original, in presentation order. A frame borrows
// storage until the next Next call or destruction.
class OriginalSourceStream {
public:
    virtual ~OriginalSourceStream() = default;
    virtual OriginalSourceStatus Next(OriginalSourceFrame* frame) = 0;
};

class OriginalSourceMediaProvider {
public:
    virtual ~OriginalSourceMediaProvider() = default;
    // Opens the pinned original from the pack `cache_key`, verified against `original`.
    virtual OriginalSourceStatus Open(const std::string& cache_key, const OriginalSourceOriginal& original,
                                      std::unique_ptr<OriginalSourceStream>* out) = 0;
};

// Shows one 480x320 RGB565 frame; false when the panel refused it.
using OriginalSourcePresent = std::function<bool(const std::uint16_t* rgb565, int width, int height)>;

const char* OriginalSourceStatusName(OriginalSourceStatus status);

// Cumulative time per playback phase (microseconds), for on-device profiling.
struct OriginalSourcePlayerTimings {
    std::uint64_t open_us = 0, decode_us = 0, paint_us = 0, convert_us = 0, present_us = 0;
    std::uint64_t opens = 0, decoded_frames = 0, renders = 0;
};

class OriginalSourceScenePlayer {
public:
    OriginalSourceScenePlayer(OriginalSourceSceneLoader loader, OriginalSourceMediaProvider* media,
                              TVideoTextRenderer* text, OriginalSourcePresent present);
    OriginalSourceScenePlayer(const OriginalSourceScenePlayer&) = delete;
    OriginalSourceScenePlayer& operator=(const OriginalSourceScenePlayer&) = delete;

    // Applies one v6 control frame. A prepare is accepted only after the cue's
    // first frame was decoded, painted and presented.
    OriginalSourceControlResult Handle(const char* frame_type, const cJSON* body, std::uint64_t now_ms);
    // Presents the active cue's frame at `now_ms` when it differs from the frame on
    // the panel. Returns nullptr, or the reason the frame could not be shown.
    const char* Tick(std::uint64_t now_ms);
    // Closes every stream (session end or teardown).
    void Release();
    // Ends the lesson session: Release plus the controller's ordering state and scene.
    void Reset();

    const OriginalSourceSceneController& controller() const { return controller_; }
    std::uint64_t presented_frames() const { return presented_frames_; }
    std::uint64_t opened_streams() const { return opened_streams_; }
    const OriginalSourcePlayerTimings& timings() const { return timings_; }
    void ResetTimings() { timings_ = {}; }

private:
    struct Layer {
        std::string cache_key, asset_id;
        std::unique_ptr<OriginalSourceStream> stream;
        OriginalSourceFrame pending{};
        bool has_pending = false;
        bool has_shown = false;
        std::int64_t shown_pts = 0;
        int shown_num = 0, shown_den = 0;
        std::vector<std::uint8_t> storage;
        ComposeSource shown{};
    };

    const char* Render(const std::string& cache_key, const OriginalSourceCue& cue,
                       const OriginalSourceSceneInfo& scene, const OriginalSourceSceneAssets& assets,
                       const TVideoFrameState& state, const TVideoFrameLayout& layout);
    const char* RenderFrame(const std::string& cache_key, const OriginalSourceCue& cue,
                            const OriginalSourceSceneInfo& scene, const OriginalSourceSceneAssets& assets,
                            const TVideoFrameState& state, const TVideoFrameLayout& layout);
    const char* Select(Layer* layer, const std::string& cache_key, const OriginalSourceSceneInfo& scene,
                       const std::string& asset_id, double media_time_ms);
    const char* Reopen(Layer* layer, const std::string& cache_key, const OriginalSourceOriginal& original);
    void Keep(Layer* layer);

    OriginalSourceSceneController controller_;
    OriginalSourceMediaProvider* media_;
    TVideoTextRenderer* text_;
    OriginalSourcePresent present_;
    Layer background_, object_, robot_;
    std::vector<std::uint8_t> stage_;
    std::vector<std::uint16_t> rgb565_;
    bool shown_valid_ = false;
    std::string shown_cue_;
    double shown_frame_index_ = -1;
    OriginalSourcePlayerTimings timings_;
    std::uint64_t presented_frames_ = 0, opened_streams_ = 0;
};

}  // namespace tbot

#endif
