#include "async_lookup_lifecycle.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace std::chrono_literals;

int main() {
    AsyncLookupLifecycle lookup;
    const uint32_t first_generation = lookup.TryAcquire();
    assert(first_generation != 0);

    AsyncLookupResult timed_out;
    std::thread waiter([&]() {
        timed_out = lookup.WaitFor(first_generation, 10ms);
    });
    waiter.join();
    assert(timed_out.status == AsyncLookupStatus::kTimedOut);
    assert(lookup.IsIdle());

    // A later lookup reuses the slot immediately. The old delayed callback is
    // generation-tagged and must not complete or corrupt the new lookup.
    const uint32_t second_generation = lookup.TryAcquire();
    assert(second_generation != 0);
    assert(second_generation != first_generation);
    lookup.Complete(first_generation, true, 0x01020304u);
    assert(!lookup.IsIdle());

    std::thread completer([&]() {
        std::this_thread::sleep_for(2ms);
        lookup.Complete(second_generation, true, 0x05060708u);
    });
    const AsyncLookupResult completed = lookup.WaitFor(second_generation, 100ms);
    completer.join();
    assert(completed.status == AsyncLookupStatus::kResolved);
    assert(completed.value == 0x05060708u);
    assert(lookup.IsIdle());

    // Model the production pool: eight unrelated lookups can coexist and a
    // ninth fails without disturbing any active slot.
    std::array<AsyncLookupLifecycle, 8> pool;
    std::array<uint32_t, 8> generations{};
    for (size_t index = 0; index < pool.size(); ++index) {
        generations[index] = pool[index].TryAcquire();
        assert(generations[index] != 0);
    }
    for (auto& slot : pool) {
        assert(slot.TryAcquire() == 0);
    }
    for (size_t index = 0; index < pool.size(); ++index) {
        pool[index].Complete(generations[index], true, static_cast<uint32_t>(index));
        const AsyncLookupResult result = pool[index].WaitFor(generations[index], 1ms);
        assert(result.status == AsyncLookupStatus::kResolved);
        assert(result.value == index);
        assert(pool[index].IsIdle());
    }
    return 0;
}
