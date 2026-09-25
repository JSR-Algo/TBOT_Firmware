#include "audio/audio_playback_refill_policy.h"
#include <cassert>

int main() {
    AudioPlaybackRefillPolicy policy;
    // One extra decode can run before encoding, never an unbounded burst.
    for (int i = 0; i < 100; ++i) {
        assert(policy.DeferEncode(true, true, true, 0, 2));
        assert(!policy.DeferEncode(true, true, true, 0, 2));
    }
    assert(!policy.DeferEncode(false, true, true, 0, 2));
    assert(!policy.DeferEncode(true, false, true, 0, 2));
    assert(!policy.DeferEncode(true, true, false, 0, 2));
    assert(!policy.DeferEncode(true, true, true, 2, 2));
    assert(!policy.DeferEncode(true, true, true, 0, 0));
    assert(policy.DeferEncode(true, true, true, 1, 2));
    // A failed decode reaches the mic encode block and resets the budget.
    assert(!policy.DeferEncode(false, true, true, 0, 2));
    assert(policy.DeferEncode(true, true, true, 0, 2));
    assert(!policy.DeferEncode(true, true, true, 0, 2));
    assert(policy.DeferEncode(true, true, true, 0, 2));
    // Stale packets skip the policy and encoding; keep the outstanding budget.
    assert(!policy.DeferEncode(true, true, true, 0, 2));
}
