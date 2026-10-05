// Deterministic fault-injecting native backend under the production allocator.
// The session proof routes every pinned FFmpeg/zlib allocation through the real
// tbot_original_* owner; only the final heap call is replaced here.
#include "lesson_original_source_allocator.h"
#include <cstdlib>
namespace {
size_t calls, fail_at, injections;
void (*allocation_event)(size_t);
tbot::OriginalSourceAllocator* allocator;
void* Allocate(void*, size_t alignment, size_t bytes) {
    ++calls;
    if (allocation_event) allocation_event(calls);
    if (calls == fail_at) { ++injections; return nullptr; }
    void* p = nullptr;
    return posix_memalign(&p, alignment, bytes) ? nullptr : p;
}
void Release(void*, void* p) { free(p); }
size_t Charged(void*, void*, size_t bytes) { return bytes; }
}
extern "C" {
void original_source_bind(tbot::OriginalSourceAllocationState* state) {
    if (allocator) abort();
    allocator = new tbot::OriginalSourceAllocator(*state, {nullptr, Allocate, Release, Charged});
    if (!allocator->Bind()) abort();
}
void original_source_on_allocation(void (*event)(size_t)) { allocation_event = event; }
void original_source_fault_reset(size_t nth) {
    if (allocator->stats().live_allocations) abort();
    calls = 0; fail_at = nth; injections = 0;
    allocator->ResetPeak();
}
// Fault selection for a forked exact-state checkpoint; retains live ownership.
void original_source_fail_at(size_t nth) { fail_at = nth; injections = 0; }
size_t original_source_live(void) { return allocator->stats().live_requested; }
size_t original_source_peak(void) { return allocator->stats().peak_requested; }
size_t original_source_calls(void) { return calls; }
size_t original_source_injections(void) { return injections; }
}
