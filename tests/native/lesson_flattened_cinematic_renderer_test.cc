#include "lesson_flattened_cinematic_renderer.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>

std::size_t g_host_heap_allocations = 0;

void* operator new(std::size_t size) {
    ++g_host_heap_allocations;
    if (void* pointer = std::malloc(size)) return pointer;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    ++g_host_heap_allocations;
    if (void* pointer = std::malloc(size)) return pointer;
    throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

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
    bool metadata_mismatch = false;
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
    std::uint32_t frame_count = 3;
    std::uint32_t duration_ms = 300;
    if (std::strstr(path, "barn-listen") != nullptr ||
        std::strstr(path, "barn-thinking") != nullptr) {
        frame_count = 13;
        duration_ms = 1300;
    } else if (std::strstr(path, "barn-correct") != nullptr) {
        frame_count = 6;
        duration_ms = 600;
    }
    *metadata = {480, 320, 10,
                 static_cast<std::uint32_t>(fake.metadata_mismatch ? 2 : frame_count),
                 duration_ms, 64};
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

tbot::LessonFlattenedCinematicPhaseConfig V2Config(
    const char* cue_id = "barn-listen", const char* effect = "listen",
    tbot::LessonCinematicPlaybackMode playback_mode =
        tbot::LessonCinematicPlaybackMode::kLoop) {
    auto config = Config();
    config.template_version = 2;
    config.phase_id = nullptr;
    config.cue_id = cue_id;
    config.effect = effect;
    config.step_key = "barn";
    config.playback_mode = playback_mode;
    config.duration_ms = 1300;
    config.frame_count = 13;
    config.asset.phase_id = nullptr;
    config.asset.cue_id = cue_id;
    config.asset.sd_path = "/sdcard/tbot/lesson-assets/flattenedCinematic.barn-listen";
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
    invalid = Config();
    invalid.phase_id = "barn-listen";
    invalid.asset.phase_id = "barn-listen";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kMetadataMismatch,
            "v1 remains restricted to its exact legacy phase allowlist");

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

void TestFailedRepreparePreservesPreparedStreamTransactionally() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted, "baseline stream prepares");
    Require(fake.opens == 1 && fake.closes == 0, "baseline stream remains open");

    auto retry = Config();
    retry.command_sequence_id = 50;
    fake.enough_memory = false;
    Require(renderer.Prepare(retry, 0).error == tbot::LessonCinematicError::kInsufficientPsram &&
                renderer.prepared() && fake.opens == 1 && fake.closes == 0,
            "allocation failure preserves the old prepared stream");
    fake.enough_memory = true;

    retry.command_sequence_id = 51;
    fake.fail_open = true;
    Require(renderer.Prepare(retry, 0).error == tbot::LessonCinematicError::kFileOpen &&
                renderer.prepared() && fake.opens == 1 && fake.closes == 0,
            "open failure preserves the old prepared stream");
    fake.fail_open = false;

    retry.command_sequence_id = 52;
    fake.metadata_mismatch = true;
    Require(renderer.Prepare(retry, 0).error == tbot::LessonCinematicError::kMetadataMismatch &&
                renderer.prepared() && fake.opens == 2 && fake.closes == 1,
            "metadata failure closes only the staged stream");
    fake.metadata_mismatch = false;

    Require(renderer.Start(53, "opening", 0).accepted,
            "old prepared stream remains playable after failed reparations");
    fake.now_ms = 100;
    Require(renderer.Tick(fake.now_ms).accepted && fake.last_frame == 1,
            "old stream continues decoding after failed reparations");
    Require(renderer.Cancel(54, "opening").accepted && fake.opens == fake.closes &&
                fake.allocation_sizes.size() == fake.frees,
            "transactional reprepare failures leak no staged or active resources");
}

void TestTemplateV2OnceCompletesAtEof() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    auto config = V2Config("barn-correct", "correct",
                           tbot::LessonCinematicPlaybackMode::kOnce);
    config.duration_ms = 600;
    config.frame_count = 6;
    config.asset.sd_path = "/sdcard/tbot/lesson-assets/flattenedCinematic.barn-correct";
    const auto prepared = renderer.Prepare(config, 0);
    Require(prepared.accepted && prepared.phase_id.empty() &&
                prepared.cue_id == "barn-correct",
            "v2 once cue prepares without relabeling cue identity as a phase");
    Require(renderer.Start(42, "barn-correct", 0).accepted, "v2 once cue starts by cue ID");
    const auto response = renderer.Tick(600);
    Require(response.accepted &&
                response.type == tbot::LessonCinematicResponseType::kPhaseComplete,
            "v2 once cue completes at EOF");
}

void TestTemplateV2LoopCrossesSeamsWithoutResourceChurn() {
    FakeRuntime fake;
    fake.decoded_indices.reserve(2005);
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(V2Config(), 0).accepted, "v2 loop cue prepares");
    Require(renderer.Start(42, "barn-listen", 0).accepted, "v2 loop cue starts by cue ID");
    Require(renderer.Tick(1299).accepted && fake.last_frame == 12,
            "loop renders the final frame before its seam");
    Require(renderer.Tick(1300).accepted && fake.last_frame == 0,
            "loop maps its seam to frame zero");
    const auto heap_allocations_before_ticks = g_host_heap_allocations;
    for (std::uint64_t seam = 1; seam <= 1000; ++seam) {
        const auto response = renderer.Tick(seam * 1300 + 100);
        Require(response.accepted &&
                    response.type == tbot::LessonCinematicResponseType::kCommandApplied &&
                    fake.last_frame == 1,
                "loop remains running across every seam");
    }
    Require(fake.opens == 1 && fake.allocation_sizes.size() == 1 &&
                fake.closes == 0 && fake.frees == 0,
            "loop seams do not reopen, close, allocate, or free resources");
    Require(g_host_heap_allocations == heap_allocations_before_ticks,
            "loop Tick performs no global heap allocation across 1,000 seams");
}

