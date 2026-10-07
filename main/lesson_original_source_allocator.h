#ifndef LESSON_ORIGINAL_SOURCE_ALLOCATOR_H
#define LESSON_ORIGINAL_SOURCE_ALLOCATOR_H
#include "lesson_original_source_session.h"
#include <cstddef>
#include <cstdint>
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
struct OriginalSourceRetentionStats {
    size_t retained_charged = 0, retained_blocks = 0;
    size_t hits = 0, misses = 0, refusal_flushes = 0;
};
// Backend decorator that keeps freed large blocks for reuse. Reopening an original
// requests the same large blocks again (file snapshot, H264 context, NAL buffer); a PSRAM
// heap fragmented by other owners in between can refuse them although enough bytes are
// free (BE08 R19). Only blocks of at least `min_block` are kept: retaining the smaller
// per-clip frame buffers held ~230 KB beyond the decoder peak and caused more refusals in
// the TLSF model than it prevented. A retained block serves a request it fits within 1/8
// slack. A miss first releases blocks not reused for `stale_after` large requests; a
// refused miss releases every retained block and retries once. Retained bytes never exceed
// `limit`. It shares the allocator's serialized owner and outlives it; ReleaseRetained
// returns every retained block to the inner backend.
class OriginalSourceRetainingBackend {
public:
    static constexpr size_t kMinBlockBytes = 256 * 1024;
    static constexpr size_t kSlots = 64;
    OriginalSourceRetainingBackend(OriginalSourceAllocatorBackend inner, size_t limit,
                                   size_t min_block = kMinBlockBytes, size_t stale_after = 32);
    ~OriginalSourceRetainingBackend();
    OriginalSourceRetainingBackend(const OriginalSourceRetainingBackend&) = delete;
    OriginalSourceRetainingBackend& operator=(const OriginalSourceRetainingBackend&) = delete;
    OriginalSourceAllocatorBackend backend();
    void ReleaseRetained();
    const OriginalSourceRetentionStats& stats() const { return stats_; }
private:
    struct Slot {
        void* raw = nullptr;
        size_t total = 0, charged = 0, stamp = 0;
        bool retained = false;
    };
    void* Allocate(size_t alignment, size_t bytes);
    void Release(void* raw);
    size_t Charged(void* raw, size_t requested);
    Slot* Find(const void* raw);
    void Drop(Slot&);
    OriginalSourceAllocatorBackend inner_;
    size_t limit_, min_block_, stale_after_, clock_ = 0;
    Slot slots_[kSlots];
    OriginalSourceRetentionStats stats_;
};
struct OriginalSourceRegionStats {
    size_t reserved_bytes = 0, live_blocks = 0, region_min_free = 0;
    size_t reservations = 0, reservation_failures = 0, overflows = 0;
};
// Backend decorator that serves decoder blocks from one dedicated PSRAM region, reserved
// from the inner backend at the first allocation after a release (the start of a lesson
// session, before other owners interleave with decoder blocks) and managed by ESP-IDF's
// TLSF multi_heap. A request the region cannot fit, or every request when the region could
// not be reserved, goes to the inner backend. ReleaseRegion returns the region once no
// block lives in it. Same serialized owner and lifetime rules as the retaining backend.
// A heap over one caller-provided span (ESP-IDF multi_heap on the device).
struct OriginalSourceRegionHeapOps {
    void* (*create)(void* base, size_t bytes);
    void* (*allocate)(void* heap, size_t alignment, size_t bytes);
    void (*release)(void* heap, void* ptr);
    size_t (*charged)(void* heap, void* ptr);
    size_t (*min_free)(void* heap);
};
class OriginalSourceRegionBackend {
public:
    OriginalSourceRegionBackend(OriginalSourceAllocatorBackend inner, OriginalSourceRegionHeapOps ops,
                                size_t region_bytes);
    static OriginalSourceRegionHeapOps EspHeapOps();
    ~OriginalSourceRegionBackend();
    OriginalSourceRegionBackend(const OriginalSourceRegionBackend&) = delete;
    OriginalSourceRegionBackend& operator=(const OriginalSourceRegionBackend&) = delete;
    OriginalSourceAllocatorBackend backend();
    // False while blocks live in the region; it is then released by a later call.
    bool ReleaseRegion();
    const OriginalSourceRegionStats& stats() const;
private:
    void* Allocate(size_t alignment, size_t bytes);
    void Release(void* raw);
    size_t Charged(void* raw, size_t requested);
    bool Contains(const void* raw) const;
    OriginalSourceAllocatorBackend inner_;
    OriginalSourceRegionHeapOps ops_;
    size_t region_bytes_;
    uint8_t* base_ = nullptr;
    void* heap_ = nullptr;
    bool attempted_ = false;
    mutable OriginalSourceRegionStats stats_;
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
