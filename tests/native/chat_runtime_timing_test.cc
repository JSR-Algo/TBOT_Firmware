#include "chat_inbound_messages.h"
#include "chat_runtime_timing.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <stdexcept>
#include <type_traits>

uint64_t clock_us = 0;
unsigned emitted = 0;
uint32_t last_site = 0, last_hi = 0, last_lo = 0;
bool display_locked = false;
uint64_t Clock() { return clock_us; }
void Emit(uint32_t site, uint32_t hi, uint32_t lo) {
    assert(!display_locked);
    ++emitted; last_site = site; last_hi = hi; last_lo = lo;
}
struct Lock {
    Lock() { display_locked = true; }
    ~Lock() { display_locked = false; }
};
void TimedReturn(bool throws) {
    ChatRuntimeTiming timing(3, Clock, Emit);
    Lock lock;
    clock_us += 50000;
    if (throws) throw std::runtime_error("unwind");
    return;
}
std::mutex copy_mutex;
std::condition_variable copy_cv;
bool copying = false, released = false;
void* BlockingAllocate(size_t size) {
    std::unique_lock<std::mutex> lock(copy_mutex);
    copying = true; copy_cv.notify_all(); copy_cv.wait(lock, [] { return released; });
    return std::malloc(size);
}
int main() {
    static_assert(!std::is_copy_constructible_v<ChatRuntimeTiming>);
    static_assert(!std::is_move_constructible_v<ChatRuntimeTiming>);
    { ChatRuntimeTiming timing(1, Clock, Emit); clock_us = 49999; }
    assert(emitted == 0);
    { ChatRuntimeTiming timing(2, Clock, Emit); clock_us += 50000; }
    assert(emitted == 1 && last_site == 2 && last_hi == 0 && last_lo == 50000);
    TimedReturn(false); assert(emitted == 2 && last_site == 3);
    try { TimedReturn(true); } catch (const std::runtime_error&) {}
    assert(emitted == 3);
    { ChatRuntimeTiming timing(4, Clock, Emit); --clock_us; }
    assert(emitted == 3);
    { ChatRuntimeTiming timing(5, Clock, Emit); clock_us += (uint64_t{2} << 32) + 50000; }
    assert(emitted == 4 && last_hi == 2 && last_lo == 50000);

    ChatInboundMessages inbox;
    auto snapshot = inbox.TrySnapshot();
    assert(snapshot.available && !snapshot.queued && !snapshot.outstanding);
    auto* root = cJSON_Parse("{\"type\":\"mcp\"}");
    for (unsigned i = 0; i < 4; ++i) assert(inbox.Admit(root, {{1, 2}, 1, 1}, 100, 0));
    snapshot = inbox.TrySnapshot();
    assert(snapshot.available && snapshot.queued == 4 && snapshot.outstanding == 4);
    auto retained = inbox.Take();
    snapshot = inbox.TrySnapshot();
    assert(snapshot.available && snapshot.queued == 3 && snapshot.outstanding == 4);
    retained.reset();
    snapshot = inbox.TrySnapshot();
    assert(snapshot.available && snapshot.queued == 3 && snapshot.outstanding == 3);
    cJSON_Hooks hooks{BlockingAllocate, std::free}; cJSON_InitHooks(&hooks);
    auto producer = std::async(std::launch::async, [&] { return inbox.Admit(root, {{1, 2}, 1, 1}, 100, 0); });
    {
        std::unique_lock<std::mutex> lock(copy_mutex);
        assert(copy_cv.wait_for(lock, std::chrono::seconds(2), [] { return copying; }));
    }
    auto reader = std::async(std::launch::async, [&] { return inbox.TrySnapshot(); });
    assert(reader.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    snapshot = reader.get();
    assert(!snapshot.available && !snapshot.queued && !snapshot.outstanding);
    { std::lock_guard<std::mutex> lock(copy_mutex); released = true; copy_cv.notify_all(); }
    assert(producer.get()); cJSON_InitHooks(nullptr); cJSON_Delete(root);
}
