// Regression (BE08 R19, robot 14:c1:9f:d1:ac:20): the 19-cue Farm v6 self-test failed 1-4
// cues with "media: no memory". Each failure was a PSRAM request of a large decoder block
// (the 853,724 B background snapshot, the H264 decoder context) refused while ~930 KB of
// PSRAM was free: other heap owners had split the free space between background reopens.
//
// Model: one PSRAM heap, managed by ESP-IDF's own TLSF multi_heap, holds the routed decoder
// allocations and a seeded foreign owner (other PSRAM users: Wi-Fi, websocket, LVGL) that
// allocates and frees small blocks between decoder requests. The real R01 decoders play the
// 19-cue plan twice per seed:
//   - routed straight to the heap (the R19 firmware), a large request is refused although
//     the heap holds more free bytes than it asks for;
//   - through the production chain (retaining backend over a dedicated region over the
//     heap), every cue plays and no decoder request is refused, without refusing the
//     foreign owner more often.
// Retention alone still failed on ~150-240 KB per-clip blocks in this heap, and a region
// alone fragmented under the decoder's own reopen pattern (BE08 R20 notes).
// Run under ASan/UBSan.
// Args: <pack root> <cacheKey> <scene sha256> <scene bytes> <seed[,seed...]> {<cue> <ms>}...
#include "checked_cjson.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_original_source_allocator.h"
#include "lesson_original_source_pack_media.h"

