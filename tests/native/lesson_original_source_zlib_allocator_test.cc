#include "lesson_original_source_allocator.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <zlib.h>
using namespace tbot;
// Pinned zlib built with MY_ZCALLOC must route default-allocator streams
// (Matroska zlib tracks, MOV cmov uncompress) through the bound owner.
struct Heap {
    size_t calls = 0, fail = 0, live = 0;
    static void* Allocate(void* context, size_t alignment, size_t bytes) {
        auto& h = *static_cast<Heap*>(context);
        if (++h.calls == h.fail) return nullptr;
        void* p = nullptr;
        if (posix_memalign(&p, alignment, bytes)) return nullptr;
        ++h.live;
        return p;
    }
    static void Release(void* context, void* p) { --static_cast<Heap*>(context)->live; free(p); }
    static size_t Charged(void*, void*, size_t bytes) { return bytes; }
};
// Matroska-style chunked Z_NO_FLUSH inflate, which also allocates the window.
static int Inflate(z_stream& z, const std::vector<uint8_t>& packed, std::vector<uint8_t>& out) {
    z.next_in = const_cast<uint8_t*>(packed.data()); z.avail_in = packed.size();
    int result = Z_OK;
    while (result == Z_OK && z.total_out < out.size()) {
        z.next_out = out.data() + z.total_out;
        z.avail_out = std::min<size_t>(4096, out.size() - z.total_out);
        result = inflate(&z, Z_NO_FLUSH);
    }
    return result;
}
int main() {
    std::vector<uint8_t> plain(64 * 1024);
    for (size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<uint8_t>((i * 131) ^ (i >> 7));
    std::vector<uint8_t> packed(compressBound(plain.size()));
    uLongf packed_size = packed.size();
    {
        // Routed zlib refuses while no owner is bound, so the fixture binds its own.
        OriginalSourceAllocationState state;
        Heap heap;
        OriginalSourceAllocator allocator(state, {&heap, Heap::Allocate, Heap::Release, Heap::Charged});
        assert(allocator.Bind());
        packed_size = packed.size();
        assert(compress2(packed.data(), &packed_size, plain.data(), plain.size(), 9) == Z_OK);
        assert(allocator.stats().live_allocations == 0 && allocator.Unbind());
    }
    packed.resize(packed_size);
    int failures = 0;
    {
        OriginalSourceAllocationState state;
        Heap heap;
        OriginalSourceAllocator allocator(state, {&heap, Heap::Allocate, Heap::Release, Heap::Charged});
        assert(allocator.Bind());
        z_stream z{};
        assert(inflateInit(&z) == Z_OK);
        if (allocator.stats().live_allocations == 0) {
            fprintf(stderr, "FAIL inflateInit default allocator bypasses owner\n");
            ++failures;
        }
        std::vector<uint8_t> out(plain.size());
        assert(Inflate(z, packed, out) == Z_STREAM_END && out == plain);
        assert(inflateEnd(&z) == Z_OK);
        assert(allocator.stats().live_allocations == 0 && heap.live == 0);
        const size_t before = heap.calls;
        uLongf out_size = out.size();
        std::fill(out.begin(), out.end(), 0);
        assert(uncompress(out.data(), &out_size, packed.data(), packed_size) == Z_OK);
        assert(out_size == plain.size() && out == plain);
        if (heap.calls == before) {
            fprintf(stderr, "FAIL uncompress default allocator bypasses owner\n");
            ++failures;
        }
        assert(allocator.stats().live_allocations == 0 && heap.live == 0 && !state.failed());
        assert(allocator.Unbind());
    }
    if (failures) return 1;
    // Each refused zlib allocation fails the shared owner and leaks nothing.
    for (int api = 0; api < 2; ++api) {
        for (size_t nth = 1;; ++nth) {
            OriginalSourceAllocationState state;
            Heap heap;
            heap.fail = nth;
            OriginalSourceAllocator allocator(state, {&heap, Heap::Allocate, Heap::Release, Heap::Charged});
            assert(allocator.Bind());
            std::vector<uint8_t> out(plain.size());
            int result;
            if (api == 0) {
                z_stream z{};
                result = inflateInit(&z);
                if (result == Z_OK) {
                    result = Inflate(z, packed, out);
                    inflateEnd(&z);
                }
            } else {
                uLongf out_size = out.size();
                result = uncompress(out.data(), &out_size, packed.data(), packed_size);
            }
            assert(allocator.stats().live_allocations == 0 && heap.live == 0);
            assert(allocator.Unbind());
            if (heap.calls < nth) {
                assert((result == Z_STREAM_END || result == Z_OK) && out == plain && !state.failed());
                printf("zlib api=%d refusal positions=%zu\n", api, nth - 1);
                break;
            }
            assert(result == Z_MEM_ERROR && state.failed());
        }
    }
    // Overflowing items*size is refused before reaching the backend.
    {
        OriginalSourceAllocationState state;
        Heap heap;
        OriginalSourceAllocator allocator(state, {&heap, Heap::Allocate, Heap::Release, Heap::Charged});
        assert(allocator.Bind());
        if (sizeof(size_t) <= sizeof(unsigned)) {
            assert(!tbot_original_zcalloc(nullptr, ~0u, 2) && heap.calls == 0 && state.failed());
        }
        void* p = tbot_original_zcalloc(nullptr, 3, 5);
        assert(p && allocator.stats().live_requested == 15);
        tbot_original_zcfree(nullptr, p);
        assert(allocator.stats().live_allocations == 0);
        assert(allocator.Unbind());
    }
    puts("PASS pinned zlib default allocator routes inflate/uncompress through owner with exhaustive refusal");
}
