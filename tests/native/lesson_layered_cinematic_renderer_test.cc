#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "lesson_layered_cinematic_renderer.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson_layered_cinematic_renderer test failed: " << message << '\n';
        std::exit(1);
    }
}

struct FakeRuntime {
    std::uint64_t now_ms = 0;
    std::size_t allocations = 0;
    std::size_t frees = 0;
    std::size_t jpeg_decodes = 0;
    std::size_t png_decodes = 0;
    std::size_t video_opens = 0;
    std::size_t video_closes = 0;
    std::size_t video_decodes = 0;
    std::size_t presents = 0;
    std::size_t last_frame = 999;
    bool fail_static = false;
    bool fail_video = false;
    bool fail_present = false;
    bool enough_memory = true;
    std::vector<std::size_t> video_frames;
};

void* Allocate(void* raw, std::size_t size) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (!fake.enough_memory) return nullptr;
    ++fake.allocations;
    return std::malloc(size);
}

void Free(void* raw, void* pointer) {
    ++static_cast<FakeRuntime*>(raw)->frees;
    std::free(pointer);
}

bool DecodeJpeg(void* raw, const char*, std::uint16_t* destination, std::size_t capacity,
                std::uint16_t* width, std::uint16_t* height, std::size_t* stride_pixels) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    ++fake.jpeg_decodes;
    if (fake.fail_static || capacity < 480u * 320u * 2u) return false;
    *width = 480;
    *height = 320;
    *stride_pixels = 480;
    std::fill_n(destination, 480u * 320u, static_cast<std::uint16_t>(0x001f));
    return true;
}

bool DecodePng(void* raw, const char*, std::uint8_t* destination, std::size_t capacity,
               std::uint16_t* width, std::uint16_t* height, std::size_t* stride) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    ++fake.png_decodes;
    if (fake.fail_static || capacity < 2u * 2u * 4u) return false;
    *width = 2;
    *height = 2;
    *stride = 8;
    const std::uint8_t rgba[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 0, 0, 255, 255, 255, 255,
    };
    std::memcpy(destination, rgba, sizeof(rgba));
    return true;
}

bool OpenVideo(void* raw, const char*, tbot::LessonCinematicStreamMetadata* metadata,
               void** handle) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    ++fake.video_opens;
    *metadata = {2, 2, 10, 3, 300, 16};
    *handle = &fake;
    return !fake.fail_video;
}

void CloseVideo(void* raw, void*) {
    ++static_cast<FakeRuntime*>(raw)->video_closes;
}

bool DecodeVideo(void* raw, void*, std::size_t frame_index, std::uint8_t* destination,
                 std::size_t capacity, std::uint16_t* width, std::uint16_t* height,
                 std::size_t* stride) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    ++fake.video_decodes;
    fake.video_frames.push_back(frame_index);
    if (fake.fail_video || capacity < 8) return false;
    *width = 2;
    *height = 2;
    *stride = 4;
    auto* pixels = reinterpret_cast<std::uint16_t*>(destination);
    pixels[0] = 0xf800;
    pixels[1] = 0x07e0;
    pixels[2] = 0xffff;
    pixels[3] = 0x001f;
    return true;
}

bool Present(void* raw, const std::uint16_t*, std::uint16_t width, std::uint16_t height,
             std::size_t frame_index) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    ++fake.presents;
    fake.last_frame = frame_index;
    return !fake.fail_present && width == 480 && height == 320;
}

tbot::LessonCinematicError LastError(void*) {
    return tbot::LessonCinematicError::kNone;
}

std::uint64_t Monotonic(void* raw) {
    return static_cast<FakeRuntime*>(raw)->now_ms;
}

tbot::LessonLayeredCinematicRendererOps Ops(FakeRuntime* fake) {
    return {fake, Allocate, Free, DecodeJpeg, DecodePng, OpenVideo, CloseVideo,
            DecodeVideo, Present, LastError, Monotonic};
}

tbot::LessonLayeredCinematicPhaseConfig Config() {
    tbot::LessonLayeredCinematicPhaseConfig config{};
    config.renderer_id = "teebot-lesson-renderer.v5";
    config.template_id = "layeredCinematic";
    config.phase_id = "teach";
    config.command_sequence_id = 7;
    config.duration_ms = 300;
    config.fps = 10;
    config.frame_count = 3;
    config.playback_mode = tbot::LessonLayeredPlaybackMode::kOnce;
    config.background.sd_path = "/sd/background.jpg";
    config.background.rect = {0, 0, 480, 320};
    config.teaching_object.sd_path = "/sd/object.png";
    config.teaching_object.rect = {10, 10, 2, 2};
    config.robot.sd_path = "/sd/robot.mp4";
    config.robot.rect = {20, 20, 2, 2};
    config.robot.chroma = {{0, 255, 0}, 20, 1};
    return config;
}

