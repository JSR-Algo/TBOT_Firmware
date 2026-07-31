#include "lesson_flattened_cinematic_renderer.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson_flattened_cinematic_renderer test failed: " << message << '\n';
        std::exit(1);
    }
}

struct FakeRuntime {
    std::uint64_t now_ms = 0;
    std::vector<std::size_t> allocation_sizes;
    std::size_t frees = 0;
    std::size_t opens = 0;
    std::size_t closes = 0;
    std::size_t presents = 0;
    std::size_t last_frame = 999;
    bool enough_memory = true;
    bool fail_open = false;
    bool fail_decode = false;
    bool fail_present = false;
    tbot::LessonCinematicError operation_error = tbot::LessonCinematicError::kNone;
    std::vector<std::size_t> decoded_indices;
};

void* Allocate(void* raw, std::size_t size) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (!fake.enough_memory) return nullptr;
    fake.allocation_sizes.push_back(size);
    return std::malloc(size);
}

void Free(void* raw, void* pointer) {
    ++static_cast<FakeRuntime*>(raw)->frees;
    std::free(pointer);
}

bool Open(void* raw, const char* path, tbot::LessonCinematicStreamMetadata* metadata,
          void** handle) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (fake.fail_open || path == nullptr || std::strstr(path, "missing") != nullptr) return false;
    ++fake.opens;
    *metadata = {480, 320, 10, 3, 300, 64};
    *handle = reinterpret_cast<void*>(fake.opens);
    return true;
}

void Close(void* raw, void*) { ++static_cast<FakeRuntime*>(raw)->closes; }

bool Decode(void* raw, void*, std::size_t index, std::uint8_t* destination,
            std::size_t capacity, std::uint16_t* width, std::uint16_t* height,
            std::size_t* stride) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (fake.fail_decode) return false;
    Require(capacity == 480u * 320u * 2u, "decode targets only the full RGB565 framebuffer");
    fake.decoded_indices.push_back(index);
    *width = 480;
    *height = 320;
    *stride = 480 * 2;
    std::memset(destination, static_cast<int>(index), capacity);
    return true;
}

bool Present(void* raw, const std::uint16_t*, std::uint16_t width, std::uint16_t height,
             std::size_t frame_index) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    Require(width == 480 && height == 320, "present receives one flattened full-screen frame");
    if (fake.fail_present) return false;
    ++fake.presents;
    fake.last_frame = frame_index;
    return true;
}

tbot::LessonCinematicError LastError(void* raw) {
    return static_cast<FakeRuntime*>(raw)->operation_error;
}

std::uint64_t MonotonicMs(void* raw) { return static_cast<FakeRuntime*>(raw)->now_ms; }

tbot::LessonCinematicRendererOps Ops(FakeRuntime* fake) {
    return {fake, Allocate, Free, Open, Close, Decode, Present, LastError, MonotonicMs};
}

tbot::LessonFlattenedCinematicPhaseConfig Config() {
    tbot::LessonFlattenedCinematicPhaseConfig config{};
    config.renderer_id = "teebot-lesson-renderer.v4";
    config.template_id = "flattenedMjpegCinematic";
    config.template_version = 1;
    config.phase_id = "opening";
    config.command_sequence_id = 41;
    config.duration_ms = 300;
    config.fps = 10;
    config.frame_count = 3;
    config.asset.derivative_id = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    config.asset.phase_id = "opening";
    config.asset.sd_path = "/sdcard/tbot/lesson-assets/flattenedCinematic.opening";
    config.asset.sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    config.asset.bytes = 1234;
    config.asset.media_type = "video/mp4";
    config.asset.width = 480;
    config.asset.height = 320;
    return config;
}

void TestExactContractAllocationTimingAndLifecycle() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    auto response = renderer.Prepare(Config(), 0);
    Require(response.accepted && response.type == tbot::LessonCinematicResponseType::kFrameZeroReady,
            "exact v4 prepare presents frame zero");
    Require(fake.opens == 1 && fake.presents == 1 && fake.last_frame == 0,
            "prepare opens and presents exactly one flattened stream");
    Require(fake.allocation_sizes == std::vector<std::size_t>{480u * 320u * 2u},
            "renderer allocates one framebuffer and no foreground/chroma scratch");

    Require(renderer.Start(42, "opening", 0).accepted, "start accepts prepared phase");
    fake.now_ms = 210;
    Require(renderer.Tick(fake.now_ms).accepted && fake.last_frame == 2,
            "deadline miss deterministically drops to the target whole frame");
    Require(fake.decoded_indices == std::vector<std::size_t>({0, 2}),
            "one target frame maps to one MP4 sample");

    Require(renderer.Pause(43, "opening", 210).accepted, "pause applies");
    fake.now_ms = 1000;
    renderer.Tick(fake.now_ms);
    Require(fake.last_frame == 2, "pause freezes playback clock");
    Require(renderer.Resume(44, "opening", 1000).accepted, "resume applies");
    fake.now_ms = 1089;
    renderer.Tick(fake.now_ms);
    Require(fake.last_frame == 2, "resume rebases playback clock");

    response = renderer.Tick(1200);
    Require(response.accepted && response.type == tbot::LessonCinematicResponseType::kPhaseComplete &&
                renderer.prepared(),
            "completion returns renderer to prepared for server-owned lifecycle");
    Require(renderer.Start(45, "opening", 1200).accepted, "completed phase can replay");
    Require(renderer.Cancel(46, "opening").accepted && fake.closes == 1 && fake.frees == 1,
            "cancel closes the file before releasing its single framebuffer");
}

