#include "lesson_original_source_allocator.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <limits>
#include <vector>
#include <algorithm>
#include <map>
#include <sanitizer/asan_interface.h>

using namespace tbot;
struct Heap {
    size_t calls = 0, fail = 0, live = 0, charge_override = 0;
    bool fail_on_free = false;
    static void* Allocate(void* context, size_t alignment, size_t bytes) {
        auto& h = *static_cast<Heap*>(context);
        if (++h.calls == h.fail) return nullptr;
        void* p = nullptr;
        assert(posix_memalign(&p, alignment, bytes) == 0);
        ++h.live;
        return p;
    }
    static void Free(void* context, void* p) {
        auto& h = *static_cast<Heap*>(context);
        assert(h.live > 0);
        --h.live;
        if (h.fail_on_free) tbot_original_allocation_failure();
        free(p);
    }
    static size_t Charged(void* context, void*, size_t bytes) {
        auto& h = *static_cast<Heap*>(context);
        return h.charge_override ? h.charge_override : bytes;
    }
    OriginalSourceAllocatorBackend backend() {
        return {this, Allocate, Free, Charged};
    }
};

// Inner heap for the retaining backend: counts calls and live blocks, refuses on demand,
// and can return blocks aligned to exactly the requested alignment (never more).
struct CountingHeap {
    size_t calls = 0, live = 0, live_bytes = 0;
    size_t refuse = 0;  // refuse the next `refuse` calls
    bool exact_alignment = false;
    std::map<void*, std::pair<void*, size_t>> blocks;  // raw -> (malloc base, bytes)
    static void* Allocate(void* context, size_t alignment, size_t bytes) {
        auto& h = *static_cast<CountingHeap*>(context);
        ++h.calls;
        if (h.refuse) { --h.refuse; return nullptr; }
        void* base = nullptr;
        const size_t pad = h.exact_alignment ? alignment : 0;
        assert(posix_memalign(&base, h.exact_alignment ? alignment * 2 : alignment, bytes + pad) == 0);
        void* raw = static_cast<uint8_t*>(base) + pad;
        h.blocks[raw] = {base, bytes};
        ++h.live; h.live_bytes += bytes;
        return raw;
    }
    static void Free(void* context, void* raw) {
        auto& h = *static_cast<CountingHeap*>(context);
        auto found = h.blocks.find(raw);
        assert(found != h.blocks.end());
        --h.live; h.live_bytes -= found->second.second;
        free(found->second.first);
        h.blocks.erase(found);
    }
    static size_t Charged(void* context, void* raw, size_t) {
        return static_cast<CountingHeap*>(context)->blocks.at(raw).second;
    }
    OriginalSourceAllocatorBackend backend() { return {this, Allocate, Free, Charged}; }
};

