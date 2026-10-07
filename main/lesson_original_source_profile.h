#ifndef LESSON_ORIGINAL_SOURCE_PROFILE_H
#define LESSON_ORIGINAL_SOURCE_PROFILE_H

#include <chrono>
#include <cstdint>

namespace tbot {

// Cumulative microseconds per renderer-v6 sub-phase, for on-device profiling
// (the lab self-test logs per-cue deltas). Written by the decode/paint path only.
struct OriginalSourceProfile {
    std::uint64_t read_us = 0, hash_us = 0, demux_open_us = 0, codec_open_us = 0;
    std::uint64_t draw_media_us = 0, fill_us = 0, text_us = 0;
    std::uint64_t first_read_us = 0, max_read_us = 0, slow_reads = 0;  // per fread; slow = over 100 ms
};

inline OriginalSourceProfile& OriginalSourceProfileCounters() {
    static OriginalSourceProfile profile;
    return profile;
}

class OriginalSourceProfileScope {
public:
    explicit OriginalSourceProfileScope(std::uint64_t* slot) : slot_(slot), start_(std::chrono::steady_clock::now()) {}
    ~OriginalSourceProfileScope() {
        *slot_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - start_)
                                                 .count());
    }
    OriginalSourceProfileScope(const OriginalSourceProfileScope&) = delete;
    OriginalSourceProfileScope& operator=(const OriginalSourceProfileScope&) = delete;

private:
    std::uint64_t* slot_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace tbot

#endif