void TestStaticLayersDecodeOnceAndRobotOwnsClock() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto response = renderer.Prepare(Config(), 0);
    Require(response.accepted && response.type == tbot::LessonCinematicResponseType::kFrameZeroReady,
            "prepare presents frame zero");
    Require(fake.jpeg_decodes == 1 && fake.png_decodes == 1 && fake.video_decodes == 1,
            "prepare decodes two static layers once and Robot frame zero once");
    Require(fake.presents == 1 && fake.last_frame == 0, "frame zero is presented");

    Require(renderer.Start(8, "teach", 0).accepted, "start succeeds");
    fake.now_ms = 110;
    Require(renderer.Tick(fake.now_ms).accepted && fake.last_frame == 1,
            "Robot clock advances to frame one");
    Require(fake.jpeg_decodes == 1 && fake.png_decodes == 1 && fake.video_decodes == 2,
            "steady playback decodes only Robot video");
    Require(fake.allocations == 4, "prepare performs four bounded buffer allocations");

    Require(renderer.Pause(9, "teach", fake.now_ms).accepted, "pause succeeds");
    fake.now_ms = 1000;
    Require(renderer.Tick(fake.now_ms).accepted && fake.last_frame == 1, "pause freezes Robot frame");
    Require(renderer.Resume(10, "teach", fake.now_ms).accepted, "resume succeeds");
    fake.now_ms = 1110;
    Require(renderer.Tick(fake.now_ms).accepted && fake.last_frame == 2, "resume rebases clock");
    Require(renderer.Stop(11, "teach").accepted, "stop succeeds");
    Require(fake.video_closes == 1 && fake.frees == 4, "stop closes stream and releases buffers");
}

void TestTypedFailuresAndLoopPlayback() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto bad = Config();
    bad.renderer_id = "teebot-lesson-renderer.v4";
    auto response = renderer.Prepare(bad, 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kUnsupportedContract,
            "wrong renderer is rejected");

    fake.enough_memory = false;
    response = renderer.Prepare(Config(), 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kInsufficientPsram,
            "allocation failure is typed");
    fake.enough_memory = true;
    fake.fail_static = true;
    response = renderer.Prepare(Config(), 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kDecodeFailed,
            "static decode failure is typed");
    fake.fail_static = false;

    auto loop = Config();
    loop.playback_mode = tbot::LessonLayeredPlaybackMode::kLoop;
    Require(renderer.Prepare(loop, 0).accepted, "loop phase prepares");
    Require(renderer.Start(8, "teach", 0).accepted, "loop phase starts");
    fake.now_ms = 410;
    response = renderer.Tick(fake.now_ms);
    Require(response.accepted && response.type == tbot::LessonCinematicResponseType::kCommandApplied,
            "loop phase stays active past duration");
    Require(fake.last_frame == 1, "loop phase wraps Robot frame index");
}

void TestDiscardSessionAllowsSequenceRestart() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "first lesson prepares");
    Require(renderer.Start(8, "teach", 0).accepted, "first lesson starts");
    Require(renderer.Stop(9, "teach").accepted, "first lesson stops");

    renderer.DiscardSession();
    auto next = Config();
    next.command_sequence_id = 1;
    Require(renderer.Prepare(next, 0).accepted,
            "new lesson may restart its command sequence after session discard");
}

void TestFallbackPhaseKeepsBackgroundAndRobotWithoutTeachingObject() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto fallback = Config();
    fallback.has_teaching_object = false;
    fallback.teaching_object = {};

    const auto response = renderer.Prepare(fallback, 0);

    Require(response.accepted, "two-layer fallback phase prepares");
    Require(fake.jpeg_decodes == 1 && fake.png_decodes == 0 && fake.video_decodes == 1,
            "fallback keeps the static background and Robot without decoding an object");
    Require(fake.allocations == 3,
            "fallback allocates only background, framebuffer, and Robot scratch buffers");
}

}  // namespace

int main() {
    TestStaticLayersDecodeOnceAndRobotOwnsClock();
    TestTypedFailuresAndLoopPlayback();
    TestDiscardSessionAllowsSequenceRestart();
    TestFallbackPhaseKeepsBackgroundAndRobotWithoutTeachingObject();
    std::cout << "lesson_layered_cinematic_renderer tests passed\n";
    return 0;
}