void RetentionTests() {
    constexpr size_t kBig = 200 * 1024, kLimit = 1024 * 1024;
    OriginalSourceAllocationState state;
    CountingHeap heap;
    {
        OriginalSourceRetainingBackend pool(heap.backend(), kLimit, 64 * 1024, 4);
        OriginalSourceAllocator allocator(state, pool.backend());
        assert(allocator.Bind());
        // Small requests bypass the pool entirely.
        void* small = tbot_original_malloc(1000);
        tbot_original_free(small);
        assert(heap.live == 0 && pool.stats().retained_blocks == 0 && pool.stats().misses == 0);
        // A freed large block serves the next request of the same size without a heap call.
        void* first = tbot_original_malloc(kBig);
        memset(first, 0x11, kBig);
        const size_t charged = allocator.stats().live_charged, calls = heap.calls;
        tbot_original_free(first);
        assert(heap.live == 1 && pool.stats().retained_blocks == 1 && pool.stats().retained_charged == charged);
        assert(allocator.stats().live_charged == 0 && allocator.stats().live_allocations == 0);
        assert(__asan_address_is_poisoned(static_cast<uint8_t*>(first) + kBig / 2));
        void* again = tbot_original_malloc(kBig);
        assert(again == first && heap.calls == calls && pool.stats().hits == 1);
        assert(!__asan_address_is_poisoned(static_cast<uint8_t*>(again) + kBig - 1));
        assert(allocator.stats().live_charged == charged && pool.stats().retained_blocks == 0);
        memset(again, 0x22, kBig);
        // Within 1/8 slack a smaller request reuses the block; beyond it, it does not.
        tbot_original_free(again);
        void* smaller = tbot_original_malloc(kBig - kBig / 10);
        assert(smaller == first && heap.calls == calls);
        tbot_original_free(smaller);
        void* much_smaller = tbot_original_malloc(kBig / 2);
        assert(much_smaller != first && heap.calls == calls + 1 && heap.live == 2);
        // Best fit: the smallest retained block that fits within slack.
        void* other = tbot_original_malloc(kBig + kBig / 16);
        tbot_original_free(much_smaller);
        tbot_original_free(other);
        void* best = tbot_original_malloc(kBig);
        assert(best == first);
        tbot_original_free(best);
        // A retained block whose raw address lacks the requested alignment is not reused.
        pool.ReleaseRetained();
        assert(heap.live == 0 && pool.stats().retained_charged == 0);
        heap.exact_alignment = true;
        // The 64-aligned block is large enough for the 4096-aligned request (offset 4096).
        void* aligned64 = tbot_original_memalign(64, kBig + 8192);
        tbot_original_free(aligned64);
        const size_t aligned_calls = heap.calls;
        void* aligned4096 = tbot_original_memalign(4096, kBig);
        assert(aligned4096 && reinterpret_cast<uintptr_t>(aligned4096) % 4096 == 0);
        assert(heap.calls == aligned_calls + 1 && pool.stats().retained_blocks == 1);
        assert(!state.failed());
        tbot_original_free(aligned4096);
        heap.exact_alignment = false;
        pool.ReleaseRetained();
        assert(heap.live == 0);
        // Retained bytes never exceed the limit: the oldest retained block is released.
        std::vector<void*> blocks;
        for (int i = 0; i < 6; ++i) blocks.push_back(tbot_original_malloc(kBig + i * 30 * 1024));
        for (void* block : blocks) {
            tbot_original_free(block);
            assert(pool.stats().retained_charged <= kLimit);
        }
        assert(pool.stats().retained_blocks < 6 && heap.live == pool.stats().retained_blocks);
        // The first-freed (oldest) block is the one released.
        const size_t hits = pool.stats().hits;
        void* oldest = tbot_original_malloc(kBig);
        assert(pool.stats().hits == hits && oldest);
        tbot_original_free(oldest);
        pool.ReleaseRetained();
        // A block larger than the limit is never retained.
        void* huge = tbot_original_malloc(kLimit + 1);
        tbot_original_free(huge);
        assert(heap.live == 0 && pool.stats().retained_blocks == 0);
        // A miss releases blocks not reused for more than `stale_after` (4) large requests.
        void* keep = tbot_original_malloc(kBig);  // request n
        tbot_original_free(keep);                  // retained at n
        for (int i = 0; i < 4; ++i) {             // requests n+1 (miss) .. n+4 (hits)
            void* reused = tbot_original_malloc(2 * kBig);
            tbot_original_free(reused);           // retained again at n+1 .. n+4
        }
        assert(pool.stats().retained_blocks == 2 && heap.live == 2);
        void* fresh = tbot_original_malloc(3 * kBig);  // n+5 misses: kBig is 5 requests old
        assert(pool.stats().retained_blocks == 1 && heap.live == 2);
        assert(heap.blocks.count(static_cast<uint8_t*>(keep) - 64) == 0);
        tbot_original_free(fresh);
        pool.ReleaseRetained();
        // A refused miss releases every retained block and retries once.
        void* held = tbot_original_malloc(kBig);
        tbot_original_free(held);
        heap.refuse = 1;
        const size_t flushes = pool.stats().refusal_flushes;
        void* retried = tbot_original_malloc(kBig * 2);
        assert(retried && pool.stats().refusal_flushes == flushes + 1 && pool.stats().retained_blocks == 0);
        assert(!state.failed() && allocator.stats().failures == 0);
        // Retry refused too: the allocator reports the failure and the session latch sets.
        tbot_original_free(retried);
        heap.refuse = 2;
        assert(tbot_original_malloc(kBig * 3) == nullptr);
        assert(state.failed() && allocator.stats().failures == 1 && heap.live == 0);
        // Without retained blocks a refusal is not retried.
        heap.refuse = 1;
        const size_t before = heap.calls;
        assert(tbot_original_malloc(kBig * 3) == nullptr && heap.calls == before + 1);
        assert(allocator.stats().failures == 2);
        // Realloc keeps contents and alignment across pooled blocks.
        void* grown = tbot_original_memalign(256, kBig);
        memset(grown, 0x5c, kBig);
        void* moved = tbot_original_realloc(grown, 2 * kBig);
        assert(moved && reinterpret_cast<uintptr_t>(moved) % 256 == 0);
        for (size_t i = 0; i < kBig; ++i) assert(static_cast<uint8_t*>(moved)[i] == 0x5c);
        void* back = tbot_original_realloc(moved, kBig);  // reuses the retained first block
        assert(back == grown);
        for (size_t i = 0; i < kBig; ++i) assert(static_cast<uint8_t*>(back)[i] == 0x5c);
        tbot_original_free(back);
        // Deterministic schedules with refusals: the heap holds exactly live + retained blocks.
        struct Entry { void* ptr; size_t bytes; uint8_t pattern; };
        std::vector<Entry> entries;
        uint32_t seed = 0x19A1;
        for (unsigned step = 0; step < 4000; ++step) {
            seed = seed * 1664525u + 1013904223u;
            if (entries.empty() || (seed % 2 == 0 && entries.size() < 40)) {
                const size_t bytes = (seed >> 7) % 4 == 0 ? (seed >> 9) % 4096 : 60 * 1024 + (seed >> 9) % (400 * 1024);
                heap.refuse = (seed >> 3) % 23 == 0 ? 1 + (seed >> 5) % 2 : 0;
                void* p = tbot_original_memalign(size_t{64} << ((seed >> 11) % 4), bytes);
                heap.refuse = 0;
                if (!p) continue;
                const uint8_t pattern = seed & 255;
                memset(p, pattern, bytes);
                entries.push_back({p, bytes, pattern});
            } else {
                const size_t index = (seed >> 8) % entries.size();
                for (size_t i = 0; i < entries[index].bytes; i += 997)
                    assert(static_cast<uint8_t*>(entries[index].ptr)[i] == entries[index].pattern);
                tbot_original_free(entries[index].ptr);
                entries.erase(entries.begin() + index);
            }
            if (seed % 101 == 0) pool.ReleaseRetained();
            assert(heap.live == entries.size() + pool.stats().retained_blocks);
            assert(pool.stats().retained_charged <= kLimit);
            assert(allocator.stats().live_allocations == entries.size());
        }
        for (const auto& e : entries) tbot_original_free(e.ptr);
        assert(heap.live == pool.stats().retained_blocks);
        assert(allocator.Unbind());
        pool.ReleaseRetained();
        assert(heap.live == 0 && heap.live_bytes == 0);
        // Destruction returns retained blocks to the inner backend.
        assert(allocator.Bind());
        void* last = tbot_original_malloc(kBig);
        tbot_original_free(last);
        assert(heap.live == 1);
        assert(allocator.Unbind());
    }
    assert(heap.live == 0);
    // An incomplete inner backend yields an incomplete backend, which Bind refuses.
    OriginalSourceRetainingBackend broken({}, kLimit);
    OriginalSourceAllocationState broken_state;
    OriginalSourceAllocator unbound(broken_state, broken.backend());
    assert(!unbound.Bind());
}

