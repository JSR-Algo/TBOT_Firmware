#pragma once
#include <cstdint>

// A stack-only scope: emit at most once, including returns and exception unwind.
class ChatRuntimeTiming {
public:
    using Clock = uint64_t (*)();
    using Sink = void (*)(uint32_t, uint32_t, uint32_t);
    ChatRuntimeTiming(uint32_t site, Clock clock, Sink sink)
        : site_(site), clock_(clock), sink_(sink), started_us_(clock()) {}
    ChatRuntimeTiming(const ChatRuntimeTiming&) = delete;
    ChatRuntimeTiming& operator=(const ChatRuntimeTiming&) = delete;
    ~ChatRuntimeTiming() {
        const auto now_us = clock_();
        const auto elapsed_us = now_us >= started_us_ ? now_us - started_us_ : 0;
        if (elapsed_us >= 50000)
            sink_(site_, static_cast<uint32_t>(elapsed_us >> 32), static_cast<uint32_t>(elapsed_us));
    }
private:
    const uint32_t site_;
    const Clock clock_;
    const Sink sink_;
    const uint64_t started_us_;
};
