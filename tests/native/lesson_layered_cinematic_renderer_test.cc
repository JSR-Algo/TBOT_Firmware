#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "lesson_layered_cinematic_renderer.h"

#ifdef ESP_PLATFORM
namespace tbot {
LessonCinematicRendererOps ProductionLessonCinematicRendererOps() { return {}; }
void ConfigureProductionLessonCinematicSession(const std::string&, const std::string&,
                                               std::uint64_t) {}
bool DecodeLessonLayeredJpeg(const char*, std::uint16_t*, std::size_t, std::uint16_t*,
                            std::uint16_t*, std::size_t*) { return false; }
bool DecodeLessonLayeredPng(const char*, std::uint8_t*, std::size_t, std::uint16_t*,
                           std::uint16_t*, std::size_t*) { return false; }
}
#endif

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
    bool fail_video_decode = false;
    bool fail_present = false;
    bool enough_memory = true;
    std::vector<std::size_t> video_frames;
    std::vector<std::uint16_t> pixels;
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
    if (fake.fail_video || fake.fail_video_decode || capacity < 8) return false;
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

bool Present(void* raw, const std::uint16_t* pixels, std::uint16_t width, std::uint16_t height,
             std::size_t frame_index) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    ++fake.presents;
    fake.last_frame = frame_index;
    fake.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height);
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

void TestActivityTransitionsRetainPinnedStaticLayers() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto initial = Config();
    initial.background.identity = "background-v1";
    initial.teaching_object.identity = "object-v1";
    Require(renderer.Prepare(initial, 0).accepted, "initial pinned composition prepares");

    auto retained = initial;
    retained.command_sequence_id = 8;
    retained.phase_id = "listen";
    retained.retain_static_layers = true;
    Require(renderer.Prepare(retained, 0).accepted, "retained activity prepares");
    Require(fake.jpeg_decodes == 1 && fake.png_decodes == 1,
            "unchanged pinned static identities are not decoded twice");

    auto changed_object = retained;
    changed_object.command_sequence_id = 9;
    changed_object.teaching_object.identity = "object-v2";
    changed_object.teaching_object.sd_path = "/sd/object-v2.png";
    Require(renderer.Prepare(changed_object, 0).accepted, "changed object prepares");
    Require(fake.jpeg_decodes == 1 && fake.png_decodes == 2,
            "only the changed pinned object is decoded");

    auto changed_background = changed_object;
    changed_background.command_sequence_id = 10;
    changed_background.background.identity = "background-v2";
    changed_background.background.sd_path = "/sd/background-v2.jpg";
    Require(renderer.Prepare(changed_background, 0).accepted, "changed background prepares");
    Require(fake.jpeg_decodes == 2 && fake.png_decodes == 2,
            "only the changed pinned background is decoded");
    renderer.DiscardSession();
    Require(fake.allocations == fake.frees,
            "retained and replaced layer buffers are released exactly once");
}

void TestVisualStateDoesNotReplayEntranceUnlessRequested() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "visual state fixture prepares");
    const std::size_t entrance_decodes = fake.video_decodes;

    tbot::LessonLayeredVisualState state{};
    state.activity_id = "activity-2";
    state.phase_id = "listen";
    state.retain_static_layers = true;
    state.replay_entrance = false;
    auto response = renderer.ApplyVisualState(state, 8, 0);
    Require(response.accepted && response.phase_id == "listen",
            "activity visual state is applied without entrance");
    Require(fake.video_decodes == entrance_decodes && fake.presents == 2,
            "entrance is not replayed and retained static composition is actively presented");

    state.activity_id = "activity-3";
    state.phase_id = "teach";
    state.replay_entrance = true;
    response = renderer.ApplyVisualState(state, 9, 0);
    Require(response.accepted && fake.video_decodes == entrance_decodes + 1,
            "explicit replayEntrance replays frame zero once");
}

void TestActivityOutcomesRetainStaticLayerIdentity() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto config = Config();
    config.background.identity = "scene-week-19";
    config.teaching_object.identity = "weather-card";
    Require(renderer.Prepare(config, 0).accepted, "outcome fixture prepares static identity");
    const auto static_decodes = fake.jpeg_decodes + fake.png_decodes;
    const auto entrance_decodes = fake.video_decodes;

    tbot::LessonLayeredVisualState state{};
    state.activity_id = "w19.a06";
    state.phase_id = "listen";
    state.phase_variant = "nearMiss";
    state.retain_static_layers = true;
    Require(renderer.ApplyVisualState(state, 8, 0).accepted,
            "near-miss outcome applies over retained static layers");
    state.activity_id = "w19.a07";
    state.phase_variant = "correct";
    Require(renderer.ApplyVisualState(state, 9, 0).accepted,
            "correct outcome applies over retained static layers");
    Require(fake.jpeg_decodes + fake.png_decodes == static_decodes,
            "activity outcomes preserve background and object identity without re-decode");
    Require(fake.video_decodes == entrance_decodes,
            "activity outcomes do not replay entrance unless explicitly requested");
}