extern "C" {
#include "multi_heap.h"
#include "multi_heap_internal.h"
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace tbot;

// TLSF's poisoning hooks are weak and unset on the robot (heap poisoning disabled); Mach-O
// cannot leave a weak reference undefined, so the host defines them as no-ops.
extern "C" void block_absorb_post_hook(void*, std::size_t, bool) {}
extern "C" bool tlsf_check_hook(void*, std::size_t, bool) { return true; }

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

// The PSRAM heap: ESP-IDF's own TLSF multi_heap (the robot's allocator, heap poisoning
// disabled as in its sdkconfig) over one fixed region.
class PsramHeap {
public:
    explicit PsramHeap(std::size_t capacity) {
        base_ = std::aligned_alloc(4096, capacity);
        heap_ = multi_heap_register_impl(base_, capacity);
        if (heap_ == nullptr) std::abort();
    }
    ~PsramHeap() { std::free(base_); }
    void* Allocate(std::size_t alignment, std::size_t bytes) {
        return alignment <= 4 ? multi_heap_malloc_impl(heap_, bytes)
                              : multi_heap_aligned_alloc_impl(heap_, bytes, alignment);
    }
    void Release(void* pointer) { multi_heap_free_impl(heap_, pointer); }
    std::size_t Size(void* pointer) const { return multi_heap_get_allocated_size_impl(heap_, pointer); }
    std::size_t free_bytes() const { return multi_heap_free_size_impl(heap_); }
    std::size_t min_free() const { return multi_heap_minimum_free_size_impl(heap_); }
    std::size_t largest_free() const {
        multi_heap_info_t info{};
        multi_heap_get_info_impl(heap_, &info);
        return info.largest_free_block;
    }

private:
    void* base_;
    multi_heap_handle_t heap_;
};

// Other PSRAM owners: blocks above the 512 B internal-RAM threshold, mostly short-lived,
// a few held for minutes. One step per routed decoder request and per frame tick.
class ForeignOwner {
public:
    ForeignOwner(PsramHeap* heap, std::uint32_t seed) : heap_(heap), seed_(seed) {}
    ~ForeignOwner() {
        for (const auto& block : blocks_) heap_->Release(block.pointer);
    }
    void Step() {
        ++clock_;
        for (std::size_t i = 0; i < blocks_.size();) {
            if (blocks_[i].expires <= clock_) {
                heap_->Release(blocks_[i].pointer);
                blocks_[i] = blocks_.back();
                blocks_.pop_back();
            } else {
                ++i;
            }
        }
        if (Next() % 8 != 0) return;
        const std::size_t bytes = 512 + Next() % (16 * 1024);
        const bool held = Next() % 32 == 0;
        const std::uint64_t lifetime = held ? 3000 + Next() % 20000 : 1 + Next() % 400;
        if (void* pointer = heap_->Allocate(4, bytes)) {
            blocks_.push_back({pointer, clock_ + lifetime});
        } else {
            ++refused_;
        }
    }
    std::size_t refused() const { return refused_; }

private:
    struct Block {
        void* pointer;
        std::uint64_t expires;
    };
    std::uint32_t Next() {
        seed_ = seed_ * 1664525u + 1013904223u;
        return seed_ >> 8;
    }
    PsramHeap* heap_;
    std::uint32_t seed_;
    std::uint64_t clock_ = 0;
    std::vector<Block> blocks_;
    std::size_t refused_ = 0;
};

struct World {
    PsramHeap* heap;
    ForeignOwner* foreign;
    std::size_t refused = 0, refused_with_free_bytes = 0, largest_refused = 0;
};
World* g_world = nullptr;

// The PSRAM heap backend under the decoder; other owners act between its requests.
void* WorldAllocate(void*, std::size_t alignment, std::size_t bytes) {
    g_world->foreign->Step();
    void* pointer = g_world->heap->Allocate(alignment, bytes);
    if (pointer == nullptr) {
        ++g_world->refused;
        if (g_world->heap->free_bytes() >= bytes) ++g_world->refused_with_free_bytes;
        g_world->largest_refused = std::max(g_world->largest_refused, bytes);
    }
    return pointer;
}
void WorldRelease(void*, void* pointer) { g_world->heap->Release(pointer); }
std::size_t WorldCharged(void*, void* pointer, std::size_t) { return g_world->heap->Size(pointer); }

// The region's heap: the same ESP-IDF TLSF multi_heap the device registers.
OriginalSourceRegionHeapOps TlsfOps() {
    return {[](void* base, std::size_t bytes) -> void* { return multi_heap_register_impl(base, bytes); },
            [](void* heap, std::size_t alignment, std::size_t bytes) {
                return multi_heap_aligned_alloc_impl(static_cast<multi_heap_handle_t>(heap), bytes, alignment);
            },
            [](void* heap, void* pointer) { multi_heap_free_impl(static_cast<multi_heap_handle_t>(heap), pointer); },
            [](void* heap, void* pointer) {
                return multi_heap_get_allocated_size_impl(static_cast<multi_heap_handle_t>(heap), pointer);
            },
            [](void* heap) { return multi_heap_minimum_free_size_impl(static_cast<multi_heap_handle_t>(heap)); }};
}

struct RunResult {
    int prepared = 0, failed = 0;
    std::size_t decoder_failures = 0, heap_refused = 0, refused_with_free_bytes = 0, largest_refused = 0;
    std::size_t foreign_refused = 0, live_peak = 0, heap_min_free = 0;
    std::size_t retain_hits = 0, region_overflows = 0, region_min_free = 0, region_reservations = 0;
    std::string first_error;
};

// R19: ~0.9 MB of PSRAM stayed free at the ~5.5 MB decoder peak; the merged firmware
// reserves ~0.7 MB more for GIF buffers. 6.4 MB leaves ~0.3-0.5 MB free at the peak.
constexpr std::size_t kHeapBytes = 6400 * 1024;
// The production decoder configuration (lesson_original_source_runtime.cc).
constexpr std::size_t kRegionBytes = 5632 * 1024;
constexpr std::size_t kRetentionBytes = 4 * 1024 * 1024;

RunResult Play(bool production, std::uint32_t seed, const std::string& root, const std::string& cache_key,
               const std::string& sha, std::uint32_t bytes, const std::vector<std::pair<std::string, unsigned>>& cues) {
    RunResult result;
    PsramHeap heap(kHeapBytes);
    ForeignOwner foreign(&heap, seed);
    World world{&heap, &foreign};
    g_world = &world;
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    const std::string session = (production ? "session-production-" : "session-r19-") + std::to_string(seed);
    const auto begun = coordinator.TryBeginLessonSession("assignment-fragmentation", session);
    if (!begun.acquired) std::abort();
    const OriginalSourceLeaseSource lease = [&]() {
        return coordinator.TryRetainLessonSession("assignment-fragmentation", session, begun.generation);
    };
    const OriginalSourceAllocatorBackend direct{nullptr, WorldAllocate, WorldRelease, WorldCharged};
    OriginalSourceRegionBackend region(direct, TlsfOps(), kRegionBytes);
    OriginalSourceRetainingBackend retention(region.backend(), kRetentionBytes);
    OriginalSourceAllocationState allocations;
    {
        OriginalSourceAllocator allocator(allocations, production ? retention.backend() : direct);
        if (!allocator.Bind()) std::abort();
        {
            OriginalSourcePackMedia media(root, lease, &allocations, nullptr);
            MeasureOnly text;
            OriginalSourceScenePlayer player(MakeOriginalSourcePackSceneLoader(root, lease), &media, &text,
                                             [](const std::uint16_t*, int, int) { return true; });
            std::uint64_t sequence = 0, now = 1000;
            for (const auto& [cue, duration] : cues) {
                CheckedCJsonPtr prepare(cJSON_Parse(
                    ("{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"" + cue +
                     "\",\"commandSequenceId\":" + std::to_string(++sequence) + ",\"scene\":{\"cacheKey\":\"" +
                     cache_key + "\",\"sha256\":\"" + sha + "\",\"bytes\":" + std::to_string(bytes) + "}}}")
                        .c_str()));
                CheckedCJsonPtr start(cJSON_Parse(("{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"" + cue +
                                                   "\",\"commandSequenceId\":" + std::to_string(++sequence) + "}}")
                                                      .c_str()));
                const auto prepared = player.Handle("lesson_prepare", prepare.get(), now);
                const auto started = player.Handle("lesson_start", start.get(), now);
                std::string error = !prepared.accepted ? prepared.error : !started.accepted ? started.error : "";
                for (const std::uint64_t origin = now; error.empty() && now - origin < duration;) {
                    now += 100;
                    foreign.Step();
                    if (const char* reason = player.Tick(now)) error = reason;
                }
                result.prepared += prepared.accepted;
                if (!error.empty()) {
                    ++result.failed;
                    if (result.first_error.empty()) result.first_error = cue + ": " + error;
                    now += 1000;  // the lesson moves on to the next cue, as the self-test does
                }
            }
            player.Reset();
        }
        result.live_peak = allocator.stats().peak_charged;
        result.decoder_failures = allocator.stats().failures;
        result.retain_hits = retention.stats().hits;
        result.region_overflows = region.stats().overflows;
        result.region_min_free = region.stats().region_min_free;
        result.region_reservations = region.stats().reservations;
        // Session end, as OriginalSourceRuntime::DiscardSession does in production.
        retention.ReleaseRetained();
        if (!region.ReleaseRegion()) std::abort();
        if (!allocator.Unbind()) std::abort();
    }
    coordinator.EndLessonSession("assignment-fragmentation", session, begun.generation);
    result.heap_refused = world.refused;
    result.refused_with_free_bytes = world.refused_with_free_bytes;
    result.largest_refused = world.largest_refused;
    result.foreign_refused = foreign.refused();
    result.heap_min_free = heap.min_free();
    g_world = nullptr;
    std::printf("%s seed=%u: prepared=%d/%zu failed=%d decoderFailures=%zu heapRefused=%zu (withFreeBytes=%zu "
                "largest=%zu) foreignRefused=%zu livePeak=%zu heapMinFree=%zu retainHits=%zu regionReservations=%zu "
                "regionOverflows=%zu regionMinFree=%zu first=%s\n",
                production ? "production" : "r19", static_cast<unsigned>(seed), result.prepared, cues.size(),
                result.failed, result.decoder_failures, result.heap_refused, result.refused_with_free_bytes,
                result.largest_refused, result.foreign_refused, result.live_peak, result.heap_min_free,
                result.retain_hits, result.region_reservations, result.region_overflows, result.region_min_free,
                result.first_error.c_str());
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 8 || (argc - 6) % 2 != 0) return 2;
    const std::string root = argv[1], cache_key = argv[2], sha = argv[3];
    const auto bytes = static_cast<std::uint32_t>(std::stoul(argv[4]));
    std::vector<std::uint32_t> seeds;
    for (const char* cursor = argv[5]; *cursor;) {
        char* end = nullptr;
        seeds.push_back(static_cast<std::uint32_t>(std::strtoul(cursor, &end, 0)));
        cursor = *end == ',' ? end + 1 : end;
    }
    std::vector<std::pair<std::string, unsigned>> cues;
    for (int i = 6; i + 1 < argc; i += 2) cues.push_back({argv[i], static_cast<unsigned>(std::stoul(argv[i + 1]))});

    for (const std::uint32_t seed : seeds) {
        const RunResult r19 = Play(false, seed, root, cache_key, sha, bytes, cues);
        Expect(r19.failed > 0 && r19.first_error.find("no memory") != std::string::npos,
               "R19 firmware: a cue fails with media: no memory");
        Expect(r19.refused_with_free_bytes > 0 && r19.largest_refused >= 256 * 1024,
               "  a large decoder request is refused while the heap holds more free bytes than it asks");

        const RunResult production = Play(true, seed, root, cache_key, sha, bytes, cues);
        Expect(production.failed == 0 && production.prepared == static_cast<int>(cues.size()),
               "production: every cue prepares and plays");
        Expect(production.decoder_failures == 0 && production.heap_refused == 0,
               "  no decoder request is refused");
        Expect(production.region_reservations == 1 && production.region_overflows == 0,
               "  one region per session serves every decoder block");
        Expect(production.retain_hits > 0, "  reopened originals reuse retained blocks");
        Expect(production.foreign_refused <= r19.foreign_refused, "  other heap owners are not refused more often");
    }
    std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
