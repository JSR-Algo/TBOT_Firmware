#ifndef LESSON_MJPEG_MP4_H
#define LESSON_MJPEG_MP4_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace tbot {

class LessonMjpegMp4Parser;

inline constexpr std::size_t kLessonMjpegMp4MaxSamples = 900;
inline constexpr std::uint64_t kLessonMjpegMp4MaxFileBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kLessonMjpegMp4MaxSampleBytes = 1024U * 1024U;
inline constexpr std::uint16_t kLessonMjpegMp4MaxWidth = 1920;
inline constexpr std::uint16_t kLessonMjpegMp4MaxHeight = 1080;

enum class LessonMjpegMp4Status : std::uint8_t {
    kOk,
    kInvalidArgument,
    kIoError,
    kTruncated,
    kMalformed,
    kUnsupported,
    kLimitExceeded,
    kMetadataMismatch,
};

struct LessonMjpegMp4Io {
    void* context;
    bool (*read_at)(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size);
    std::uint64_t file_size;
};

struct LessonMjpegMp4Frame {
    std::uint64_t offset;
    std::uint32_t size;
};

class LessonMjpegMp4Reader {
public:
    LessonMjpegMp4Status Open(const LessonMjpegMp4Io& io);
    LessonMjpegMp4Status ReadFrame(
        std::size_t index,
        std::uint8_t* destination,
        std::size_t capacity,
        std::size_t* bytes_read
    ) const;

    std::size_t frame_count() const { return frame_count_; }
    std::uint16_t width() const { return width_; }
    std::uint16_t height() const { return height_; }
    std::uint32_t timescale() const { return timescale_; }
    std::uint64_t duration_ticks() const { return duration_ticks_; }
    std::uint32_t frame_duration_ticks() const { return frame_duration_ticks_; }
    std::uint32_t fps_milli() const { return fps_milli_; }
    const LessonMjpegMp4Frame& frame(std::size_t index) const { return frames_[index]; }

private:
    friend class LessonMjpegMp4Parser;
    LessonMjpegMp4Io io_{};
    std::array<LessonMjpegMp4Frame, kLessonMjpegMp4MaxSamples> frames_{};
    std::size_t frame_count_ = 0;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    std::uint32_t timescale_ = 0;
    std::uint64_t duration_ticks_ = 0;
    std::uint32_t frame_duration_ticks_ = 0;
    std::uint32_t fps_milli_ = 0;
};

}  // namespace tbot

#endif  // LESSON_MJPEG_MP4_H