void TestVisualStateStaticPresentFailureIsTypedAndRetryable() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "static visual retry fixture prepares");
    tbot::LessonLayeredVisualState state{};
    state.activity_id = "activity-static";
    state.phase_id = "listen";
    state.retain_static_layers = true;
    fake.fail_present = true;
    auto response = renderer.ApplyVisualState(state, 8, 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kPresentFailed,
            "static activity present failure is typed");
    fake.fail_present = false;
    response = renderer.ApplyVisualState(state, 8, 0);
    Require(response.accepted, "static activity may retry after a non-applied present failure");
}

void TestRejectedVisualStatePreservesPriorControlPhase() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "control rollback fixture prepares teach");
    tbot::LessonLayeredVisualState state{};
    state.activity_id = "activity-listen";
    state.phase_id = "listen";
    state.retain_static_layers = true;
    fake.fail_present = true;
    const auto rejected = renderer.ApplyVisualState(state, 8, 0);
    Require(!rejected.accepted, "listen transition is rejected when static present fails");
    fake.fail_present = false;
    Require(renderer.Start(8, "teach", 0).accepted,
            "rejected listen transition preserves prior teach control phase");
}

void TestFailedRetainedReprepareLeavesOldCompositionUsable() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto initial = Config();
    initial.retain_static_layers = true;
    Require(renderer.Prepare(initial, 0).accepted, "transaction fixture prepares");
    fake.fail_video = true;
    auto replacement = initial;
    replacement.command_sequence_id = 8;
    replacement.phase_id = "listen";
    replacement.retain_static_layers = false;
    auto response = renderer.Prepare(replacement, 0);
    Require(!response.accepted, "failed normal replacement is rejected");
    Require(renderer.Start(8, "teach", 0).accepted,
            "failed replacement preserves the prior stream, phase, and composition");
}

void TestRobotFailureKeepsLastGoodStaticCompositionDegraded() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "last good composition prepares");
    const std::size_t static_presents = fake.presents;
    fake.fail_video = true;

    tbot::LessonLayeredVisualState state{};
    state.activity_id = "activity-degraded";
    state.phase_id = "celebrate";
    state.retain_static_layers = true;
    state.replay_entrance = true;
    const auto response = renderer.ApplyVisualState(state, 8, 0);

    Require(response.accepted && renderer.last_apply_degraded(),
            "Robot animation failure is accepted as degraded visual state");
    Require(fake.presents == static_presents + 1,
            "Robot failure presents the retained static background and object");
    Require(fake.jpeg_decodes == 1 && fake.png_decodes == 1,
            "degraded animation does not re-decode static layers");
}

void TestFirstCoursePrepareKeepsNewStaticWhenRobotFails() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto config = Config();
    config.retain_static_layers = true;
    fake.fail_video = true;

    const auto response = renderer.Prepare(config, 0);

    Require(response.accepted && renderer.last_apply_degraded(),
            "first Course Mode prepare degrades when Robot cannot open");
    Require(renderer.prepared() && fake.jpeg_decodes == 1 && fake.png_decodes == 1 &&
                fake.presents == 1,
            "newly decoded static composition remains prepared and visible");
    const auto start = renderer.Start(8, "teach", 0);
    Require(!start.accepted && start.error == tbot::LessonCinematicError::kFileOpen &&
                renderer.last_apply_degraded(),
            "static-only degraded phase cannot acknowledge animation readiness");
    const auto tick = renderer.Tick(100);
    Require(!tick.accepted && tick.error == tbot::LessonCinematicError::kFileOpen,
            "static-only degraded phase cannot acknowledge animation completion");

    FakeRuntime decode_fake;
    tbot::LessonLayeredCinematicRenderer decode_renderer(Ops(&decode_fake));
    decode_fake.fail_video_decode = true;
    const auto decode_response = decode_renderer.Prepare(config, 0);
    Require(decode_response.accepted && decode_renderer.last_apply_degraded() &&
                decode_renderer.prepared() && decode_fake.presents == 1,
            "first frame-zero decode failure also retains the newly decoded static composition");
}

