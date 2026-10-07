// Regression (BE08 R16, found on the LCDWiki robot): one refused decoder allocation during a
// v6 prepare left every later prepare in the lesson session failing with "no memory".
// The shared allocation-failure latch only clears when no stream holds the allocation
// state, and the player kept its other layers' streams open. After a refused allocation
// the next prepare must succeed once memory is available again.
// Run under ASan/UBSan with the real R01 decoders.
// Args: <pack root> <cacheKey> <scene sha256> <scene bytes> <first cue id> <second cue id>
#include "checked_cjson.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_original_source_allocator.h"
#include "lesson_original_source_pack_media.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace tbot;

namespace {

class MeasureOnly final : public TVideoTextRenderer {
public:
    double Measure(const std::string& text, int font_px) override { return text.size() * font_px * .48; }
    void Render(const std::string&, int, double, double, double, const TVideoCoveragePlot&) override {}
};

int failures = 0;
void Expect(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    failures += !condition;
}

// Refuses exactly one allocation when armed; otherwise a plain host heap.
bool g_refuse_next = false;
int g_refused = 0;

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) return 2;
    const std::string root = argv[1], cache_key = argv[2], sha = argv[3], first_cue = argv[5], second_cue = argv[6];
    const auto bytes = static_cast<std::uint32_t>(std::stoul(argv[4]));
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    const auto begun = coordinator.TryBeginLessonSession("assignment-alloc", "session-alloc");
    if (!begun.acquired) return 4;
    const OriginalSourceLeaseSource lease = [&]() {
        return coordinator.TryRetainLessonSession("assignment-alloc", "session-alloc", begun.generation);
    };
    OriginalSourceAllocationState allocations;
    OriginalSourceAllocatorBackend host{
        nullptr,
        [](void*, std::size_t alignment, std::size_t size) -> void* {
            if (g_refuse_next) {
                g_refuse_next = false;
                ++g_refused;
                return nullptr;
            }
            void* pointer = nullptr;
            return posix_memalign(&pointer, std::max(alignment, sizeof(void*)), size) == 0 ? pointer : nullptr;
        },
        [](void*, void* pointer) { std::free(pointer); },
        [](void*, void*, std::size_t requested) { return requested; }};
    OriginalSourceAllocator allocator(allocations, host);
    if (!allocator.Bind()) return 8;
    {
        OriginalSourcePackMedia media(root, lease, &allocations, nullptr);
        MeasureOnly text;
        OriginalSourceScenePlayer player(MakeOriginalSourcePackSceneLoader(root, lease), &media, &text,
                                         [](const std::uint16_t*, int, int) { return true; });
        std::uint64_t sequence = 0;
        const auto prepare = [&](const std::string& cue, std::uint64_t now) {
            CheckedCJsonPtr json(cJSON_Parse(
                ("{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"" + cue + "\",\"commandSequenceId\":" +
                 std::to_string(++sequence) + ",\"scene\":{\"cacheKey\":\"" + cache_key + "\",\"sha256\":\"" + sha +
                 "\",\"bytes\":" + std::to_string(bytes) + "}}}")
                    .c_str()));
            return player.Handle("lesson_prepare", json.get(), now);
        };
        Expect(prepare(first_cue, 1000).accepted, "first cue prepares with memory available");
        g_refuse_next = true;  // the second cue opens new streams; refuse one allocation
        const auto refused = prepare(second_cue, 20000);
        Expect(g_refused == 1 && !refused.accepted, "second cue refused when one allocation is refused");
        std::printf("  reason: %s\n", refused.error.c_str());
        const auto recovered = prepare(second_cue, 40000);
        Expect(recovered.accepted, "the next prepare succeeds once memory is available again");
        if (!recovered.accepted) std::printf("  reason: %s\n", recovered.error.c_str());
        Expect(player.Tick(40100) == nullptr, "playback continues after recovery");
        player.Reset();
        player.Release();
    }
    coordinator.EndLessonSession("assignment-alloc", "session-alloc", begun.generation);
    if (!allocator.Unbind()) return 9;
    std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
