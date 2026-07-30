#ifndef LESSON_MJPEG_MP4_H
#define LESSON_MJPEG_MP4_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "lesson_asset_storage_coordinator.h"

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
    kLeaseUnavailable,
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
    LessonMjpegMp4Status GetFrame(std::size_t index, LessonMjpegMp4Frame* frame) const;

    std::size_t frame_count() const { return frame_count_; }
    std::uint16_t width() const { return width_; }
    std::uint16_t height() const { return height_; }
    std::uint32_t timescale() const { return timescale_; }
    std::uint64_t duration_ticks() const { return duration_ticks_; }
    std::uint32_t frame_duration_ticks() const { return frame_duration_ticks_; }
    std::uint32_t fps_milli() const { return fps_milli_; }

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

struct LessonMjpegMp4FileOps {
    void* context;
    void* (*open)(void* context, const char* path);
    bool (*size)(void* context, void* file, std::uint64_t* size);
    bool (*seek)(void* context, void* file, std::uint64_t offset);
    std::size_t (*read)(void* context, void* file, std::uint8_t* out, std::size_t size);
    void (*close)(void* context, void* file);
};

class LessonMjpegMp4File {
public:
    LessonMjpegMp4File() = default;
    ~LessonMjpegMp4File();

    LessonMjpegMp4Status OpenUnderLessonSession(
        const char* path,
        const std::string& assignment_id,
        const std::string& session_id,
        std::uint64_t generation,
        LessonMjpegMp4FileOps ops = {}
    );
    LessonMjpegMp4Status ReadFrame(
        std::size_t index,
        std::uint8_t* destination,
        std::size_t capacity,
        std::size_t* bytes_read
    );
    LessonMjpegMp4Status GetFrame(std::size_t index, LessonMjpegMp4Frame* frame) const;
    void Close();
    bool is_open() const;

    LessonMjpegMp4File(const LessonMjpegMp4File&) = delete;
    LessonMjpegMp4File& operator=(const LessonMjpegMp4File&) = delete;
    LessonMjpegMp4File(LessonMjpegMp4File&&) = delete;
    LessonMjpegMp4File& operator=(LessonMjpegMp4File&&) = delete;

private:
    static bool ReadAt(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size);
    void CloseLocked();

    mutable std::mutex io_mutex_;
    void* file_ = nullptr;
    LessonMjpegMp4FileOps ops_{};
    LessonAssetReadLease lesson_read_lease_;
    LessonMjpegMp4Reader reader_;
};

}  // namespace tbot

#endif  // LESSON_MJPEG_MP4_H