void TestTickRobotFailureFallsBackToStaticComposition() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "tick failure fixture prepares");
    Require(renderer.Start(8, "teach", 0).accepted, "tick failure fixture starts");
    const std::size_t presents_before = fake.presents;
    fake.fail_video_decode = true;

    const auto response = renderer.Tick(110);

    Require(!response.accepted && response.error == tbot::LessonCinematicError::kDecodeFailed &&
                renderer.last_apply_degraded(),
            "later Robot frame failure returns an explicit error while retaining degraded state");
    Require(renderer.prepared() && fake.presents == presents_before + 1,
            "later Robot frame failure returns safely to retained static composition");
    const auto repeated = renderer.Tick(220);
    Require(!repeated.accepted && repeated.error == tbot::LessonCinematicError::kDecodeFailed,
            "subsequent timer ticks preserve the same observable degraded failure");
}

void TestTickFailsWhenStaticFallbackCannotPresent() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "static present failure fixture prepares");
    Require(renderer.Start(8, "teach", 0).accepted, "static present failure fixture starts");
    fake.fail_video_decode = true;
    fake.fail_present = true;

    const auto response = renderer.Tick(110);

    Require(!response.accepted && response.error == tbot::LessonCinematicError::kPresentFailed,
            "failed static fallback never advertises degraded-safe success");
    Require(!renderer.prepared(), "failed static fallback leaves a coherent failed state");
}

void TestLateOnceTickPresentsFinalFrameBeforeCompletion() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "late once fixture prepares");
    Require(renderer.Start(8, "teach", 0).accepted, "late once fixture starts");
    const auto complete = renderer.Tick(350);
    Require(complete.accepted && complete.type == tbot::LessonCinematicResponseType::kPhaseComplete,
            "late once tick completes");
    Require(fake.last_frame == 2 && fake.video_frames == std::vector<std::size_t>({0, 2}),
            "late once completion presents final pixels without queuing skipped frames");
}

void TestFinalFrameFailureCannotComplete() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "final failure fixture prepares");
    Require(renderer.Start(8, "teach", 0).accepted, "final failure fixture starts");
    fake.fail_video_decode = true;
    const auto complete = renderer.Tick(350);
    Require(!complete.accepted && complete.error == tbot::LessonCinematicError::kDecodeFailed &&
                renderer.last_apply_degraded(),
            "failed final decode reports degradation instead of animation completion");
    Require(fake.video_closes == 1 && fake.frees == 1,
            "failed final decode releases stream and robot scratch");
    Require(!renderer.Start(9, "teach", 400).accepted,
            "restarting degraded final frame cannot erase failure");
}

void TestStaticVisualAfterDecodeFailureCannotStartAnimation() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "static transition fixture prepares");
    Require(renderer.Start(8, "teach", 0).accepted, "static transition fixture starts");
    fake.fail_video_decode = true;
    Require(!renderer.Tick(110).accepted, "robot decoding fails before static transition");
    tbot::LessonLayeredVisualState visual{};
    visual.activity_id = "next-activity";
    visual.phase_id = "listen";
    Require(renderer.ApplyVisualState(visual, 9, 110).accepted,
            "static activity presentation remains accepted without animation");
    const auto start = renderer.Start(10, "listen", 120);
    Require(!start.accepted && start.error == tbot::LessonCinematicError::kDecodeFailed &&
                renderer.last_apply_degraded(),
            "static activity cannot restart a released animation stream");
}

