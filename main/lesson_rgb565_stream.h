#ifndef LESSON_RGB565_STREAM_H
#define LESSON_RGB565_STREAM_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "lesson_asset_storage_coordinator.h"

namespace tbot {

inline constexpr std::uint32_t kLessonRgb565FrameBytes = 320U * 480U * 2U;

enum class LessonRgb565Status : std::uint8_t {
    kOk,
    kInvalidArgument,
    kUnsafePath,
    kIoError,
    kTruncated,
    kMalformed,
    kUnsupported,
    kOverflow,
    kSmallBuffer,
    kFrameOutOfRange,
    kHeaderCrcMismatch,
    kFrameCrcMismatch,
    kLeaseUnavailable,
};

struct LessonRgb565Io {
    void* context;
    bool (*read_at)(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size);
    std::uint64_t file_size;
};

struct LessonRgb565Metadata {
    std::uint16_t stored_width;
    std::uint16_t stored_height;
    std::uint16_t display_width;
    std::uint16_t display_height;
    std::uint16_t fps;
    std::uint32_t frame_count;
    std::uint32_t duration_ms;
    std::uint32_t frame_bytes;
};

class LessonRgb565Reader {
public:
    LessonRgb565Status Open(const LessonRgb565Io& io);
    LessonRgb565Status ReadFrame(
        std::uint32_t index,
        std::uint8_t* destination,
        std::size_t capacity
    ) const;

    LessonRgb565Metadata metadata() const { return metadata_; }

private:
    LessonRgb565Status ReadIndexRecord(
        std::uint32_t index,
        std::uint64_t* offset,
        std::uint32_t* length,
        std::uint32_t* crc
    ) const;

    LessonRgb565Io io_{};
    LessonRgb565Metadata metadata_{};
    std::uint32_t index_offset_ = 0;
    std::uint32_t data_offset_ = 0;
    std::uint64_t file_bytes_ = 0;
};

struct LessonRgb565FileOps {
    void* context;
    void* (*open)(void* context, const char* path);
    bool (*size)(void* context, void* file, std::uint64_t* size);
    bool (*seek)(void* context, void* file, std::uint64_t offset);
    std::size_t (*read)(void* context, void* file, std::uint8_t* out, std::size_t size);
    void (*close)(void* context, void* file);
};

class LessonRgb565File {
public:
    LessonRgb565File() = default;
    ~LessonRgb565File();

    LessonRgb565Status OpenUnderLessonSession(
        const char* path,
        const std::string& assignment_id,
        const std::string& session_id,
        std::uint64_t generation,
        LessonRgb565FileOps ops = {}
    );
    LessonRgb565Status ReadFrame(
        std::uint32_t index,
        std::uint8_t* destination,
        std::size_t capacity
    );
    void Close();
    bool is_open() const;
    LessonRgb565Metadata metadata() const;

    LessonRgb565File(const LessonRgb565File&) = delete;
    LessonRgb565File& operator=(const LessonRgb565File&) = delete;
    LessonRgb565File(LessonRgb565File&&) = delete;
    LessonRgb565File& operator=(LessonRgb565File&&) = delete;

private:
    static bool ReadAt(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size);
    void CloseLocked();

    mutable std::mutex io_mutex_;
    void* file_ = nullptr;
    LessonRgb565FileOps ops_{};
    LessonAssetReadLease lesson_read_lease_;
    std::string assignment_id_;
    std::string session_id_;
    std::uint64_t generation_ = 0;
    LessonRgb565Reader reader_;
};

}  // namespace tbot

#endif  // LESSON_RGB565_STREAM_H
