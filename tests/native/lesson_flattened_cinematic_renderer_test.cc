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
    bool metadata_mismatch = false;
    std::uint64_t decode_duration_ms = 0;
    tbot::LessonCinematicError operation_error = tbot::LessonCinematicError::kNone;
    std::vector<std::size_t> decoded_indices;
    std::vector<std::string> native_events;
    const std::uint16_t* dma_owned = nullptr;
    bool fail_queue = false;
    bool wait_completes = true;
    std::size_t begins = 0;
    std::size_t ends = 0;
    bool open_trgb = false;
    std::uint64_t opened_bytes = 922112;
    bool bad_native_dimensions = false;
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
    const bool trgb = std::strstr(path, ".trgb") != nullptr;
    fake.open_trgb = trgb;
    *metadata = {static_cast<std::uint16_t>(trgb ? 320 : 480),
                 static_cast<std::uint16_t>(trgb ? 480 : 320), 10,
                 static_cast<std::uint32_t>(fake.metadata_mismatch ? 2 : 3), 300,
                 static_cast<std::uint32_t>(trgb ? 307200 : 64)};
    *handle = reinterpret_cast<void*>(fake.opens);
    return true;
}

void Close(void* raw, void*) { ++static_cast<FakeRuntime*>(raw)->closes; }

bool Decode(void* raw, void*, std::size_t index, std::uint8_t* destination,
            std::size_t capacity, std::uint16_t* width, std::uint16_t* height,
            std::size_t* stride) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    if (fake.fail_decode) return false;
    Require(destination != reinterpret_cast<const std::uint8_t*>(fake.dma_owned),
            "reader never overwrites a DMA-owned buffer");
    fake.now_ms += fake.decode_duration_ms;
    Require(capacity == 480u * 320u * 2u, "decode targets only the full RGB565 framebuffer");
    fake.decoded_indices.push_back(index);
    const bool trgb = fake.open_trgb;
    *width = static_cast<std::uint16_t>(trgb ? 320 : 480);
    *height = static_cast<std::uint16_t>(trgb ? 480 : 320);
    *stride = (trgb ? 320 : 480) * 2;
    if (fake.bad_native_dimensions && trgb) {
        *stride = 1;
    }
    std::memset(destination, static_cast<int>(index), capacity);
    return true;
}

bool BeginNative(void* raw) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    ++fake.begins;
    fake.native_events.emplace_back("begin");
    return true;
}

bool QueueNative(void* raw, const std::uint16_t* pixels, std::uint16_t width,
                 std::uint16_t height) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    Require(fake.dma_owned == nullptr, "queue follows completion of the previous DMA");
    Require(width == 320 && height == 480, "TRGB queues panel-native dimensions");
    fake.native_events.emplace_back("queue");
    if (fake.fail_queue) return false;
    fake.dma_owned = pixels;
    return true;
}

bool WaitNative(void* raw, std::uint32_t timeout_ms) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    Require(fake.dma_owned != nullptr, "wait applies only to an in-flight DMA");
    Require(timeout_ms > 0, "DMA completion wait is bounded");
    fake.native_events.emplace_back("wait");
    if (!fake.wait_completes) return false;
    fake.dma_owned = nullptr;
    return true;
}

void EndNative(void* raw) {
    auto& fake = *static_cast<FakeRuntime*>(raw);
    Require(fake.dma_owned == nullptr, "LVGL ownership resumes only after true DMA completion");
    ++fake.ends;
    fake.native_events.emplace_back("end");
}

