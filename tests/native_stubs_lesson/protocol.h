#pragma once
// Host stub for main/protocols/protocol.h (lesson subset only). Records every frame the
// renderer emits via protocol_->SendLessonFrame so tests can assert the exact outbound
// JSON envelopes (type/sequence/body) — the real contract surface.
#include <cJSON.h>

#include <string>
#include <vector>

class Protocol {
public:
    virtual ~Protocol() = default;
    std::vector<std::string> sent_frames;

    bool SendLessonFrame(const std::string& frame) {
        sent_frames.push_back(frame);
        return true;
    }
};