void TestValidationErrorsIdempotencyAndCleanup() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    auto invalid = Config();
    invalid.renderer_id = "teebot-lesson-renderer.v3";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kUnsupportedContract,
            "v3/v4 identity confusion is rejected");
    invalid = Config();
    invalid.asset.sd_path = "https://cdn.example/opening.mp4";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kInvalidPath,
            "URL asset is rejected");
    invalid.asset.sd_path = "/sdcard/tbot/../secret.mp4";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kInvalidPath,
            "path traversal is rejected");
    invalid = Config();
    invalid.asset.derivative_id =
        "Dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kMetadataMismatch,
            "derivative identity must be exact lowercase SHA-256");

    fake.fail_open = true;
    fake.operation_error = tbot::LessonCinematicError::kParserFailed;
    Require(renderer.Prepare(Config(), 0).error == tbot::LessonCinematicError::kParserFailed,
            "MP4 parser failure remains distinct from file-open failure");
    fake.operation_error = tbot::LessonCinematicError::kSessionMismatch;
    Require(renderer.Prepare(Config(), 0).error == tbot::LessonCinematicError::kSessionMismatch,
            "SD lease/session failure remains distinctly typed");
    fake.fail_open = false;
    fake.operation_error = tbot::LessonCinematicError::kNone;

    fake.enough_memory = false;
    Require(renderer.Prepare(Config(), 0).error == tbot::LessonCinematicError::kInsufficientPsram,
            "framebuffer allocation failure is typed");
    fake.enough_memory = true;
    Require(renderer.Prepare(Config(), 0).accepted, "valid prepare succeeds after an error");
    const auto presents = fake.presents;
    Require(renderer.Prepare(Config(), 0).accepted && fake.presents == presents,
            "identical prepare command replays without work");
    auto changed = Config();
    changed.asset.bytes++;
    Require(renderer.Prepare(changed, 0).error == tbot::LessonCinematicError::kStaleCommand,
            "same sequence with changed asset identity is rejected");
    Require(renderer.Start(42, "opening", 0).accepted, "prepared stream starts for error cleanup");
    fake.fail_decode = true;
    Require(renderer.Tick(100).error == tbot::LessonCinematicError::kDecodeFailed &&
                fake.opens == fake.closes && fake.allocation_sizes.size() == fake.frees,
            "decode error closes the stream and releases buffers before lease release");
    fake.fail_decode = false;
    auto retry = Config();
    retry.command_sequence_id = 44;
    Require(renderer.Prepare(retry, 0).accepted, "renderer can prepare after cleaned failure");
    Require(renderer.Stop(45, "opening").accepted && fake.opens == fake.closes &&
                fake.allocation_sizes.size() == fake.frees,
            "stop cleans every open file and allocation");
}

void TestRepeatedPreparePlayCancelIsLeakFree() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    for (std::uint64_t cycle = 0; cycle < 20; ++cycle) {
        auto config = Config();
        config.command_sequence_id = 100 + cycle * 3;
        Require(renderer.Prepare(config, 0).accepted, "cycle prepares");
        Require(renderer.Start(config.command_sequence_id + 1, "opening", 0).accepted,
                "cycle starts");
        Require(renderer.Cancel(config.command_sequence_id + 2, "opening").accepted,
                "cycle cancels");
    }
    Require(fake.opens == fake.closes && fake.allocation_sizes.size() == fake.frees,
            "repeated phase cycles leak neither files nor buffers");
}

}  // namespace

int main() {
    TestExactContractAllocationTimingAndLifecycle();
    TestValidationErrorsIdempotencyAndCleanup();
    TestRepeatedPreparePlayCancelIsLeakFree();
    std::cout << "lesson_flattened_cinematic_renderer tests passed\n";
    return 0;
}