bool StreamBytes(void* raw, void*, std::uint64_t* bytes) {
    *bytes = static_cast<FakeRuntime*>(raw)->opened_bytes;
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

tbot::LessonFlattenedCinematicRendererOps NativeOps(FakeRuntime* fake) {
    return {Ops(fake), BeginNative, QueueNative, WaitNative, EndNative, nullptr, StreamBytes};
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

tbot::LessonFlattenedCinematicPhaseConfig TrgbConfig(bool loop = false) {
    auto config = Config();
    config.template_version = 2;
    config.phase_id = loop ? "barn-listen" : "barn-opening";
    config.duration_ms = 300;
    config.frame_count = 3;
    config.loop = loop;
    config.asset.phase_id = config.phase_id;
    config.asset.sd_path = "/sdcard/tbot/lesson-assets/flattenedCinematic.barn-opening.trgb";
    config.asset.media_type = "application/vnd.tbot.rgb565-indexed";
    config.asset.bytes = 922112;
    config.asset.width = 0;
    config.asset.height = 0;
    config.asset.container_version = 1;
    config.asset.stored_width = 320;
    config.asset.stored_height = 480;
    config.asset.orientation = "panelNativeClockwise";
    config.asset.frame_bytes = 307200;
    return config;
}

void TestTrgbDoubleBufferOrderingLoopAndCleanup() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(NativeOps(&fake));
    Require(renderer.Prepare(TrgbConfig(), 0).accepted, "TRGB prepare succeeds");
    Require(fake.allocation_sizes == std::vector<std::size_t>({307200, 307200}),
            "TRGB allocates exactly two full native frame buffers");
    Require(fake.decoded_indices == std::vector<std::size_t>({0, 1}),
            "TRGB preloads frame zero and sequentially prefetches frame one");
    Require(fake.native_events == std::vector<std::string>({"begin", "queue"}),
            "TRGB begins ownership before queuing frame zero");
    Require(renderer.Start(42, "barn-opening", 0).accepted, "TRGB starts");
    Require(renderer.Tick(210).accepted, "one isolated late TRGB tick is degraded, not fatal");
    Require(fake.decoded_indices == std::vector<std::size_t>({0, 1}) &&
                fake.native_events == std::vector<std::string>({"begin", "queue"}),
            "isolated lateness retains the completed frame without seeking or DMA reuse");
    Require(renderer.Tick(310).accepted &&
                fake.decoded_indices == std::vector<std::size_t>({0, 1, 2}),
            "clock rebase resumes the next sequential frame after isolated lateness");
    Require(renderer.Tick(520).error == tbot::LessonCinematicError::kDecodeTimeout,
            "a second drop inside 300 presented frames fails the cue");
    Require(fake.ends == 1 && fake.frees == 2 && fake.closes == 1,
            "failed late cue cleans resources only after DMA completion");

    FakeRuntime consecutive_fake;
    tbot::LessonFlattenedCinematicRenderer consecutive_renderer(NativeOps(&consecutive_fake));
    Require(consecutive_renderer.Prepare(TrgbConfig(), 0).accepted &&
                consecutive_renderer.Start(42, "barn-opening", 0).accepted,
            "consecutive-drop fixture starts");
    Require(consecutive_renderer.Tick(210).accepted &&
                consecutive_renderer.Tick(420).error == tbot::LessonCinematicError::kDecodeTimeout,
            "consecutive timing drops fail without seeking forward");
    Require(consecutive_fake.decoded_indices == std::vector<std::size_t>({0, 1}),
            "consecutive drops never trigger out-of-sequence reads");

    FakeRuntime loop_fake;
    tbot::LessonFlattenedCinematicRenderer loop_renderer(NativeOps(&loop_fake));
    Require(loop_renderer.Prepare(TrgbConfig(true), 0).accepted, "loop TRGB prepares");
    Require(loop_renderer.Start(42, "barn-listen", 0).accepted, "loop TRGB starts");
    Require(loop_renderer.Tick(100).accepted && loop_renderer.Tick(200).accepted &&
                loop_renderer.Tick(300).accepted,
            "loop TRGB crosses its cue boundary sequentially");
    Require(loop_fake.decoded_indices == std::vector<std::size_t>({0, 1, 2, 0, 1}),
            "loop resets sequential reading at frame zero and prefetches the next frame");
}

void TestTrgbValidationFailuresAndDmaQuarantine() {
    const char* const message = "invalid TRGB metadata is rejected before allocation";
    auto AssertInvalid = [&](auto mutate) {
        FakeRuntime fake;
        auto config = TrgbConfig();
        mutate(config);
        tbot::LessonFlattenedCinematicRenderer renderer(NativeOps(&fake));
        Require(renderer.Prepare(config, 0).error == tbot::LessonCinematicError::kMetadataMismatch &&
                    fake.allocation_sizes.empty(), message);
    };
    AssertInvalid([](auto& c) { c.asset.container_version = 0; });
    AssertInvalid([](auto& c) { c.asset.stored_width = 480; });
    AssertInvalid([](auto& c) { c.asset.stored_height = 320; });
    AssertInvalid([](auto& c) { c.asset.orientation = "landscape"; });
    AssertInvalid([](auto& c) { c.asset.frame_bytes = 1; });
    AssertInvalid([](auto& c) { c.duration_ms = 301; });

    FakeRuntime bytes_mismatch;
    ++bytes_mismatch.opened_bytes;
    tbot::LessonFlattenedCinematicRenderer bytes_renderer(NativeOps(&bytes_mismatch));
    Require(bytes_renderer.Prepare(TrgbConfig(), 0).error ==
                tbot::LessonCinematicError::kMetadataMismatch && bytes_mismatch.begins == 0,
            "TRGB file bytes must match the attested asset before display ownership");

    FakeRuntime read_failure;
    read_failure.fail_decode = true;
    read_failure.operation_error = tbot::LessonCinematicError::kFileRead;
    tbot::LessonFlattenedCinematicRenderer read_renderer(NativeOps(&read_failure));
    Require(read_renderer.Prepare(TrgbConfig(), 0).error == tbot::LessonCinematicError::kFileRead,
            "TRGB CRC/read failures retain their typed error");

    FakeRuntime queue_failure;
    queue_failure.fail_queue = true;
    tbot::LessonFlattenedCinematicRenderer queue_renderer(NativeOps(&queue_failure));
    Require(queue_renderer.Prepare(TrgbConfig(), 0).error == tbot::LessonCinematicError::kPresentFailed &&
                queue_failure.ends == 1 && queue_failure.frees == 2,
            "TRGB queue failure returns ownership and cleans resources");

    FakeRuntime timeout;
    tbot::LessonFlattenedCinematicRenderer timeout_renderer(NativeOps(&timeout));
    Require(timeout_renderer.Prepare(TrgbConfig(), 0).accepted, "timeout fixture prepares");
    Require(timeout_renderer.Start(42, "barn-opening", 0).accepted, "timeout fixture starts");
    timeout.wait_completes = false;
    Require(timeout_renderer.Tick(100).error == tbot::LessonCinematicError::kDecodeTimeout,
            "DMA wait timeout is typed");
    Require(timeout.frees == 0 && timeout.closes == 0 && timeout.ends == 0,
            "timed-out DMA buffer and its resources remain quarantined");
    timeout.wait_completes = true;
    timeout_renderer.DiscardSession();
    Require(timeout.frees == 2 && timeout.closes == 1 && timeout.ends == 1,
            "later confirmed DMA completion drains quarantine safely");
}

void TestTrgbStopAndSessionReplacementDrainInFlightDma() {
    FakeRuntime fake;
    tbot::LessonFlattenedCinematicRenderer renderer(NativeOps(&fake));
    Require(renderer.Prepare(TrgbConfig(), 0).accepted, "replacement baseline prepares");
    auto replacement = TrgbConfig();
    replacement.command_sequence_id = 50;
    Require(renderer.Prepare(replacement, 0).accepted,
            "replacement prepare safely drains the previous session");
    Require(fake.begins == 2 && fake.ends == 1 && fake.frees == 2 && fake.closes == 1,
            "session replacement waits, ends ownership, and releases only the old pair");
    Require(renderer.Start(51, "barn-opening", 0).accepted, "replacement session starts");
    Require(renderer.Stop(52, "barn-opening").accepted,
            "TRGB stop drains its in-flight frame");
    Require(fake.ends == 2 && fake.frees == 4 && fake.closes == 2,
            "TRGB stop leaves no stream, PSRAM buffer, or display ownership behind");
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

void TestDecodeStallWatchdogAllowsExpectedS3Latency() {
    FakeRuntime fake;
    fake.decode_duration_ms = 250;
    tbot::LessonFlattenedCinematicRenderer renderer(Ops(&fake));
    Require(renderer.Prepare(Config(), 0).accepted,
            "valid S3 frame-zero decode latency does not fail the cinematic phase");
    Require(renderer.Start(42, "opening", fake.now_ms).accepted,
            "accepted slow prepare can start playback");
    fake.decode_duration_ms = 101;
    Require(renderer.Tick(fake.now_ms + 100).error == tbot::LessonCinematicError::kDecodeTimeout,
            "steady playback still enforces the 100 ms frame budget");

    auto boundary = Config();
    boundary.command_sequence_id = 43;
    fake.decode_duration_ms = 500;
    Require(renderer.Prepare(boundary, 0).accepted,
            "decode exactly at the stall watchdog boundary remains valid");
    Require(renderer.Cancel(44, "opening").accepted, "boundary prepare remains cancellable");

    auto stalled = Config();
    stalled.command_sequence_id = 45;
    fake.decode_duration_ms = 501;
    Require(renderer.Prepare(stalled, 0).error == tbot::LessonCinematicError::kDecodeTimeout,
            "decode beyond the stall watchdog is rejected as a timeout");
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

void TestTrgbReplayValidatesNativePrefetchAndDeadline() {
    FakeRuntime mismatch;
    tbot::LessonFlattenedCinematicRenderer mismatch_renderer(NativeOps(&mismatch));
    Require(mismatch_renderer.Prepare(TrgbConfig(), 0).accepted &&
                mismatch_renderer.Start(42, "barn-opening", 0).accepted,
            "replay mismatch fixture starts");
    Require(mismatch_renderer.Tick(100).accepted && mismatch_renderer.Tick(200).accepted &&
                mismatch_renderer.Tick(300).accepted,
            "replay mismatch fixture completes first pass");
    mismatch.bad_native_dimensions = true;
    Require(mismatch_renderer.Start(43, "barn-opening", 300).error ==
                tbot::LessonCinematicError::kMetadataMismatch,
            "TRGB replay prefetch validates native dimensions and stride");

    FakeRuntime timeout;
    tbot::LessonFlattenedCinematicRenderer timeout_renderer(NativeOps(&timeout));
    Require(timeout_renderer.Prepare(TrgbConfig(), 0).accepted &&
                timeout_renderer.Start(42, "barn-opening", 0).accepted,
            "replay timeout fixture starts");
    Require(timeout_renderer.Tick(100).accepted && timeout_renderer.Tick(200).accepted &&
                timeout_renderer.Tick(300).accepted,
            "replay timeout fixture completes first pass");
    timeout.decode_duration_ms = 101;
    Require(timeout_renderer.Start(43, "barn-opening", timeout.now_ms).error ==
                tbot::LessonCinematicError::kDecodeTimeout,
            "TRGB replay start enforces read/decode deadline");
}

}  // namespace

int main() {
    TestTrgbDoubleBufferOrderingLoopAndCleanup();
    TestTrgbValidationFailuresAndDmaQuarantine();
    TestTrgbStopAndSessionReplacementDrainInFlightDma();
    TestExactContractAllocationTimingAndLifecycle();
    TestValidationErrorsIdempotencyAndCleanup();
    TestRepeatedPreparePlayCancelIsLeakFree();
    TestDecodeStallWatchdogAllowsExpectedS3Latency();
    TestFailedRepreparePreservesPreparedStreamTransactionally();
    TestTrgbReplayValidatesNativePrefetchAndDeadline();
    std::cout << "lesson_flattened_cinematic_renderer tests passed\n";
    return 0;
}