void TestTemplateV2LoopPauseResumePreservesPhase() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(V2Config(), 0).accepted, "v2 loop cue prepares for pause");
    Require(renderer.Start(42, "barn-listen", 0).accepted, "v2 loop cue starts for pause");
    Require(renderer.Tick(1200).accepted && fake.last_frame == 12,
            "loop advances before pause");
    Require(renderer.Pause(43, "barn-listen", 1200).accepted, "v2 loop pauses");
    Require(renderer.Tick(5000).accepted && fake.last_frame == 12,
            "paused loop does not advance");
    Require(renderer.Resume(44, "barn-listen", 5000).accepted, "v2 loop resumes");
    Require(renderer.Tick(5099).accepted && fake.last_frame == 12,
            "resume preserves the pre-pause loop phase");
    Require(renderer.Tick(5100).accepted && fake.last_frame == 0,
            "resume reaches the same seam after the remaining interval");
}

void TestTemplateV2IdentityFencingAndTransactionalReplacement() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(V2Config(), 0).accepted, "first v2 cue prepares");

    auto stale = V2Config("barn-thinking", "thinking");
    stale.command_sequence_id = 40;
    stale.asset.cue_id = "barn-thinking";
    Require(renderer.Prepare(stale, 0).error == tbot::LessonCinematicError::kStaleCommand,
            "lower-sequence v2 prepare is rejected without staging resources");
    Require(renderer.Start(40, "barn-listen", 0).error ==
                tbot::LessonCinematicError::kStaleCommand,
            "lower-sequence v2 control is rejected");
    Require(renderer.Start(41, "barn-thinking", 0).error ==
                tbot::LessonCinematicError::kStaleCommand,
            "same sequence with changed cue identity is rejected");
    Require(fake.opens == 1 && fake.closes == 0 && fake.allocation_sizes.size() == 1,
            "stale v2 commands do not touch resources");

    auto replacement = V2Config("barn-thinking", "thinking");
    replacement.command_sequence_id = 42;
    replacement.asset.cue_id = "barn-thinking";
    replacement.asset.sd_path =
        "/sdcard/tbot/lesson-assets/flattenedCinematic.barn-thinking";
    Require(renderer.Prepare(replacement, 0).accepted,
            "a newer v2 cue transactionally replaces the old cue");
    Require(fake.opens == 2 && fake.closes == 1 &&
                fake.allocation_sizes.size() == 2 && fake.frees == 1,
            "old cue closes and frees exactly once after replacement frame zero succeeds");

    auto changed_identity = replacement;
    changed_identity.effect = "listen";
    Require(renderer.Prepare(changed_identity, 0).error ==
                tbot::LessonCinematicError::kStaleCommand,
            "same sequence with changed v2 effect is rejected");
    replacement.command_sequence_id = 1;
    replacement.new_session = true;
    fake.fail_open = true;
    Require(renderer.Prepare(replacement, 0).error == tbot::LessonCinematicError::kFileOpen &&
                renderer.prepared() && fake.opens == 2 && fake.closes == 1,
            "failed fresh-session staging preserves the previous cue and sequence state");
    fake.fail_open = false;
    Require(renderer.Prepare(replacement, 0).accepted,
            "fresh session transactionally permits a command sequence starting at one");
}

void TestTemplateV2RejectsUnsafeOrInexactMetadata() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    auto invalid = V2Config("Barn Listen");
    invalid.asset.cue_id = "Barn Listen";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kMetadataMismatch,
            "v2 cue ID must be a safe lowercase slug");
    invalid = V2Config();
    invalid.step_key = "barn/one";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kMetadataMismatch,
            "v2 step key must be a safe lowercase slug");
    invalid = V2Config();
    invalid.effect = "unknown";
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kMetadataMismatch,
            "v2 effect must be allowlisted");
    invalid = V2Config();
    invalid.playback_mode = tbot::LessonCinematicPlaybackMode::kOnce;
    Require(renderer.Prepare(invalid, 0).error == tbot::LessonCinematicError::kMetadataMismatch,
            "v2 playback mode must match the effect contract");
}

}  // namespace

int main() {
    TestExactContractAllocationTimingAndLifecycle();
    TestValidationErrorsIdempotencyAndCleanup();
    TestRepeatedPreparePlayCancelIsLeakFree();
    TestFailedRepreparePreservesPreparedStreamTransactionally();
    TestTemplateV2OnceCompletesAtEof();
    TestTemplateV2LoopCrossesSeamsWithoutResourceChurn();
    TestTemplateV2LoopPauseResumePreservesPhase();
    TestTemplateV2IdentityFencingAndTransactionalReplacement();
    TestTemplateV2RejectsUnsafeOrInexactMetadata();
    std::cout << "lesson_flattened_cinematic_renderer tests passed\n";
    return 0;
}
