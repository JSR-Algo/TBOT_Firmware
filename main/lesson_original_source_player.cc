#include "lesson_original_source_player.h"

#include <chrono>

#include <algorithm>
#include <cstring>

namespace tbot {
namespace {
std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
}
}  // namespace

namespace {

const OriginalSourceOriginal* FindOriginal(const OriginalSourceSceneInfo& scene, const std::string& id) {
    for (const auto& original : scene.originals) {
        if (original.asset_version_id == id) return &original;
    }
    return nullptr;
}

// Browser seek rule: frame pts (in seconds) is not after the target time.
bool NotAfter(const OriginalSourceFrame& frame, double seconds) {
    return static_cast<double>(frame.pts) * frame.time_base_num <= seconds * frame.time_base_den + 1e-9;
}

TVideoMedia RobotMedia(TVideoClipRole role) {
    switch (role) {
        case TVideoClipRole::kFlight: return TVideoMedia::kRobotFlight;
        case TVideoClipRole::kWalking: return TVideoMedia::kRobotWalking;
        case TVideoClipRole::kGreetingTeaching: return TVideoMedia::kRobotGreetingTeaching;
        case TVideoClipRole::kCelebration: return TVideoMedia::kRobotCelebration;
    }
    return TVideoMedia::kRobotFlight;
}

}  // namespace

const char* OriginalSourceStatusName(OriginalSourceStatus status) {
    switch (status) {
        case OriginalSourceStatus::kOk: return "ok";
        case OriginalSourceStatus::kEnd: return "end";
        case OriginalSourceStatus::kInvalid: return "invalid";
        case OriginalSourceStatus::kIntegrity: return "integrity";
        case OriginalSourceStatus::kIo: return "io";
        case OriginalSourceStatus::kNoMemory: return "no memory";
        case OriginalSourceStatus::kCancelled: return "cancelled";
        case OriginalSourceStatus::kUnsupported: return "unsupported";
        case OriginalSourceStatus::kDecode: return "decode";
        case OriginalSourceStatus::kMetadata: return "metadata";
        case OriginalSourceStatus::kLeaseUnavailable: return "lease unavailable";
    }
    return "unknown";
}

OriginalSourceScenePlayer::OriginalSourceScenePlayer(OriginalSourceSceneLoader loader,
                                                     OriginalSourceMediaProvider* media, TVideoTextRenderer* text,
                                                     OriginalSourcePresent present)
    : controller_(std::move(loader)), media_(media), text_(text), present_(std::move(present)) {
    controller_.SetPrepareCheck([this](const OriginalSourcePrepareContext& context) -> const char* {
        // Frame zero of the cue, from the scene being prepared (not yet committed).
        TVideoFrameInput input;
        input.effect = context.cue.effect;
        input.time_ms = 0;
        input.scene = &context.path;
        input.progress_index = context.cue.progress_index;
        input.progress_count = context.cue.progress_count;
        TVideoFrameState state;
        TVideoFrameLayout layout;
        if (const char* error = EvaluateTVideoFrame(input, &state)) return error;
        LayoutTVideoFrame(state, &layout);
        shown_valid_ = false;
        if (const char* error = Render(context.cache_key, context.cue, context.scene, context.assets, state, layout)) {
            return error;
        }
        shown_valid_ = true;
        shown_cue_ = context.cue.cue_id;
        shown_frame_index_ = state.frame_index;
        return nullptr;
    });
}

void OriginalSourceScenePlayer::Release() {
    for (Layer* layer : {&background_, &object_, &robot_}) *layer = Layer{};
    shown_valid_ = false;
}

void OriginalSourceScenePlayer::Reset() {
    Release();
    controller_.Reset();
}

