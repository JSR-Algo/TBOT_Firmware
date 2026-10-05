#include "lesson_original_source_allocator.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cstdio>
#include <cerrno>
#include <climits>
extern "C" {
#include <libavutil/mem.h>
}
using namespace tbot;
static void* Allocate(void*, size_t alignment, size_t bytes) {
    void* ptr = nullptr;
    assert(posix_memalign(&ptr, alignment, bytes) == 0);
    return ptr;
}
static void Release(void*, void* ptr) { free(ptr); }
static size_t Charged(void*, void*, size_t bytes) { return bytes; }
int main() {
    const size_t max = std::numeric_limits<size_t>::max();
    // Doubling stays within int, but the pointer table exceeds INT_MAX bytes.
    const int huge_count = 1 << 29;
    int bypassed = 0;
    for (int scenario = 0; scenario < 13; ++scenario) {
        OriginalSourceAllocationState state;
        OriginalSourceAllocator allocator(state, {nullptr, Allocate, Release, Charged});
        assert(allocator.Bind());
        av_max_alloc(1024);
        void* ptr = av_malloc(64); assert(ptr);
        memset(ptr, 0x6a, 64);
        unsigned capacity = 64;
        void* result = nullptr;
        switch (scenario) {
            case 0: result = av_malloc(1025); break;
            case 1: result = av_realloc(ptr, 1025); break;
            case 2: result = av_malloc_array(max, 2); break;
            case 3: result = av_realloc_array(ptr, max, 2); break;
            case 4: result = av_calloc(max, 2); break;
            case 5: result = av_realloc_f(ptr, max, 2); ptr = nullptr; break;
            case 6: assert(av_reallocp_array(&ptr, max, 2) < 0); break;
            case 7: result = av_fast_realloc(ptr, &capacity, 1025); assert(capacity == 0); break;
            case 8: av_fast_malloc(&ptr, &capacity, 1025); assert(!ptr && capacity == 0); break;
            case 9: av_fast_mallocz(&ptr, &capacity, 1025); assert(!ptr && capacity == 0); break;
            case 10: { int count = huge_count; assert(av_dynarray_add_nofree(&ptr, &count, nullptr) < 0); break; }
            case 11: { int count = huge_count; av_dynarray_add(&ptr, &count, nullptr); assert(!ptr && count == 0); break; }
            case 12: { int count = 1; assert(!av_dynarray2_add(&ptr, &count, max, nullptr)); assert(!ptr && count == 0); break; }
        }
        assert(result == nullptr);
        if (!state.failed()) {
            fprintf(stderr, "FAIL early refusal bypasses shared allocation state: scenario=%d\n", scenario);
            ++bypassed;
        }
        if (ptr) for (size_t i = 0; i < 64; ++i) assert(static_cast<uint8_t*>(ptr)[i] == 0x6a);
        av_free(ptr);
        assert(allocator.stats().live_allocations == 0 && allocator.stats().live_charged == 0);
        assert(allocator.Unbind());
    }
    if (bypassed) return 1;
    // Utility multiplication and legal zero operations must not invent OOM.
    OriginalSourceAllocationState state;
    OriginalSourceAllocator allocator(state, {nullptr, Allocate, Release, Charged});
    assert(allocator.Bind());
    av_max_alloc(1024);
    size_t multiplied = 0;
    assert(av_size_mult(max, 2, &multiplied) < 0 && !state.failed());
    void* ptr = av_malloc(0); assert(ptr && !state.failed()); av_free(ptr);
    ptr = av_realloc(nullptr, 0); assert(ptr && !state.failed()); av_free(ptr);
    ptr = av_malloc(8); assert(ptr);
    assert(av_reallocp(&ptr, 0) == 0 && !ptr && !state.failed());
    // Invalid posix_memalign alignment is a refusal reaching av_malloc callers.
    void* sentinel = &multiplied;
    assert(tbot_original_posix_memalign(&sentinel, 3, 8) == EINVAL && sentinel == &multiplied);
    assert(state.failed());
    av_max_alloc(INT_MAX);
    assert(allocator.Unbind());
    puts("PASS actual pinned FFmpeg: 13 early refusal paths, exact ownership, utility and zero semantics");
}
