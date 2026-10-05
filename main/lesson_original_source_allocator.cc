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
