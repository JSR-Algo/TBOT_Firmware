#include "speaking_arm_dispatch.h"
#include <cassert>
#include <vector>

int main() {
    SpeakingArmDispatch dispatch;
    std::vector<SpeakingArmGesture::Target> sent;
    auto transport = [&](auto target, auto owns) {
        if (!owns()) return false;
        sent.push_back(target);
        return true;
    };
    dispatch.BeginResponse(10);
    dispatch.Poll(1000, true, transport);
    assert(sent.empty()); // Thinking / intake alone is not output evidence.
    dispatch.PublishOutput(10, false, 1000);
    dispatch.Poll(1000, true, transport);
    assert(sent.empty()); // Wake sounds and PCM effects never qualify.
    dispatch.PublishOutput(9, true, 1000);
    dispatch.Poll(1000, true, transport);
    assert(sent.empty()); // Old decoded output retains its original identity.
    dispatch.PublishOutput(10, true, 1000);
    dispatch.Poll(1000, true, transport);
    assert(sent.size() == 1 && sent.back().left && sent.back().percent == 20);
    dispatch.Cancel(); // Disconnect, tts stop and abort share this boundary.
    dispatch.BeginResponse(10);
    dispatch.PublishOutput(10, true, 2100);
    dispatch.Poll(2100, true, transport);
    assert(sent.size() == 1); // Manual ownership lasts the current response.
    dispatch.BeginResponse(11);
    dispatch.PublishOutput(11, true, 2200);
    dispatch.Poll(2200, false, transport); // Lesson/state exclusion cancels.
    dispatch.Poll(2200, true, transport);
    assert(sent.size() == 1);
    dispatch.BeginResponse(12);
    dispatch.PublishOutput(12, true, 2300);
    dispatch.Poll(2500, true, transport);
    assert(sent.size() == 1); // Queued observation expires.
    dispatch.PublishOutput(12, true, 2600);
    dispatch.Poll(2600, true, [&](auto target, auto owns) {
        dispatch.Cancel(); // Explicit command before transport serialization.
        if (owns()) sent.push_back(target);
        return false;
    });
    assert(sent.size() == 1);
    dispatch.BeginResponse(13);
    dispatch.PublishOutput(13, true, 4000);
    dispatch.Poll(4000, true, [](auto, auto) { return false; }); // UART busy: skip.
    dispatch.Poll(4200, true, transport);
    assert(sent.size() == 1); // No queued command drains later.
    dispatch.PublishOutput(13, true, 5000);
    dispatch.Poll(5000, true, transport);
    assert(sent.size() == 2 && sent.back().percent == 0);
}
