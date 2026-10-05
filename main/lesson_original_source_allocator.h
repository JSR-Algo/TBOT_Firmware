#ifndef LESSON_ORIGINAL_SOURCE_ALLOCATOR_H
#define LESSON_ORIGINAL_SOURCE_ALLOCATOR_H
#include "lesson_original_source_session.h"
#include <cstddef>
namespace tbot {
struct OriginalSourceAllocatorBackend {
    void* context;
    void* (*allocate)(void*, size_t alignment, size_t bytes);
    void (*release)(void*, void*);
    size_t (*charged)(void*, void*, size_t requested);
};
struct OriginalSourceAllocatorStats {
    size_t live_requested = 0, peak_requested = 0;
    size_t live_charged = 0, peak_charged = 0;
    size_t live_allocations = 0, failures = 0;
};
// Bind, hooks, stats and destruction share one serialized owner. The allocation
// state and backend outlive the allocator. Unbind requires every buffer drained;
// allocator lifecycle never resets the session's sticky allocation failure.
class OriginalSourceAllocator {
public:
    OriginalSourceAllocator(OriginalSourceAllocationState&, OriginalSourceAllocatorBackend);
    ~OriginalSourceAllocator();
    OriginalSourceAllocator(const OriginalSourceAllocator&) = delete;
    OriginalSourceAllocator& operator=(const OriginalSourceAllocator&) = delete;
    bool Bind();
    bool Unbind();
    const OriginalSourceAllocatorStats& stats() const { return stats_; }
    // Restart peak measurement from the current live bytes (e.g. per session).
    void ResetPeak();
    void* Allocate(size_t bytes, size_t alignment = 64);
    void* Reallocate(void*, size_t bytes);
    void Free(void*);
    void NotifyFailure();
    static OriginalSourceAllocatorBackend EspBackend();
private:
    struct Header;
    OriginalSourceAllocationState& failure_;
    OriginalSourceAllocatorBackend backend_;
    OriginalSourceAllocatorStats stats_;
};
}
extern "C" {
void* tbot_original_malloc(size_t);
void* tbot_original_memalign(size_t alignment, size_t bytes);
int tbot_original_posix_memalign(void**, size_t alignment, size_t bytes);
void* tbot_original_realloc(void*, size_t bytes);
void tbot_original_free(void*);
void tbot_original_allocation_failure();
// zlib MY_ZCALLOC hooks for streams that keep the default allocator.
void* tbot_original_zcalloc(void* opaque, unsigned items, unsigned size);
void tbot_original_zcfree(void* opaque, void* ptr);
}
#endif
