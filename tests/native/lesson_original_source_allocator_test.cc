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
    puts("PASS allocator: ownership/alignment/refusal/realloc/zero/overflow and 3000 accounting schedules");
}