void TestRuntimeFailureRetainsWorkerDiagnostic() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted && renderer.Start(8, "teach", 0).accepted,
            "runtime diagnostic fixture starts");
    fake.fail_video_decode = true;
    Require(!renderer.Tick(110).accepted, "runtime diagnostic fixture fails decode");
    Require(renderer.PendingRuntimeError().has_value(),
            "asynchronous renderer failure remains available for lesson worker reporting");
    const auto failure = *renderer.PendingRuntimeError();
    Require(failure.generation == renderer.RuntimeGeneration() &&
                failure.command_sequence_id == 8 && failure.phase_id == "teach" &&
                failure.error == tbot::LessonCinematicError::kDecodeFailed,
            "worker diagnostic owns exact playback identity and typed error");
    Require(!renderer.AcknowledgeRuntimeError(failure.generation + 1, 8) &&
                !renderer.AcknowledgeRuntimeError(failure.generation, 9),
            "stale diagnostic acknowledgement cannot consume current failure");
    Require(!renderer.Tick(220).accepted && renderer.PendingRuntimeError().has_value(),
            "failed playback retains one pending error over repeated ticks");
    Require(!renderer.ReleaseFailedRuntimeResources(failure.generation + 1, 8) &&
                !renderer.ReleaseFailedRuntimeResources(failure.generation, 9),
            "stale cleanup cannot release current runtime resources");
    Require(renderer.ReleaseFailedRuntimeResources(failure.generation, 8) &&
                renderer.ReleaseFailedRuntimeResources(failure.generation, 8) &&
                fake.allocations == fake.frees && fake.video_opens == fake.video_closes &&
                renderer.RuntimeGeneration() == failure.generation && renderer.PendingRuntimeError().has_value(),
            "failure cleanup releases once while preserving exact pending terminal identity");
    Require(renderer.Start(8, "teach", 240).accepted && !renderer.Start(9, "teach", 240).accepted,
            "failed runtime keeps prior ACK replay but rejects fresh playback");
    Require(renderer.AcknowledgeRuntimeError(failure.generation, 8) &&
                !renderer.PendingRuntimeError().has_value(),
            "successful worker handoff consumes one exact diagnostic");
    renderer.Tick(330);
    Require(!renderer.PendingRuntimeError().has_value(),
            "repeated degraded ticks do not regenerate a reported error");
    Require(renderer.Stop(9, "teach").accepted && fake.allocations == fake.frees,
            "real later terminal command accepts original phase without duplicate cleanup");
}

void TestRuntimeDiagnosticFencesReplacementAndDiscard() {
    for (int replacement = 0; replacement < 4; ++replacement) {
        FakeRuntime fake;
        tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
        Require(renderer.Prepare(Config(), 0).accepted && renderer.Start(8, "teach", 0).accepted,
                "diagnostic replacement fixture starts");
        fake.fail_video_decode = true;
        fake.fail_present = replacement == 3;
        renderer.Tick(110);
        const auto failure = renderer.PendingRuntimeError();
        Require(failure.has_value(), "replacement fixture captures error");
        if (replacement == 0) {
            auto invalid = Config();
            invalid.command_sequence_id = 9;
            invalid.background.sd_path = "invalid";
            Require(!renderer.Prepare(invalid, 0).accepted && renderer.PendingRuntimeError().has_value(),
                    "failed replacement preserves pending old diagnostic");
            fake.fail_video_decode = false;
            auto next = Config();
            next.command_sequence_id = 9;
            Require(renderer.Prepare(next, 0).accepted, "successful replacement prepares");
        } else if (replacement == 1) {
            tbot::LessonLayeredVisualState visual{};
            visual.activity_id = "replacement";
            visual.phase_id = "listen";
            Require(renderer.ApplyVisualState(visual, 9, 120).accepted,
                    "accepted activity replaces diagnostic identity");
        } else if (replacement == 2) {
            Require(renderer.Cancel(9, "teach").accepted, "cancel clears pending error");
        } else {
            Require(failure->error == tbot::LessonCinematicError::kPresentFailed,
                    "failed static fallback retains presentation error");
            renderer.DiscardSession();
        }
        Require(renderer.RuntimeGeneration() != failure->generation &&
                    !renderer.PendingRuntimeError().has_value(),
                "replacement, activity, cancellation and discard fence old diagnostics");
    }
}

void TestGenerationGuardSerializesReplacementPrepare() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "generation guard fixture prepares");
    const auto generation = renderer.RuntimeGeneration();
    auto next = Config();
    next.command_sequence_id = 9;
    std::promise<void> attempting;
    std::promise<bool> completed;
    auto attempting_future = attempting.get_future();
    auto completed_future = completed.get_future();
    std::thread replacement;
    Require(renderer.WithRuntimeGeneration(generation, [&]() {
        replacement = std::thread([&]() {
            attempting.set_value();
            completed.set_value(renderer.Prepare(next, 0).accepted);
        });
        attempting_future.wait();
        Require(completed_future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
                "concurrent replacement must wait until generation-guarded display cleanup exits");
    }), "current generation executes its cleanup");
    replacement.join();
    Require(completed_future.get() && renderer.RuntimeGeneration() != generation,
            "replacement prepares after guarded cleanup releases its lock");
    bool stale_called = false;
    Require(!renderer.WithRuntimeGeneration(generation, [&]() { stale_called = true; }) && !stale_called,
            "late old-generation cleanup cannot mutate the replacement display");
}

