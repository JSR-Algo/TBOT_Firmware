#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
std::function<void(int)> hook;
void EpochPublicationHook(int point) { if (hook) hook(point); }
// INSTRUMENTED_PUBLICATIONS

int main() {
    AudioResetEpochPublication publication;
    publication.BeginReset();
    publication.Publish(UINT64_C(0xffffffff));
    unsigned intermediate_reads = 0;
    hook = [&](int point) {
        if (point > 1) return;
        uint64_t out = 77;
        assert(!publication.TryRead(out) && out == 77);
        ++intermediate_reads;
    };
    publication.BeginReset();
    publication.Publish(UINT64_C(0x100000000));
    assert(intermediate_reads == 2);
    hook = {};
    uint64_t out = 88;
    assert(publication.TryRead(out) && out == UINT64_C(0x100000000));

    // Reset between the reader's two halves: even a completed publication
    // must cause the original capture to reject its mixed observations.
    hook = [&](int point) {
        if (point == 2) {
            publication.BeginReset();
            publication.Publish(UINT64_C(0x200000001));
        }
    };
    out = 99;
    assert(!publication.TryRead(out) && out == 99);
    hook = {};
    assert(publication.TryRead(out) && out == UINT64_C(0x200000001));

    SaturatingPublication saturated;
    saturated.BeginReset();
    saturated.Publish(1);
    assert(saturated.TryRead(out) && out == 1);
    for (unsigned i = 0; i < 3; ++i) {
        saturated.BeginReset();
        saturated.Publish(2 + i);
        assert(!saturated.TryRead(out) && out == 1);
    }
}