// Region heap stand-in: a first-fit list over the span the region backend reserved.
struct SpanHeap {
    uint8_t* base = nullptr;
    size_t bytes = 0, min_free = 0;
    std::map<size_t, size_t> used;  // offset -> bytes
    bool refuse_create = false;
    static SpanHeap* current;
    static void* Create(void* base, size_t bytes) {
        if (current->refuse_create) return nullptr;
        current->base = static_cast<uint8_t*>(base);
        current->bytes = bytes; current->min_free = bytes; current->used.clear();
        return current;
    }
    static void* Allocate(void* heap, size_t alignment, size_t bytes) {
        auto& h = *static_cast<SpanHeap*>(heap);
        size_t cursor = 0;
        for (auto it = h.used.begin();; ++it) {
            const size_t start = (cursor + alignment - 1) & ~(alignment - 1);
            const size_t end = it == h.used.end() ? h.bytes : it->first;
            if (start + bytes <= end) {
                h.used[start] = bytes;
                size_t in_use = 0;
                for (const auto& u : h.used) in_use += u.second;
                h.min_free = std::min(h.min_free, h.bytes - in_use);
                return h.base + start;
            }
            if (it == h.used.end()) return nullptr;
            cursor = it->first + it->second;
        }
    }
    static void Release(void* heap, void* ptr) {
        auto& h = *static_cast<SpanHeap*>(heap);
        assert(h.used.erase(static_cast<uint8_t*>(ptr) - h.base) == 1);
    }
    static size_t Charged(void* heap, void* ptr) {
        auto& h = *static_cast<SpanHeap*>(heap);
        return h.used.at(static_cast<uint8_t*>(ptr) - h.base);
    }
    static size_t MinFree(void* heap) { return static_cast<SpanHeap*>(heap)->min_free; }
    static OriginalSourceRegionHeapOps ops() { return {Create, Allocate, Release, Charged, MinFree}; }
};
SpanHeap* SpanHeap::current = nullptr;

