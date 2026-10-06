// Regression (BE08 R14, found on the LCDWiki robot): the lesson storage session can end
// while a v6 prepare is in flight (the application abandons it when the lesson transport
// fails). Prepare must fail cleanly, later prepares must be refused, and Reset/Release
// and a fresh session must still work. Run under ASan/UBSan with the real R01 decoders.
// Args: <pack root> <cacheKey> <scene sha256> <scene bytes> <first cue id>
#include "checked_cjson.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_original_source_allocator.h"
#include "lesson_original_source_pack_media.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

enum class LossMode { kLeaseRefused, kForceEnded };

int RunCase(LossMode mode, const std::string& root, const std::string& cache_key, const std::string& sha,
            std::uint32_t bytes, const std::string& cue) {
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    const std::string assignment = "assignment-loss", session = mode == LossMode::kLeaseRefused ? "s1" : "s2";
    const auto begun = coordinator.TryBeginLessonSession(assignment, session);
    if (!begun.acquired) return 4;
    int lease_calls = 0;
    std::string current_session = session;
    std::uint64_t generation = begun.generation;
    const OriginalSourceLeaseSource lease = [&]() -> LessonAssetReadLease {
        // The first lease (scene load) succeeds; the session is lost before the media opens.
        ++lease_calls;
        if (mode == LossMode::kLeaseRefused && lease_calls >= 2 && lease_calls < 100) return {};
        if (mode == LossMode::kForceEnded && lease_calls == 2) coordinator.ForceEndLessonSession();
        return coordinator.TryRetainLessonSession(assignment, current_session, generation);
    };
    OriginalSourceAllocationState allocations;
    OriginalSourceAllocatorBackend host{
        nullptr,
        [](void*, std::size_t alignment, std::size_t size) -> void* {
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
        const auto prepare = [&](std::uint64_t now) {
            CheckedCJsonPtr json(cJSON_Parse(
                ("{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"" + cue + "\",\"commandSequenceId\":" +
                 std::to_string(++sequence) + ",\"scene\":{\"cacheKey\":\"" + cache_key + "\",\"sha256\":\"" + sha +
                 "\",\"bytes\":" + std::to_string(bytes) + "}}}")
                    .c_str()));
            return player.Handle("lesson_prepare", json.get(), now);
        };
        const auto first = prepare(1000);
        Expect(!first.accepted, "prepare refused when the session is lost mid-prepare");
        std::printf("  reason: %s\n", first.error.c_str());
        const auto second = prepare(2000);
        Expect(!second.accepted, "later prepare refused while no session");
        Expect(player.Tick(3000) == nullptr || true, "tick after failure does not crash");
        player.Reset();
        player.Release();
        Expect(true, "Reset and Release after a lost session");
        // Recovery: a fresh session plays again.
        coordinator.ForceEndLessonSession();
        const auto again = coordinator.TryBeginLessonSession(assignment, session + "-again");
        current_session = session + "-again";
        generation = again.generation;
        lease_calls = 100;
        const auto recovered = prepare(5000);
        Expect(again.acquired && recovered.accepted, "a fresh session prepares again");
        if (!recovered.accepted) std::printf("  reason: %s\n", recovered.error.c_str());
        player.Reset();
        player.Release();
    }
    coordinator.ForceEndLessonSession();
    if (!allocator.Unbind()) return 9;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) return 2;
    const std::string root = argv[1], cache_key = argv[2], sha = argv[3], cue = argv[5];
    const auto bytes = static_cast<std::uint32_t>(std::stoul(argv[4]));
    for (LossMode mode : {LossMode::kLeaseRefused, LossMode::kForceEnded}) {
        std::printf("== %s\n", mode == LossMode::kLeaseRefused ? "lease refused" : "session force-ended");
        if (const int code = RunCase(mode, root, cache_key, sha, bytes, cue)) {
            std::printf("setup failure %d\n", code);
            return code;
        }
    }
    std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
