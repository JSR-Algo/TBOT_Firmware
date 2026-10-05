// Cross-check tool (not a unit test): plays the real Farm scene from an SD-layout
// pack through OriginalSourcePackMedia (R01 session decode under a real lesson read
// lease), the scene player, painter and software canvas, and
//  1. plays every cue end to end on the 100 ms clock, reporting refusals/errors;
//  2. renders every paint-op trace frame and compares it with a Chromium rendering of
//     the same frame and originals (text excluded on both sides), in RGB565.
#include "checked_cjson.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_original_source_allocator.h"
#include "lesson_original_source_pack_media.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tbot;

namespace {
std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
const cJSON* Get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

class MeasureOnly final : public TVideoTextRenderer {
public:
    double Measure(const std::string& text, int font_px) override { return text.size() * font_px * .48; }
    void Render(const std::string&, int, double, double, double, const TVideoCoveragePlot&) override {}
};

int Expand5(int v) { return (v << 3) | (v >> 2); }
int Expand6(int v) { return (v << 2) | (v >> 4); }
}  // namespace

int main(int argc, char** argv) {
    // pack root (contains <cacheKey>/...), cacheKey, scene sha256, scene bytes,
    // paint-op vectors, frame-state vectors, chromium dir (<index>.rgba)
    if (argc != 8) return 2;
    const std::string root = argv[1], cache_key = argv[2], scene_sha = argv[3];
    const std::uint32_t scene_bytes = static_cast<std::uint32_t>(std::stoul(argv[4]));
    CheckedCJsonPtr paint(cJSON_Parse(Read(argv[5]).c_str()));
    CheckedCJsonPtr frames(cJSON_Parse(Read(argv[6]).c_str()));
    const std::string chromium_dir = argv[7];
    if (!paint || !frames) return 3;

    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    const auto begun = coordinator.TryBeginLessonSession("assignment-farm", "session-farm");
    if (!begun.acquired) return 4;
    const std::uint64_t generation = begun.generation;
    const OriginalSourceLeaseSource lease = [&]() {
        return coordinator.TryRetainLessonSession("assignment-farm", "session-farm", generation);
    };
    OriginalSourceAllocationState allocations;
    // Production routing (tbot_original_* owner) over a plain host heap backend.
    OriginalSourceAllocatorBackend host{
        nullptr,
        [](void*, std::size_t alignment, std::size_t bytes) -> void* {
            void* pointer = nullptr;
            return posix_memalign(&pointer, std::max(alignment, sizeof(void*)), bytes) == 0 ? pointer : nullptr;
        },
        [](void*, void* pointer) { std::free(pointer); },
        [](void*, void*, std::size_t requested) { return requested; }};
    OriginalSourceAllocator allocator(allocations, host);
    if (!allocator.Bind()) return 8;
    OriginalSourcePackMedia media(root, lease, &allocations, nullptr);
    MeasureOnly text;
    std::vector<std::uint16_t> panel;
    OriginalSourceScenePlayer player(MakeOriginalSourcePackSceneLoader(root, lease), &media, &text,
                                     [&](const std::uint16_t* rgb565, int w, int h) {
                                         panel.assign(rgb565, rgb565 + w * h);
                                         return true;
                                     });
    std::uint64_t sequence = 0;
    const auto handle = [&](const char* type, const std::string& body, std::uint64_t now) {
        CheckedCJsonPtr json(cJSON_Parse(body.c_str()));
        return player.Handle(type, json.get(), now);
    };
    const auto prepare = [&](const std::string& cue, std::uint64_t now) {
        return handle("lesson_prepare",
                      "{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"" + cue + "\",\"commandSequenceId\":" +
                          std::to_string(++sequence) + ",\"scene\":{\"cacheKey\":\"" + cache_key + "\",\"sha256\":\"" +
                          scene_sha + "\",\"bytes\":" + std::to_string(scene_bytes) + "}}}",
                      now);
    };
    const auto start = [&](const std::string& cue, std::uint64_t now) {
        return handle("lesson_start",
                      "{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"" + cue + "\",\"commandSequenceId\":" +
                          std::to_string(++sequence) + "}}",
                      now);
    };

    // 1. Every cue end to end.
    std::printf("{\n \"playthrough\": [\n");
    std::uint64_t now = 1000;
    int errors = 0;
    double slowest_ms = 0, total_ms = 0;
    int rendered = 0;
    for (const cJSON* cue = Get(frames.get(), "cues")->child; cue != nullptr; cue = cue->next) {
        const std::string id = Get(cue, "cueId")->valuestring;
        const auto clock_start = std::chrono::steady_clock::now();
        const auto prepared = prepare(id, now);
        const auto started = start(id, now);
        const std::uint64_t presented_before = player.presented_frames();
        const double duration = Get(cue, "durationMs")->valuedouble;
        std::string error;
        for (std::uint64_t t = 0; t <= static_cast<std::uint64_t>(duration); t += 100) {
            const auto frame_start = std::chrono::steady_clock::now();
            if (const char* reason = player.Tick(now + t)) {
                error = reason;
                break;
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count();
            slowest_ms = std::max(slowest_ms, ms);
            total_ms += ms;
            ++rendered;
        }
        const double cue_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock_start).count();
        errors += !prepared.accepted || !started.accepted || !error.empty();
        std::printf("  {\"cueId\":\"%s\",\"prepared\":%s,\"started\":%s,\"presented\":%llu,\"error\":\"%s%s\","
                    "\"hostMs\":%.1f},\n",
                    id.c_str(), prepared.accepted ? "true" : "false", started.accepted ? "true" : "false",
                    static_cast<unsigned long long>(player.presented_frames() - presented_before), error.c_str(),
                    prepared.accepted ? "" : prepared.error.c_str(), cue_ms);
        now += static_cast<std::uint64_t>(duration) + 1000;
    }
    std::printf("  {\"cues\":%d,\"errors\":%d,\"framesRendered\":%d,\"hostMeanFrameMs\":%.2f,\"hostSlowestFrameMs\":%.2f,"
                "\"streamsOpened\":%llu,\"decoderPeakBytes\":%zu,\"decoderFailures\":%zu}\n ],\n",
                cJSON_GetArraySize(Get(frames.get(), "cues")), errors, rendered, total_ms / std::max(1, rendered),
                slowest_ms, static_cast<unsigned long long>(player.opened_streams()), allocator.stats().peak_charged,
                allocator.stats().failures);

    // 2. Trace frames against Chromium.
    std::printf(" \"chromium\": [\n");
    int index = 0;
    double worst_mae = 0;
    std::string current_cue;
    for (const cJSON* trace = Get(paint.get(), "traces")->child; trace != nullptr; trace = trace->next, ++index) {
        const std::string id = Get(trace, "cueId")->valuestring;
        const std::uint64_t frame_ms = static_cast<std::uint64_t>(Get(trace, "frameIndex")->valueint) * 100;
        if (id != current_cue) {
            now += 100000;
            if (!prepare(id, now).accepted || !start(id, now).accepted) return 5;
            current_cue = id;
        }
        const auto origin = now;
        if (const char* reason = player.Tick(origin + frame_ms)) {
            std::fprintf(stderr, "trace %d: %s\n", index, reason);
            return 6;
        }
        const std::string chromium = Read(chromium_dir + "/" + std::to_string(index) + ".rgba");
        if (chromium.size() != 480 * 320 * 4 || panel.size() != 480 * 320) return 7;
        double sum = 0;
        int max = 0, over8 = 0, over24 = 0;
        for (int pixel = 0; pixel < 480 * 320; ++pixel) {
            const std::uint16_t value = panel[pixel];
            const int ours[3] = {Expand5(value >> 11), Expand6((value >> 5) & 0x3f), Expand5(value & 0x1f)};
            const auto* c = reinterpret_cast<const std::uint8_t*>(chromium.data()) + pixel * 4;
            const int theirs[3] = {Expand5(c[0] >> 3), Expand6(c[1] >> 2), Expand5(c[2] >> 3)};
            for (int channel = 0; channel < 3; ++channel) {
                const int diff = std::abs(ours[channel] - theirs[channel]);
                sum += diff;
                max = std::max(max, diff);
                over8 += diff > 8;
                over24 += diff > 24;
            }
        }
        const double mae = sum / (480 * 320 * 3);
        worst_mae = std::max(worst_mae, mae);
        std::printf("  {\"trace\":%d,\"cueId\":\"%s\",\"frameIndex\":%d,\"mae\":%.4f,\"max\":%d,\"pctOver8\":%.4f,"
                    "\"pctOver24\":%.4f},\n",
                    index, id.c_str(), Get(trace, "frameIndex")->valueint, mae, max, 100.0 * over8 / (480 * 320 * 3),
                    100.0 * over24 / (480 * 320 * 3));
        // Keep the panel for inspection.
        std::ofstream(chromium_dir + "/../player-" + std::to_string(index) + ".rgb565", std::ios::binary)
            .write(reinterpret_cast<const char*>(panel.data()), panel.size() * 2);
    }
    std::printf("  {\"traces\":%d,\"worstMae\":%.4f}\n ]\n}\n", index, worst_mae);
    player.Release();
    if (!allocator.Unbind()) return 9;
    coordinator.EndLessonSession("assignment-farm", "session-farm", generation);
    return errors == 0 ? 0 : 1;
}