void RegionTests() {
    constexpr size_t kRegion = 1024 * 1024;
    SpanHeap span;
    SpanHeap::current = &span;
    CountingHeap heap;
    OriginalSourceAllocationState state;
    {
        OriginalSourceRegionBackend region(heap.backend(), SpanHeap::ops(), kRegion);
        OriginalSourceAllocator allocator(state, region.backend());
        assert(allocator.Bind());
        assert(region.stats().reserved_bytes == 0 && heap.live == 0);
        // The first allocation reserves the region; requests are served inside it.
        void* a = tbot_original_memalign(256, 300 * 1024);
        assert(a && reinterpret_cast<uintptr_t>(a) % 256 == 0);
        assert(heap.live == 1 && heap.calls == 1 && region.stats().reserved_bytes == kRegion);
        assert(static_cast<uint8_t*>(a) >= span.base && static_cast<uint8_t*>(a) < span.base + kRegion);
        assert(allocator.stats().live_charged == SpanHeap::Charged(&span, static_cast<uint8_t*>(a) - 256));
        void* b = tbot_original_malloc(500 * 1024);
        assert(heap.live == 1 && region.stats().live_blocks == 2);
        // A request the region cannot fit overflows to the inner heap.
        void* c = tbot_original_malloc(400 * 1024);
        assert(c && heap.live == 2 && region.stats().overflows == 1);
        assert(static_cast<uint8_t*>(c) < span.base || static_cast<uint8_t*>(c) >= span.base + kRegion);
        // Inner refusal of an overflow is an allocation failure.
        heap.refuse = 1;
        assert(tbot_original_malloc(600 * 1024) == nullptr && state.failed());
        // The region stays while blocks live in it.
        tbot_original_free(c);
        assert(heap.live == 1);
        assert(!region.ReleaseRegion() && region.stats().reserved_bytes == kRegion);
        tbot_original_free(a);
        tbot_original_free(b);
        assert(region.stats().live_blocks == 0 && region.stats().region_min_free < kRegion);
        assert(region.ReleaseRegion() && heap.live == 0 && region.stats().reserved_bytes == 0);
        // The next session reserves again.
        void* d = tbot_original_malloc(100 * 1024);
        assert(heap.live == 1 && region.stats().reservations == 2);
        tbot_original_free(d);
        assert(region.ReleaseRegion() && heap.live == 0);
        // A refused reservation is attempted once per session; requests use the inner heap.
        heap.refuse = 1;
        const size_t calls = heap.calls;
        void* e = tbot_original_malloc(100 * 1024);
        void* f = tbot_original_malloc(100 * 1024);
        assert(e && f && heap.calls == calls + 3 && heap.live == 2);
        assert(region.stats().reservation_failures == 1 && region.stats().reserved_bytes == 0);
        tbot_original_free(e);
        tbot_original_free(f);
        assert(region.ReleaseRegion());
        // A heap that cannot be created over the span returns it.
        span.refuse_create = true;
        void* g = tbot_original_malloc(100 * 1024);
        assert(g && heap.live == 1 && region.stats().reservation_failures == 2);
        tbot_original_free(g);
        span.refuse_create = false;
        assert(region.ReleaseRegion() && heap.live == 0);
        // The retaining backend over the region keeps large blocks inside it.
        OriginalSourceRetainingBackend pool(region.backend(), kRegion, 256 * 1024);
        assert(allocator.Unbind());
        OriginalSourceAllocator pooled(state, pool.backend());
        assert(pooled.Bind());
        void* big = tbot_original_malloc(300 * 1024);
        tbot_original_free(big);
        assert(pool.stats().retained_blocks == 1 && region.stats().live_blocks == 1);
        assert(!region.ReleaseRegion());  // a retained block still lives in the region
        assert(tbot_original_malloc(300 * 1024) == big);
        tbot_original_free(big);
        pool.ReleaseRetained();
        assert(region.ReleaseRegion() && heap.live == 0);
        assert(pooled.Unbind());
    }
    // An incomplete inner backend yields an incomplete backend, which Bind refuses.
    OriginalSourceRegionBackend broken({}, SpanHeap::ops(), kRegion);
    OriginalSourceAllocationState broken_state;
    OriginalSourceAllocator unbound(broken_state, broken.backend());
    assert(!unbound.Bind());
    // Without heap operations every request goes to the inner backend.
    {
        OriginalSourceRegionBackend plain(heap.backend(), {}, kRegion);
        OriginalSourceAllocationState plain_state;
        OriginalSourceAllocator allocator(plain_state, plain.backend());
        assert(allocator.Bind());
        void* p = tbot_original_malloc(100 * 1024);
        assert(p && heap.live == 1 && plain.stats().reserved_bytes == 0);
        tbot_original_free(p);
        assert(allocator.Unbind());
    }
    assert(heap.live == 0);
}

