#ifndef LESSON_TRGB_SIZE_POLICY_H
#define LESSON_TRGB_SIZE_POLICY_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace tbot {

inline constexpr std::size_t kLessonTrgbMaxFileBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kLessonTrgbMaxPackBytes = 768U * 1024U * 1024U;
inline constexpr std::size_t kLessonTrgbFrameBytes = 320U * 480U * 2U;

inline bool LessonTrgbExpectedContainerBytes(std::uint64_t frame_count,
                                             std::uint64_t frame_bytes,
                                             std::uint64_t* expected) {
    constexpr std::uint64_t kHeaderBytes = 64;
    constexpr std::uint64_t kIndexRecordBytes = 16;
    constexpr std::uint64_t kAlignment = 512;
    if (expected == nullptr || frame_count == 0 || frame_bytes != kLessonTrgbFrameBytes ||
        frame_count > (std::numeric_limits<std::uint64_t>::max() - kHeaderBytes) /
            kIndexRecordBytes) {
        return false;
    }
    const std::uint64_t index_end = kHeaderBytes + frame_count * kIndexRecordBytes;
    if (index_end > std::numeric_limits<std::uint64_t>::max() - (kAlignment - 1) ||
        frame_count > std::numeric_limits<std::uint64_t>::max() / frame_bytes) {
        return false;
    }
    const std::uint64_t data_offset = (index_end + kAlignment - 1) & ~(kAlignment - 1);
    const std::uint64_t payload = frame_count * frame_bytes;
    if (payload > std::numeric_limits<std::uint64_t>::max() - data_offset) return false;
    *expected = data_offset + payload;
    return *expected <= kLessonTrgbMaxFileBytes;
}

inline bool LessonTrgbPlausibleContainerBytes(std::uint64_t bytes) {
    if (bytes == 0 || bytes > kLessonTrgbMaxFileBytes || bytes % 512 != 0) return false;
    const std::uint64_t max_frames = kLessonTrgbMaxFileBytes / kLessonTrgbFrameBytes;
    for (std::uint64_t frames = 1; frames <= max_frames; ++frames) {
        std::uint64_t expected = 0;
        if (LessonTrgbExpectedContainerBytes(frames, kLessonTrgbFrameBytes, &expected) &&
            expected == bytes) {
            return true;
        }
    }
    return false;
}

}  // namespace tbot

#endif  // LESSON_TRGB_SIZE_POLICY_H
