#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "lesson_cinematic_renderer.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson_cinematic_renderer test failed: " << message << '\n';
        std::exit(1);
    }
}

struct FakeRuntime {
    std::uint64_t now_ms = 0;
    std::size_t allocations = 0;
    std::size_t frees = 0;
    std::size_t opens = 0;
    std::size_t closes = 0;
    std::size_t presents = 0;
    std::size_t last_frame = 999;
    bool fail_decode = false;
    bool enough_memory = true;
    std::vector<std::size_t> decoded_indices;
};

void* Allocate(void* raw, std::size_t size) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (!fake.enough_memory) return nullptr;
    ++fake.allocations;
    return std::malloc(size);
}

void Free(void* raw, void* ptr) {
    ++static_cast<FakeRuntime*>(raw)->frees;
    std::free(ptr);
}

bool Open(void* raw, const char* path, tbot::LessonCinematicStreamMetadata* metadata,
          void** handle) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (path == nullptr || std::strstr(path, "missing") != nullptr) return false;
    ++fake.opens;
    const bool background = std::strstr(path, "background") != nullptr;
    *metadata = {static_cast<std::uint16_t>(background ? 480u : 2u),
                 static_cast<std::uint16_t>(background ? 320u : 2u),
                 10u, 3u, 300u, 16u};
    *handle = reinterpret_cast<void*>(fake.opens);
    return true;
}

void Close(void* raw, void*) {
    ++static_cast<FakeRuntime*>(raw)->closes;
}

bool Decode(void* raw, void*, std::size_t index, std::uint8_t* destination,
            std::size_t capacity, std::uint16_t* width, std::uint16_t* height,
            std::size_t* stride) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (fake.fail_decode) return false;
    fake.decoded_indices.push_back(index);
    const bool background = capacity >= 480u * 320u * 2u;
    *width = background ? 480 : 2;
    *height = background ? 320 : 2;
    *stride = static_cast<std::size_t>(*width) * 2;
    std::memset(destination, background ? static_cast<int>(index) : 255,
                static_cast<std::size_t>(*height) * *stride);
    return true;
}

bool Present(void* raw, const std::uint16_t*, std::uint16_t width, std::uint16_t height,
             std::size_t frame_index) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    Require(width == 480 && height == 320, "presentation is one full framebuffer");
    ++fake.presents;
    fake.last_frame = frame_index;
    return true;
}

tbot::LessonCinematicRendererOps Ops(FakeRuntime* fake) {
    return {fake, Allocate, Free, Open, Close, Decode, Present};
}

tbot::LessonCinematicPhaseConfig Config() {
    tbot::LessonCinematicPhaseConfig config{};
    config.renderer_id = "teebot-lesson-renderer.v3";
    config.template_id = "directMp4Cinematic";
    config.phase_id = "teach";
    config.command_sequence_id = 7;
    config.layers[0].sd_path = "/sd/background.mp4";
    config.layers[1].sd_path = "/sd/object.mp4";
    config.layers[2].sd_path = "/sd/robot.mp4";
    config.layers[1].rect = {0, 0, 2, 2};
    config.layers[2].rect = {0, 0, 2, 2};
    config.layers[1].chroma = {{0, 255, 0}, 20, 1};
    config.layers[2].chroma = {{0, 255, 0}, 20, 1};
    return config;
}

void TestPrepareStartClockPauseResumeAndNoSteadyAllocations() {
    FakeRuntime fake;
    tbot::LessonCinematicRenderer renderer(Ops(&fake));
    auto response = renderer.Prepare(Config(), fake.now_ms);
    Require(response.type == tbot::LessonCinematicResponseType::kFrameZeroReady &&
                response.accepted && response.command_sequence_id == 7 &&
                std::string(response.phase_id) == "teach",
            "prepare decodes and presents frame zero before frameZeroReady");
    Require(fake.opens == 3 && fake.presents == 1 && fake.last_frame == 0,
            "prepare opens exactly three streams and presents frame zero");
    Require(fake.allocations == 2,
            "prepare allocates one framebuffer and one foreground scratch, not a second full frame");

    response = renderer.Start(8, "teach", fake.now_ms);
    Require(response.type == tbot::LessonCinematicResponseType::kPhaseReady && response.accepted,
            "start releases the shared clock and reports phaseReady");
    const auto allocations_after_prepare = fake.allocations;
    fake.now_ms = 210;
    Require(renderer.Tick(fake.now_ms).accepted && fake.last_frame == 2,
            "integer clock advances the whole triplet to one index");
    Require(fake.decoded_indices.size() >= 6 &&
                fake.decoded_indices[fake.decoded_indices.size() - 1] == 2 &&
                fake.decoded_indices[fake.decoded_indices.size() - 2] == 2 &&
                fake.decoded_indices[fake.decoded_indices.size() - 3] == 2,
            "all three decodes use the same frame index");
    Require(fake.allocations == allocations_after_prepare, "frame loop performs no allocations");

    response = renderer.Pause(9, "teach", fake.now_ms);
    Require(response.type == tbot::LessonCinematicResponseType::kCommandApplied && response.accepted,
            "pause reports commandApplied");
    fake.now_ms = 1000;
    Require(renderer.Tick(fake.now_ms).accepted && fake.last_frame == 2,
            "pause freezes clock and frame");
    response = renderer.Resume(10, "teach", fake.now_ms);
    Require(response.accepted, "resume applies");
    fake.now_ms = 1090;
    renderer.Tick(fake.now_ms);
    Require(fake.last_frame == 2, "resume rebases time without a frame jump");
}

void TestIdempotencyValidationFailureAndSafeStop() {
    FakeRuntime fake;
    tbot::LessonCinematicRenderer renderer(Ops(&fake));
    auto bad = Config();
    bad.renderer_id = "teebot-lesson-renderer.v2";
    auto response = renderer.Prepare(bad, 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kUnsupportedContract,
            "prepare rejects unsupported renderer/template identity");

    auto missing = Config();
    missing.layers[1].sd_path = "/sd/missing-object.mp4";
    response = renderer.Prepare(missing, 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kFileOpen,
            "prepare returns typed missing-file failure");
    Require(fake.closes == 1, "partial prepare closes opened streams");

    fake.enough_memory = false;
    response = renderer.Prepare(Config(), 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kInsufficientPsram,
            "prepare returns typed PSRAM failure");
    fake.enough_memory = true;
    response = renderer.Prepare(Config(), 0);
    Require(response.accepted, "valid retry prepares");
    const auto present_count = fake.presents;
    auto duplicate = renderer.Prepare(Config(), 0);
    Require(duplicate.accepted && fake.presents == present_count,
            "duplicate command sequence is idempotent");

    renderer.Start(8, "teach", 0);
    fake.fail_decode = true;
    response = renderer.Tick(100);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kDecodeFailed,
            "decode failure is typed and stops presentation");
    response = renderer.Stop(11, "teach");
    Require(response.accepted && fake.closes == 4,
            "stop halts and closes all three current files before caller releases SD lease");
    const auto closes = fake.closes;
    duplicate = renderer.Stop(11, "teach");
    Require(duplicate.accepted && fake.closes == closes, "duplicate stop is idempotent");
    response = renderer.Resume(10, "old-phase", 0);
    Require(!response.accepted && response.error == tbot::LessonCinematicError::kStaleCommand,
            "stale phase/sequence is ACK-safe and rejected");
}

}  // namespace

int main() {
    TestPrepareStartClockPauseResumeAndNoSteadyAllocations();
    TestIdempotencyValidationFailureAndSafeStop();
    std::cout << "lesson_cinematic_renderer test passed\n";
    return 0;
}