void TestRobotPathOutlivesPrepareCaller() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    {
        std::string path = "/sd/" + std::string(200, 'r') + ".mp4";
        auto config = Config();
        config.robot.sd_path = path.c_str();
        Require(renderer.Prepare(config, 0).accepted, "temporary path prepares");
    }
    Require(renderer.Start(8, "teach", 0).accepted, "temporary path starts");
    Require(renderer.Tick(110).accepted && fake.last_frame == 1,
            "deferred frame diagnostics do not borrow the destroyed prepare path");
}

void TestThreeLayerPixelsSurviveFramesAndClipReplacement() {
    FakeRuntime fake;
    tbot::LessonLayeredCinematicRenderer renderer(Ops(&fake));
    auto config = Config();
    config.retain_static_layers = true;
    config.robot.rect = {10, 10, 2, 2};
    Require(renderer.Prepare(config, 0).accepted, "pixel fixture prepares");
    const auto initial = fake.pixels;
    Require(initial[0] == 0x001f, "background stays blue outside foregrounds");
    Require(initial[10 * 480 + 10] == 0xf800, "robot opaque red covers object");
    Require(initial[10 * 480 + 11] == 0x040f,
            "robot chroma reveals half-alpha object over blue background");
    Require(initial[11 * 480 + 10] == 0xffff,
            "robot opaque white covers transparent object");
    Require(renderer.Start(8, "teach", 0).accepted && renderer.Tick(110).accepted,
            "pixel fixture advances");
    Require(fake.pixels == initial, "all layer pixels persist across robot frame");
    config.command_sequence_id = 9;
    config.phase_id = "listen";
    config.robot.rect = {20, 20, 2, 2};
    Require(renderer.Prepare(config, 110).accepted, "replacement clip prepares");
    Require(fake.jpeg_decodes == 1 && fake.png_decodes == 1,
            "replacement clip reuses both static decodes");
    Require(fake.pixels[10 * 480 + 10] == 0xf800 &&
                fake.pixels[10 * 480 + 11] == 0x040f &&
                fake.pixels[11 * 480 + 10] == 0x001f &&
                fake.pixels[11 * 480 + 11] == 0xffff,
            "clip replacement retains opaque partial-alpha transparent object pixels");
    Require(renderer.Cancel(10, "listen").accepted && fake.allocations == fake.frees,
            "pixel lifecycle releases buffers exactly once on cancel");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    if (selected == "late-final") { TestLateOnceTickPresentsFinalFrameBeforeCompletion(); return 0; }
    if (selected == "degraded") { TestFirstCoursePrepareKeepsNewStaticWhenRobotFails(); return 0; }
    if (selected == "path") { TestRobotPathOutlivesPrepareCaller(); return 0; }
    if (selected == "static-transition") {
        TestStaticVisualAfterDecodeFailureCannotStartAnimation(); return 0;
    }
    if (selected == "runtime-error") { TestRuntimeFailureRetainsWorkerDiagnostic(); return 0; }
    Require(selected == "all", "unknown test selection must not silently run another scope");
    TestStaticLayersDecodeOnceAndRobotOwnsClock();
    TestTypedFailuresAndLoopPlayback();
    TestDiscardSessionAllowsSequenceRestart();
    TestFallbackPhaseKeepsBackgroundAndRobotWithoutTeachingObject();
    TestActivityTransitionsRetainPinnedStaticLayers();
    TestVisualStateDoesNotReplayEntranceUnlessRequested();
    TestActivityOutcomesRetainStaticLayerIdentity();
    TestVisualStateStaticPresentFailureIsTypedAndRetryable();
    TestRejectedVisualStatePreservesPriorControlPhase();
    TestFailedRetainedReprepareLeavesOldCompositionUsable();
    TestRobotFailureKeepsLastGoodStaticCompositionDegraded();
    TestFirstCoursePrepareKeepsNewStaticWhenRobotFails();
    TestTickRobotFailureFallsBackToStaticComposition();
    TestTickFailsWhenStaticFallbackCannotPresent();
    TestLateOnceTickPresentsFinalFrameBeforeCompletion();
    TestFinalFrameFailureCannotComplete();
    TestRobotPathOutlivesPrepareCaller();
    TestThreeLayerPixelsSurviveFramesAndClipReplacement();
    TestStaticVisualAfterDecodeFailureCannotStartAnimation();
    TestRuntimeFailureRetainsWorkerDiagnostic();
    TestRuntimeDiagnosticFencesReplacementAndDiscard();
    TestGenerationGuardSerializesReplacementPrepare();
    std::cout << "lesson_layered_cinematic_renderer tests: 22 passed, 0 failed, 0 skipped\n";
    return 0;
}