void OriginalSourceScenePlayer::Keep(Layer* layer) {
    const OriginalSourceFrame& frame = layer->pending;
    const int width = static_cast<int>(frame.width), height = static_cast<int>(frame.height);
    ComposeSource& shown = layer->shown;
    shown = ComposeSource{};
    shown.width = width;
    shown.height = height;
    shown.rgba = frame.rgba;
    shown.alpha = frame.alpha;
    int widths[4], heights[4], planes = 0;
    if (frame.rgba) {
        widths[0] = width * 4;
        heights[0] = height;
        planes = 1;
    } else {
        const int chroma_width = (width + 1) / 2, chroma_height = (height + 1) / 2;
        widths[0] = width;
        heights[0] = height;
        widths[1] = widths[2] = chroma_width;
        heights[1] = heights[2] = chroma_height;
        planes = 3;
        if (frame.alpha) {
            widths[3] = width;
            heights[3] = height;
            planes = 4;
        }
    }
    std::size_t total = 0;
    for (int plane = 0; plane < planes; ++plane) total += static_cast<std::size_t>(widths[plane]) * heights[plane];
    if (layer->storage.size() < total) layer->storage.resize(total);
    std::uint8_t* cursor = layer->storage.data();
    for (int plane = 0; plane < planes; ++plane) {
        for (int row = 0; row < heights[plane]; ++row) {
            std::memcpy(cursor + static_cast<std::size_t>(row) * widths[plane],
                        frame.planes[plane] + static_cast<std::ptrdiff_t>(row) * frame.strides[plane], widths[plane]);
        }
        shown.planes[plane] = cursor;
        shown.strides[plane] = widths[plane];
        cursor += static_cast<std::size_t>(widths[plane]) * heights[plane];
    }
    layer->has_shown = true;
    layer->shown_pts = frame.pts;
    layer->shown_num = frame.time_base_num;
    layer->shown_den = frame.time_base_den;
}

const char* OriginalSourceScenePlayer::Reopen(Layer* layer, const std::string& cache_key,
                                             const OriginalSourceOriginal& original) {
    layer->stream.reset();
    layer->has_pending = layer->has_shown = false;
    layer->asset_id.clear();
    layer->cache_key.clear();
    if (media_ == nullptr) return "no media provider";
    std::unique_ptr<OriginalSourceStream> stream;
    const auto open_start = std::chrono::steady_clock::now();
    const OriginalSourceStatus opened = media_->Open(cache_key, original, &stream);
    if (opened != OriginalSourceStatus::kOk || !stream) return OriginalSourceStatusName(opened);
    ++opened_streams_;
    const OriginalSourceStatus first = stream->Next(&layer->pending);
    timings_.open_us += ElapsedUs(open_start);
    ++timings_.opens;
    if (first == OriginalSourceStatus::kEnd) return "original has no frames";
    if (first != OriginalSourceStatus::kOk) return OriginalSourceStatusName(first);
    layer->stream = std::move(stream);
    layer->has_pending = true;
    layer->asset_id = original.asset_version_id;
    layer->cache_key = cache_key;
    return nullptr;
}

const char* OriginalSourceScenePlayer::Select(Layer* layer, const std::string& cache_key,
                                             const OriginalSourceSceneInfo& scene, const std::string& asset_id,
                                             double media_time_ms) {
    const OriginalSourceOriginal* original = FindOriginal(scene, asset_id);
    if (original == nullptr) return "layer is not a pinned original";
    // Unbounded duration: past the last frame the last frame stays, as a clamped
    // browser seek shows it.
    const double seconds = CanonicalMediaSeconds(media_time_ms, 1e30);
    const bool rewound = layer->has_shown && static_cast<double>(layer->shown_pts) * layer->shown_num >
                                                 seconds * layer->shown_den + 1e-9;
    // An ended layer has no stream but keeps its last frame; it reopens only when
    // its identity changes or its media time goes back.
    if (layer->cache_key != cache_key || layer->asset_id != asset_id || rewound || !layer->has_shown) {
        if (const char* error = Reopen(layer, cache_key, *original)) return error;
    }
    while (layer->has_pending && (!layer->has_shown || NotAfter(layer->pending, seconds))) {
        Keep(layer);
        const auto decode_start = std::chrono::steady_clock::now();
        const OriginalSourceStatus next = layer->stream->Next(&layer->pending);
        timings_.decode_us += ElapsedUs(decode_start);
        ++timings_.decoded_frames;
        if (next == OriginalSourceStatus::kEnd) {
            // The last frame is copied: release the decoder and its file snapshot.
            layer->has_pending = false;
            layer->stream.reset();
        } else if (next != OriginalSourceStatus::kOk) {
            layer->has_pending = false;
            layer->stream.reset();
            layer->asset_id.clear();  // reopen on the next frame
            return OriginalSourceStatusName(next);
        }
    }
    return nullptr;
}

