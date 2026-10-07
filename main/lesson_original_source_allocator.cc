#include "lesson_original_source_allocator.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "multi_heap.h"
#endif
#if defined(__SANITIZE_ADDRESS__)
#define TBOT_ORIGINAL_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TBOT_ORIGINAL_ASAN 1
#endif
#endif
#ifdef TBOT_ORIGINAL_ASAN
#include <sanitizer/asan_interface.h> // Retained blocks stay poisoned until reused.
#else
#define ASAN_POISON_MEMORY_REGION(address, size) ((void)(address), (void)(size))
#define ASAN_UNPOISON_MEMORY_REGION(address, size) ((void)(address), (void)(size))
#endif
namespace {
tbot::OriginalSourceAllocator* owner = nullptr;
bool PowerOfTwo(size_t value) { return value && !(value & (value - 1)); }
}
namespace tbot {
struct OriginalSourceAllocator::Header {
    OriginalSourceAllocator* allocator;
    void* raw;
    size_t requested, charged, alignment;
};
OriginalSourceAllocator::OriginalSourceAllocator(OriginalSourceAllocationState& state,
    OriginalSourceAllocatorBackend backend) : failure_(state), backend_(backend) {}
OriginalSourceAllocator::~OriginalSourceAllocator() {
    // Destruction with live codec buffers is an owner-lifetime programming error.
    if (stats_.live_allocations || !Unbind()) std::abort();
}
bool OriginalSourceAllocator::Bind() {
    if (!backend_.allocate || !backend_.release || !backend_.charged ||
        (owner && owner != this)) return false;
    owner = this;
    return true;
}
bool OriginalSourceAllocator::Unbind() {
    if (stats_.live_allocations) return false;
    if (owner == this) owner = nullptr;
    return true;
}
void OriginalSourceAllocator::ResetPeak() {
    stats_.peak_requested = stats_.live_requested;
    stats_.peak_charged = stats_.live_charged;
}
void OriginalSourceAllocator::NotifyFailure() {
    if (stats_.failures < std::numeric_limits<size_t>::max()) ++stats_.failures;
    failure_.NotifyFailure();
}
void* OriginalSourceAllocator::Allocate(size_t bytes, size_t alignment) {
    const size_t max = std::numeric_limits<size_t>::max();
    if (owner != this || !PowerOfTwo(alignment)) {
        NotifyFailure(); return nullptr;
    }
    alignment = std::max(alignment, size_t{64});
    // Header ends immediately before an aligned payload; no global slot table.
    if (sizeof(Header) > max - (alignment - 1)) {
        NotifyFailure(); return nullptr;
    }
    const size_t offset = (sizeof(Header) + alignment - 1) & ~(alignment - 1);
    const size_t payload = std::max(bytes, size_t{1});
    if (payload > max - offset || bytes > max - stats_.live_requested ||
        stats_.live_allocations == max) {
        NotifyFailure(); return nullptr;
    }
    const size_t total = offset + payload;
    void* raw = backend_.allocate(backend_.context, alignment, total);
    if (!raw) { NotifyFailure(); return nullptr; }
    const size_t charged = backend_.charged(backend_.context, raw, total);
    if (reinterpret_cast<uintptr_t>(raw) % alignment || charged < total ||
        charged > max - stats_.live_charged) {
        backend_.release(backend_.context, raw);
        NotifyFailure(); return nullptr;
    }
    auto* result = static_cast<uint8_t*>(raw) + offset;
    new (result - sizeof(Header)) Header{this, raw, bytes, charged, alignment};
    stats_.live_requested += bytes;
    stats_.live_charged += charged;
    ++stats_.live_allocations;
    stats_.peak_requested = std::max(stats_.peak_requested, stats_.live_requested);
    stats_.peak_charged = std::max(stats_.peak_charged, stats_.live_charged);
    return result;
}
void OriginalSourceAllocator::Free(void* ptr) {
    if (!ptr) return;
    auto* header = reinterpret_cast<Header*>(static_cast<uint8_t*>(ptr) - sizeof(Header));
    if (header->allocator != this || owner != this) std::abort();
    void* raw = header->raw;
    stats_.live_requested -= header->requested;
    stats_.live_charged -= header->charged;
    --stats_.live_allocations;
    header->~Header();
    backend_.release(backend_.context, raw);
}
void* OriginalSourceAllocator::Reallocate(void* ptr, size_t bytes) {
    if (!ptr) return Allocate(bytes);
    if (!bytes) { Free(ptr); return nullptr; }
    auto* header = reinterpret_cast<Header*>(static_cast<uint8_t*>(ptr) - sizeof(Header));
    if (header->allocator != this || owner != this) std::abort();
    void* result = Allocate(bytes, header->alignment);
    if (!result) return nullptr; // Original pointer, bytes and accounting survive.
    std::memcpy(result, ptr, std::min(bytes, header->requested));
    Free(ptr);
    return result;
}
OriginalSourceAllocatorBackend OriginalSourceAllocator::EspBackend() {
#ifdef ESP_PLATFORM
    return {nullptr,
        [](void*, size_t alignment, size_t bytes) -> void* {
            return heap_caps_aligned_alloc(alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        },
        [](void*, void* ptr) { heap_caps_free(ptr); },
        [](void*, void* ptr, size_t) { return heap_caps_get_allocated_size(ptr); }};
#else
    return {}; // A native caller must supply an explicit test backend.
#endif
}
OriginalSourceRetainingBackend::OriginalSourceRetainingBackend(OriginalSourceAllocatorBackend inner,
    size_t limit, size_t min_block, size_t stale_after)
    : inner_(inner), limit_(limit), min_block_(min_block), stale_after_(stale_after) {}
OriginalSourceRetainingBackend::~OriginalSourceRetainingBackend() {
    ReleaseRetained();
    // A block still owned by the allocator is an owner-lifetime programming error.
    for (const auto& slot : slots_) if (slot.raw) std::abort();
}
OriginalSourceAllocatorBackend OriginalSourceRetainingBackend::backend() {
    if (!inner_.allocate || !inner_.release || !inner_.charged) return {};
    return {this,
        [](void* self, size_t alignment, size_t bytes) {
            return static_cast<OriginalSourceRetainingBackend*>(self)->Allocate(alignment, bytes);
        },
        [](void* self, void* raw) { static_cast<OriginalSourceRetainingBackend*>(self)->Release(raw); },
        [](void* self, void* raw, size_t requested) {
            return static_cast<OriginalSourceRetainingBackend*>(self)->Charged(raw, requested);
        }};
}
OriginalSourceRetainingBackend::Slot* OriginalSourceRetainingBackend::Find(const void* raw) {
    for (auto& slot : slots_) if (slot.raw == raw && !slot.retained) return &slot;
    return nullptr;
}
void OriginalSourceRetainingBackend::Drop(Slot& slot) {
    if (slot.retained) {
        stats_.retained_charged -= slot.charged;
        --stats_.retained_blocks;
        ASAN_UNPOISON_MEMORY_REGION(slot.raw, slot.total);
    }
    inner_.release(inner_.context, slot.raw);
    slot = {};
}
void OriginalSourceRetainingBackend::ReleaseRetained() {
    for (auto& slot : slots_) if (slot.retained) Drop(slot);
}
void* OriginalSourceRetainingBackend::Allocate(size_t alignment, size_t bytes) {
    if (!limit_ || bytes < min_block_) return inner_.allocate(inner_.context, alignment, bytes);
    ++clock_;
    Slot* best = nullptr;
    for (auto& slot : slots_) {
        if (slot.retained && slot.total >= bytes && slot.total - bytes <= bytes / 8 &&
            reinterpret_cast<uintptr_t>(slot.raw) % alignment == 0 && (!best || slot.total < best->total))
            best = &slot;
    }
    if (best) {
        best->retained = false;
        stats_.retained_charged -= best->charged;
        --stats_.retained_blocks;
        ++stats_.hits;
        ASAN_UNPOISON_MEMORY_REGION(best->raw, best->total);
        return best->raw;
    }
    ++stats_.misses;
    for (auto& slot : slots_) if (slot.retained && clock_ - slot.stamp > stale_after_) Drop(slot);
    void* raw = inner_.allocate(inner_.context, alignment, bytes);
    if (!raw && stats_.retained_blocks) {
        // Retained blocks may split the free span this request needs.
        ++stats_.refusal_flushes;
        ReleaseRetained();
        raw = inner_.allocate(inner_.context, alignment, bytes);
    }
    if (!raw) return nullptr;
    Slot* free_slot = nullptr;
    for (auto& slot : slots_) {
        if (!slot.raw) { free_slot = &slot; break; }
        if (slot.retained && (!free_slot || slot.stamp < free_slot->stamp)) free_slot = &slot;
    }
    // Every slot holds a live block: the block stays untracked and is never retained.
    if (!free_slot) return raw;
    if (free_slot->raw) Drop(*free_slot);
    *free_slot = {raw, bytes, inner_.charged(inner_.context, raw, bytes), 0, false};
    return raw;
}
void OriginalSourceRetainingBackend::Release(void* raw) {
    Slot* slot = Find(raw);
    if (!slot) { inner_.release(inner_.context, raw); return; }
    if (slot->charged > limit_) { Drop(*slot); return; }
    while (stats_.retained_charged > limit_ - slot->charged) {
        Slot* oldest = nullptr;
        for (auto& other : slots_)
            if (other.retained && (!oldest || other.stamp < oldest->stamp)) oldest = &other;
        Drop(*oldest);
    }
    slot->retained = true;
    slot->stamp = clock_;
    stats_.retained_charged += slot->charged;
    ++stats_.retained_blocks;
    ASAN_POISON_MEMORY_REGION(slot->raw, slot->total);
}
size_t OriginalSourceRetainingBackend::Charged(void* raw, size_t requested) {
    const Slot* slot = Find(raw);
    return slot ? slot->charged : inner_.charged(inner_.context, raw, requested);
}
OriginalSourceRegionBackend::OriginalSourceRegionBackend(OriginalSourceAllocatorBackend inner,
    OriginalSourceRegionHeapOps ops, size_t region_bytes)
    : inner_(inner), ops_(ops), region_bytes_(region_bytes) {}
OriginalSourceRegionBackend::~OriginalSourceRegionBackend() {
    // A block still owned by the allocator is an owner-lifetime programming error.
    if (!ReleaseRegion()) std::abort();
}
OriginalSourceRegionHeapOps OriginalSourceRegionBackend::EspHeapOps() {
#ifdef ESP_PLATFORM
    return {[](void* base, size_t bytes) -> void* { return multi_heap_register(base, bytes); },
        [](void* heap, size_t alignment, size_t bytes) {
            return multi_heap_aligned_alloc(static_cast<multi_heap_handle_t>(heap), bytes, alignment);
        },
        [](void* heap, void* ptr) { multi_heap_free(static_cast<multi_heap_handle_t>(heap), ptr); },
        [](void* heap, void* ptr) {
            return multi_heap_get_allocated_size(static_cast<multi_heap_handle_t>(heap), ptr);
        },
        [](void* heap) { return multi_heap_minimum_free_size(static_cast<multi_heap_handle_t>(heap)); }};
#else
    return {}; // A native caller must supply explicit heap operations.
#endif
}
OriginalSourceAllocatorBackend OriginalSourceRegionBackend::backend() {
    if (!inner_.allocate || !inner_.release || !inner_.charged) return {};
    return {this,
        [](void* self, size_t alignment, size_t bytes) {
            return static_cast<OriginalSourceRegionBackend*>(self)->Allocate(alignment, bytes);
        },
        [](void* self, void* raw) { static_cast<OriginalSourceRegionBackend*>(self)->Release(raw); },
        [](void* self, void* raw, size_t requested) {
            return static_cast<OriginalSourceRegionBackend*>(self)->Charged(raw, requested);
        }};
}
bool OriginalSourceRegionBackend::Contains(const void* raw) const {
    const auto* byte = static_cast<const uint8_t*>(raw);
    return base_ && byte >= base_ && byte < base_ + region_bytes_;
}
const OriginalSourceRegionStats& OriginalSourceRegionBackend::stats() const {
    stats_.region_min_free = heap_ ? ops_.min_free(heap_) : 0;
    return stats_;
}
bool OriginalSourceRegionBackend::ReleaseRegion() {
    attempted_ = false;
    if (!base_) return true;
    if (stats_.live_blocks) return false;
    inner_.release(inner_.context, base_);
    base_ = nullptr;
    heap_ = nullptr;
    stats_.reserved_bytes = 0;
    return true;
}
void* OriginalSourceRegionBackend::Allocate(size_t alignment, size_t bytes) {
    if (!base_ && !attempted_ && region_bytes_ && ops_.create && ops_.allocate && ops_.release &&
        ops_.charged && ops_.min_free) {
        // One attempt per session: a refused reservation is not retried on every request.
        attempted_ = true;
        base_ = static_cast<uint8_t*>(inner_.allocate(inner_.context, 64, region_bytes_));
        heap_ = base_ ? ops_.create(base_, region_bytes_) : nullptr;
        if (base_ && !heap_) {
            inner_.release(inner_.context, base_);
            base_ = nullptr;
        }
        if (base_) {
            ++stats_.reservations;
            stats_.reserved_bytes = region_bytes_;
        } else {
            ++stats_.reservation_failures;
        }
    }
    if (heap_) {
        if (void* raw = ops_.allocate(heap_, alignment, bytes)) {
            ++stats_.live_blocks;
            return raw;
        }
        ++stats_.overflows;
    }
    return inner_.allocate(inner_.context, alignment, bytes);
}
void OriginalSourceRegionBackend::Release(void* raw) {
    if (!Contains(raw)) { inner_.release(inner_.context, raw); return; }
    ops_.release(heap_, raw);
    --stats_.live_blocks;
}
size_t OriginalSourceRegionBackend::Charged(void* raw, size_t requested) {
    return Contains(raw) ? ops_.charged(heap_, raw) : inner_.charged(inner_.context, raw, requested);
}
}
extern "C" void* tbot_original_malloc(size_t bytes) {
    return owner ? owner->Allocate(bytes) : nullptr;
}
extern "C" void* tbot_original_memalign(size_t alignment, size_t bytes) {
    return owner ? owner->Allocate(bytes, alignment) : nullptr;
}
extern "C" int tbot_original_posix_memalign(void** output, size_t alignment, size_t bytes) {
    if (!output || alignment < sizeof(void*) || !PowerOfTwo(alignment)) {
        tbot_original_allocation_failure(); // av_malloc reports any refusal as OOM.
        return EINVAL;
    }
    void* result = tbot_original_memalign(alignment, bytes);
    if (!result) return ENOMEM;
    *output = result;
    return 0;
}
extern "C" void* tbot_original_realloc(void* ptr, size_t bytes) {
    return owner ? owner->Reallocate(ptr, bytes) : nullptr;
}
extern "C" void tbot_original_free(void* ptr) {
    if (!ptr) return;
    if (!owner) std::abort();
    owner->Free(ptr);
}
extern "C" void tbot_original_allocation_failure() {
    if (owner) owner->NotifyFailure();
}
extern "C" void* tbot_original_zcalloc(void*, unsigned items, unsigned size) {
    if (size && items > std::numeric_limits<size_t>::max() / size) {
        tbot_original_allocation_failure();
        return nullptr;
    }
    return tbot_original_malloc(size_t{items} * size);
}
extern "C" void tbot_original_zcfree(void*, void* ptr) {
    tbot_original_free(ptr);
}