int main() {
    assert(tbot_original_malloc(1) == nullptr);
    OriginalSourceAllocationState state, other_state;
    Heap heap, other_heap;
    OriginalSourceAllocator allocator(state, heap.backend());
    OriginalSourceAllocator other(other_state, other_heap.backend());
    assert(allocator.Bind());
    assert(!other.Bind());
    void* p = tbot_original_malloc(23);
    assert(p && reinterpret_cast<uintptr_t>(p) % 64 == 0);
    memset(p, 0xa5, 23);
    assert(allocator.stats().live_requested == 23);
    assert(allocator.stats().live_allocations == 1);
    assert(allocator.stats().live_charged > 23);
    const auto charged = allocator.stats().live_charged;
    assert(!allocator.Unbind());
    heap.fail = heap.calls + 1;
    assert(tbot_original_realloc(p, 1024) == nullptr);
    assert(state.failed() && allocator.stats().failures == 1);
    assert(allocator.stats().live_requested == 23 && heap.live == 1);
    for (size_t i = 0; i < 23; ++i) assert(static_cast<uint8_t*>(p)[i] == 0xa5);
    heap.fail = 0;
    void* grown = tbot_original_realloc(p, 1024);
    assert(grown && reinterpret_cast<uintptr_t>(grown) % 64 == 0);
    for (size_t i = 0; i < 23; ++i) assert(static_cast<uint8_t*>(grown)[i] == 0xa5);
    assert(allocator.stats().peak_requested == 1047);
    assert(allocator.stats().peak_charged >= allocator.stats().live_charged + charged);
    allocator.ResetPeak();
    assert(allocator.stats().peak_requested == 1024);
    assert(allocator.stats().peak_charged == allocator.stats().live_charged);
    p = tbot_original_realloc(grown, 7);
    assert(p && allocator.stats().live_requested == 7);
    for (size_t i = 0; i < 7; ++i) assert(static_cast<uint8_t*>(p)[i] == 0xa5);
    assert(tbot_original_realloc(p, 0) == nullptr);
    assert(heap.live == 0 && allocator.stats().live_charged == 0);
    tbot_original_free(nullptr);
    p = tbot_original_malloc(0);
    assert(p && allocator.stats().live_requested == 0 && heap.live == 1);
    tbot_original_free(p);
    assert(allocator.Unbind());
    assert(state.failed()); // Allocator lifecycle never clears a session's fault.
    assert(other.Bind());
    void* sentinel = reinterpret_cast<void*>(uintptr_t{1});
    assert(tbot_original_posix_memalign(&sentinel, 3, 12) == EINVAL);
    assert(sentinel == reinterpret_cast<void*>(uintptr_t{1}));
    for (size_t alignment : {sizeof(void*), size_t{16}, size_t{64}, size_t{256}, size_t{4096}}) {
        p = nullptr;
        assert(tbot_original_posix_memalign(&p, alignment, 4097) == 0);
        assert(reinterpret_cast<uintptr_t>(p) % alignment == 0);
        memset(p, 0x39, 4097);
        grown = tbot_original_realloc(p, 8193);
        assert(grown && reinterpret_cast<uintptr_t>(grown) % alignment == 0);
        for (size_t i = 0; i < 4097; ++i) assert(static_cast<uint8_t*>(grown)[i] == 0x39);
        tbot_original_free(grown);
    }
    assert(tbot_original_malloc(std::numeric_limits<size_t>::max()) == nullptr);
    assert(other_state.failed());
    other_heap.fail = other_heap.calls + 1;
    sentinel = reinterpret_cast<void*>(uintptr_t{1});
    assert(tbot_original_posix_memalign(&sentinel, 64, 100) == ENOMEM);
    assert(sentinel == reinterpret_cast<void*>(uintptr_t{1}));
    other_heap.fail = 0;
    // Deterministic schedules use a separate payload/count model after every mutation.
    struct Entry { void* ptr; size_t bytes, alignment; uint8_t pattern; };
    std::vector<Entry> entries;
    uint32_t seed = 0xB008;
    for (unsigned step = 0; step < 3000; ++step) {
        seed = seed * 1664525u + 1013904223u;
        if (entries.empty() || (seed % 3 == 0 && entries.size() < 32)) {
            size_t bytes = (seed >> 5) % 4096, alignment = size_t{16} << ((seed >> 3) % 5);
            p = tbot_original_memalign(alignment, bytes);
            assert(p); uint8_t pattern = seed & 255;
            memset(p, pattern, bytes); entries.push_back({p, bytes, alignment, pattern});
        } else {
            size_t index = (seed >> 8) % entries.size();
            auto& e = entries[index];
            if (seed % 3 == 1) {
                size_t bytes = 1 + ((seed >> 12) % 8192);
                grown = tbot_original_realloc(e.ptr, bytes); assert(grown);
                for (size_t i = 0; i < std::min(e.bytes, bytes); ++i)
                    assert(static_cast<uint8_t*>(grown)[i] == e.pattern);
                assert(reinterpret_cast<uintptr_t>(grown) % e.alignment == 0);
                memset(grown, e.pattern, bytes); e.ptr = grown; e.bytes = bytes;
            } else { tbot_original_free(e.ptr); entries.erase(entries.begin() + index); }
        }
        size_t payload = 0;
        for (const auto& e : entries) payload += e.bytes;
        assert(other.stats().live_requested == payload);
        assert(other.stats().live_allocations == entries.size() && other_heap.live == entries.size());
        assert(other.stats().live_charged >= payload);
    }
    for (auto e : entries) tbot_original_free(e.ptr);
    assert(other.stats().live_allocations == 0 && other.stats().live_requested == 0);
    assert(other.stats().live_charged == 0 && other_heap.live == 0);
    // Backend-reported charge is bounded before aggregate accounting, too.
    other_heap.charge_override = std::numeric_limits<size_t>::max();
    p = tbot_original_malloc(1); assert(p);
    assert(other.stats().live_charged == std::numeric_limits<size_t>::max());
    assert(tbot_original_malloc(1) == nullptr && other_heap.live == 1);
    tbot_original_free(p);
    other_heap.charge_override = 1;
    assert(tbot_original_malloc(1) == nullptr && other_heap.live == 0);
    other_heap.charge_override = 0;
    const size_t before_cleanup = other.stats().failures;
    p = tbot_original_malloc(32); assert(p);
    other_heap.fail_on_free = true;
    tbot_original_free(p);
    other_heap.fail_on_free = false;
    assert(other.stats().failures == before_cleanup + 1 && other_state.failed());
    assert(other.Unbind());
    RetentionTests();
    RegionTests();
    puts("PASS allocator: ownership/alignment/refusal/realloc/zero/overflow and 3000 accounting schedules");
    puts("PASS retention: reuse/slack/best-fit/alignment/limit/stale/refusal-flush/realloc and 4000 schedules");
    puts("PASS region: reserve/serve/overflow/refusal/release-with-live/re-reserve/reserve-failure/pool-over-region");
}
