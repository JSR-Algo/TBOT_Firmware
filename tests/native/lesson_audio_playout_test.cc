#include "lesson_audio_playout.h"
#include <cassert>
#include <cstdio>

int main() {
    LessonAudioPlayout p;
    const char* a = "0123456789abcdef0123456789abcdef";
    const char* b = "1123456789abcdef0123456789abcdef";
    PlaybackDrainSnapshot d{7, 1, false, 0, 0, false, false,
                            {1, AudioOutputDrainState::Drained}};
    assert(!p.Begin(1, "invalid", 7));
    assert(p.Begin(1, a, 7));
    assert(!p.Begin(2, a, 7)); // Duplicate START cannot reset live audio.
    assert(!p.TakeStart()); // Text, START and queued audio are not output.
    p.PublishOutput(9, true, 100);
    p.PublishOutput(1, false, 100);
    assert(!p.TakeStart());
    p.PublishOutput(1, true, 101);
    auto start = p.TakeStart();
    assert(start && start->at_ms == 101 && start->generation == 1);
    p.PublishOutput(1, true, 200);
    assert(!p.TakeStart());
    assert(!p.Drained(d)); // Chunk gap before STOP is not completion.
    assert(!p.Stop(b));
    assert(p.Stop(a));
    d.decode_in_flight = true;
    assert(!p.Drained(d));
    d.decode_in_flight = false;
    d.codec.state = AudioOutputDrainState::Pending;
    assert(!p.Drained(d));
    d.codec.state = AudioOutputDrainState::Failed;
    assert(!p.Drained(d));
    d.codec.state = AudioOutputDrainState::Drained;
    d.stopped = true;
    assert(!p.Drained(d));
    d.stopped = false;
    d.reset_epoch = 8;
    assert(!p.Drained(d));
    d.reset_epoch = 7;
    assert(p.Drained(d));
    assert(p.Begin(2, b, 8));
    assert(!p.Stop(a)); // Delayed old STOP cannot retire replacement.
    assert(!p.Begin(3, a, 8)); // Previously retired START is obsolete.
    p.PublishOutput(1, true, 300);
    assert(!p.TakeStart());
    p.PublishOutput(2, true, 301);
    assert(p.TakeStart());
    p.Cancel(); // Barge-in/disconnect/error invalidates queued callback.
    assert(!p.Stop(b));
    p.PublishOutput(2, true, 302);
    assert(!p.TakeStart());
    assert(!p.Begin(3, b, 9));
    std::puts("lesson audio playout identity/drain: PASS");
}