const char* OriginalSourceScenePlayer::Render(const std::string& cache_key, const OriginalSourceCue& cue,
                                              const OriginalSourceSceneInfo& scene,
                                              const OriginalSourceSceneAssets& assets, const TVideoFrameState& state,
                                              const TVideoFrameLayout& layout) {
    const std::string& robot_id = assets.robot_clip_ids[static_cast<std::size_t>(layout.robot.clip_role)];
    if (const char* error =
            Select(&background_, cache_key, scene, assets.background_id, layout.background_media_time_ms)) {
        return error;
    }
    if (const char* error = Select(&object_, cache_key, scene, cue.teaching_object_id, layout.object.media_time_ms)) {
        return error;
    }
    if (const char* error = Select(&robot_, cache_key, scene, robot_id, layout.robot.media_time_ms)) return error;
    stage_.resize(static_cast<std::size_t>(kTVideoStageWidth) * kTVideoStageHeight * 3);
    TVideoRasterCanvas canvas(stage_.data(), text_);
    canvas.SetMedia(TVideoMedia::kBackground, &background_.shown);
    canvas.SetMedia(TVideoMedia::kTeachingObject, &object_.shown);
    canvas.SetMedia(RobotMedia(layout.robot.clip_role), &robot_.shown);
    const auto paint_start = std::chrono::steady_clock::now();
    if (const char* error = PaintTVideoFrame(&canvas, state, layout, cue.copy)) return error;
    if (canvas.unsupported() != nullptr) return canvas.unsupported();
    timings_.paint_us += ElapsedUs(paint_start);
    const auto convert_start = std::chrono::steady_clock::now();
    rgb565_.resize(static_cast<std::size_t>(kTVideoStageWidth) * kTVideoStageHeight);
    ConvertRgb888ToRgb565(stage_.data(), rgb565_.data(), rgb565_.size());
    timings_.convert_us += ElapsedUs(convert_start);
    const auto present_start = std::chrono::steady_clock::now();
    if (!present_ || !present_(rgb565_.data(), kTVideoStageWidth, kTVideoStageHeight)) return "panel refused the frame";
    timings_.present_us += ElapsedUs(present_start);
    ++timings_.renders;
    ++presented_frames_;
    return nullptr;
}

OriginalSourceControlResult OriginalSourceScenePlayer::Handle(const char* frame_type, const cJSON* body,
                                                              std::uint64_t now_ms) {
    OriginalSourceControlResult result = controller_.Handle(frame_type, body, now_ms);
    if (result.accepted && result.terminal) Release();
    return result;
}

const char* OriginalSourceScenePlayer::Tick(std::uint64_t now_ms) {
    const OriginalSourceCue* cue = controller_.active_cue();
    if (cue == nullptr) return nullptr;
    TVideoFrameState state;
    TVideoFrameLayout layout;
    if (!controller_.FrameAt(now_ms, &state, &layout)) return "frame state unavailable";
    if (shown_valid_ && shown_cue_ == cue->cue_id && shown_frame_index_ == state.frame_index) return nullptr;
    shown_valid_ = false;
    if (const char* error = Render(controller_.cache_key(), *cue, controller_.scene_info(),
                                   controller_.scene_assets(), state, layout)) {
        return error;
    }
    shown_valid_ = true;
    shown_cue_ = cue->cue_id;
    shown_frame_index_ = state.frame_index;
    return nullptr;
}

}  // namespace tbot
