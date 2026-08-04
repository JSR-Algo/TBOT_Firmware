#include "lesson_rgb565_stream.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace tbot {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kIndexRecordBytes = 16;
constexpr std::uint32_t kSectorBytes = 512;
constexpr char kMagic[] = "TBOTRGB1";
constexpr char kLessonAssetRoot[] = "/sdcard/tbot/lesson-assets/";
constexpr std::size_t kMaxPathBytes = 1024;

std::uint16_t Le16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t Le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t Le64(const std::uint8_t* bytes) {
    return static_cast<std::uint64_t>(Le32(bytes)) |
           (static_cast<std::uint64_t>(Le32(bytes + 4)) << 32);
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (result == nullptr || right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (result == nullptr || (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

const std::array<std::uint32_t, 256>& CrcTable() {
    static const std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            std::uint32_t crc = index;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xedb88320U : 0U);
            }
            values[index] = crc;
        }
        return values;
    }();
    return table;
}

std::uint32_t Crc32(const std::uint8_t* bytes, std::size_t size) {
    const auto& table = CrcTable();
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc = table[(crc ^ bytes[index]) & 0xffU] ^ (crc >> 8);
    }
    return ~crc;
}

bool IsSafeLessonAssetPath(const char* path) {
    if (path == nullptr) return false;
    const std::size_t root_size = sizeof(kLessonAssetRoot) - 1;
    const std::size_t size = strnlen(path, kMaxPathBytes + 1);
    if (size <= root_size || size > kMaxPathBytes ||
        std::memcmp(path, kLessonAssetRoot, root_size) != 0) {
        return false;
    }

    std::size_t component_start = root_size;
    for (std::size_t index = root_size; index <= size; ++index) {
        const unsigned char byte = static_cast<unsigned char>(path[index]);
        if (index < size && (byte < 0x21 || byte > 0x7e || byte == '\\')) {
            return false;
        }
        if (index == size || byte == '/') {
            const std::size_t component_size = index - component_start;
            if (component_size == 0 ||
                (component_size == 1 && path[component_start] == '.') ||
                (component_size == 2 && path[component_start] == '.' &&
                 path[component_start + 1] == '.')) {
                return false;
            }
            component_start = index + 1;
        }
    }
    return true;
}

void* OpenStdFile(void*, const char* path) {
    return std::fopen(path, "rb");
}

bool SizeStdFile(void*, void* raw_file, std::uint64_t* size) {
    FILE* file = static_cast<FILE*>(raw_file);
    if (fseeko(file, 0, SEEK_END) != 0) return false;
    const off_t end = ftello(file);
    if (end < 0) return false;
    *size = static_cast<std::uint64_t>(end);
    return true;
}

bool SeekStdFile(void*, void* raw_file, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return false;
    return fseeko(static_cast<FILE*>(raw_file), static_cast<off_t>(offset), SEEK_SET) == 0;
}

std::size_t ReadStdFile(void*, void* raw_file, std::uint8_t* out, std::size_t size) {
    return std::fread(out, 1, size, static_cast<FILE*>(raw_file));
}

void CloseStdFile(void*, void* raw_file) {
    std::fclose(static_cast<FILE*>(raw_file));
}

LessonRgb565FileOps DefaultFileOps() {
    return {nullptr, OpenStdFile, SizeStdFile, SeekStdFile, ReadStdFile, CloseStdFile};
}

bool ValidFileOps(const LessonRgb565FileOps& ops) {
    return ops.open != nullptr && ops.size != nullptr && ops.seek != nullptr &&
           ops.read != nullptr && ops.close != nullptr;
}

}  // namespace

LessonRgb565Status LessonRgb565Reader::Open(const LessonRgb565Io& io) {
    *this = LessonRgb565Reader{};
    if (io.read_at == nullptr || io.file_size == 0) return LessonRgb565Status::kInvalidArgument;
    if (io.file_size < kHeaderBytes) return LessonRgb565Status::kTruncated;

    std::array<std::uint8_t, kHeaderBytes> header{};
    if (!io.read_at(io.context, 0, header.data(), header.size())) return LessonRgb565Status::kIoError;
    const std::uint32_t stored_header_crc = Le32(header.data() + 52);
    std::fill(header.begin() + 52, header.begin() + 56, 0);
    if (Crc32(header.data(), header.size()) != stored_header_crc) {
        return LessonRgb565Status::kHeaderCrcMismatch;
    }
    if (std::memcmp(header.data(), kMagic, 8) != 0 || Le16(header.data() + 8) != kHeaderBytes ||
        Le16(header.data() + 10) != 1) {
        return LessonRgb565Status::kMalformed;
    }

    LessonRgb565Metadata metadata{};
    metadata.stored_width = Le16(header.data() + 12);
    metadata.stored_height = Le16(header.data() + 14);
    metadata.display_width = Le16(header.data() + 16);
    metadata.display_height = Le16(header.data() + 18);
    const std::uint16_t fps_numerator = Le16(header.data() + 20);
    const std::uint16_t fps_denominator = Le16(header.data() + 22);
    metadata.fps = fps_numerator;
    metadata.frame_count = Le32(header.data() + 24);
    metadata.duration_ms = Le32(header.data() + 28);
    metadata.frame_bytes = Le32(header.data() + 32);
    const std::uint32_t index_offset = Le32(header.data() + 36);
    const std::uint32_t data_offset = Le32(header.data() + 40);
    const std::uint32_t pixel_format = Le32(header.data() + 44);
    const std::uint32_t orientation = Le32(header.data() + 48);
    const std::uint64_t file_bytes = Le64(header.data() + 56);
    metadata.file_bytes = file_bytes;

    if (metadata.stored_width != 320 || metadata.stored_height != 480 ||
        metadata.display_width != 480 || metadata.display_height != 320 ||
        fps_numerator != 10 || fps_denominator != 1 || metadata.frame_bytes != kLessonRgb565FrameBytes ||
        pixel_format != 1 || orientation != 1) {
        return LessonRgb565Status::kUnsupported;
    }
    if (metadata.frame_count == 0 || index_offset != kHeaderBytes) {
        return LessonRgb565Status::kMalformed;
    }
    if (metadata.frame_count > std::numeric_limits<std::uint32_t>::max() / 100U) {
        return LessonRgb565Status::kOverflow;
    }
    if (metadata.duration_ms != metadata.frame_count * 100U) {
        return LessonRgb565Status::kMalformed;
    }

    std::uint64_t index_bytes = 0;
    std::uint64_t index_end = 0;
    std::uint64_t aligned_index_end = 0;
    if (!CheckedMultiply(metadata.frame_count, kIndexRecordBytes, &index_bytes) ||
        !CheckedAdd(index_offset, index_bytes, &index_end) ||
        !CheckedAdd(index_end, kSectorBytes - 1, &aligned_index_end)) {
        return LessonRgb565Status::kOverflow;
    }
    aligned_index_end &= ~(static_cast<std::uint64_t>(kSectorBytes) - 1U);
    if (aligned_index_end > std::numeric_limits<std::uint32_t>::max()) {
        return LessonRgb565Status::kOverflow;
    }
    if (data_offset != aligned_index_end || data_offset % kSectorBytes != 0) {
        return LessonRgb565Status::kMalformed;
    }
    if (index_end > io.file_size) return LessonRgb565Status::kTruncated;

    std::uint64_t payload_bytes = 0;
    std::uint64_t expected_file_bytes = 0;
    if (!CheckedMultiply(metadata.frame_count, metadata.frame_bytes, &payload_bytes) ||
        !CheckedAdd(data_offset, payload_bytes, &expected_file_bytes)) {
        return LessonRgb565Status::kOverflow;
    }
    if (file_bytes != expected_file_bytes) return LessonRgb565Status::kMalformed;
    if (io.file_size < file_bytes) return LessonRgb565Status::kTruncated;
    if (io.file_size != file_bytes) return LessonRgb565Status::kMalformed;

    LessonRgb565Reader candidate;
    candidate.io_ = io;
    candidate.metadata_ = metadata;
    candidate.index_offset_ = index_offset;
    candidate.data_offset_ = data_offset;
    candidate.file_bytes_ = file_bytes;

    std::uint64_t expected_offset = data_offset;
    for (std::uint32_t index = 0; index < metadata.frame_count; ++index) {
        std::uint64_t frame_offset = 0;
        std::uint32_t frame_length = 0;
        std::uint32_t frame_crc = 0;
        const LessonRgb565Status status = candidate.ReadIndexRecord(
            index, &frame_offset, &frame_length, &frame_crc);
        (void)frame_crc;
        if (status != LessonRgb565Status::kOk) return status;
        if (frame_length != kLessonRgb565FrameBytes) return LessonRgb565Status::kUnsupported;
        if (frame_offset != expected_offset || frame_offset % kSectorBytes != 0) {
            return LessonRgb565Status::kMalformed;
        }
        if (!CheckedAdd(frame_offset, frame_length, &expected_offset)) {
            return LessonRgb565Status::kOverflow;
        }
        if (expected_offset > file_bytes) return LessonRgb565Status::kTruncated;
    }
    if (expected_offset != file_bytes) return LessonRgb565Status::kMalformed;

    std::array<std::uint8_t, 64> padding{};
    std::uint64_t padding_offset = index_end;
    while (padding_offset < data_offset) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(padding.size(), data_offset - padding_offset));
        if (!io.read_at(io.context, padding_offset, padding.data(), chunk)) {
            return LessonRgb565Status::kIoError;
        }
        for (std::size_t index = 0; index < chunk; ++index) {
            if (padding[index] != 0) return LessonRgb565Status::kMalformed;
        }
        padding_offset += chunk;
    }

    *this = candidate;
    return LessonRgb565Status::kOk;
}

LessonRgb565Status LessonRgb565Reader::ReadIndexRecord(
    std::uint32_t index,
    std::uint64_t* offset,
    std::uint32_t* length,
    std::uint32_t* crc
) const {
    if (offset == nullptr || length == nullptr || crc == nullptr || index >= metadata_.frame_count) {
        return LessonRgb565Status::kInvalidArgument;
    }
    std::uint64_t record_delta = 0;
    std::uint64_t record_offset = 0;
    if (!CheckedMultiply(index, kIndexRecordBytes, &record_delta) ||
        !CheckedAdd(index_offset_, record_delta, &record_offset)) {
        return LessonRgb565Status::kOverflow;
    }
    if (record_offset > io_.file_size || kIndexRecordBytes > io_.file_size - record_offset) {
        return LessonRgb565Status::kTruncated;
    }
    std::array<std::uint8_t, kIndexRecordBytes> record{};
    if (!io_.read_at(io_.context, record_offset, record.data(), record.size())) {
        return LessonRgb565Status::kIoError;
    }
    *offset = Le64(record.data());
    *length = Le32(record.data() + 8);
    *crc = Le32(record.data() + 12);
    return LessonRgb565Status::kOk;
}

LessonRgb565Status LessonRgb565Reader::ReadFrame(
    std::uint32_t index,
    std::uint8_t* destination,
    std::size_t capacity
) const {
    if (index >= metadata_.frame_count) return LessonRgb565Status::kFrameOutOfRange;
    if (destination == nullptr) return LessonRgb565Status::kInvalidArgument;
    if (capacity < metadata_.frame_bytes) return LessonRgb565Status::kSmallBuffer;

    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t expected_crc = 0;
    LessonRgb565Status status = ReadIndexRecord(index, &offset, &length, &expected_crc);
    if (status != LessonRgb565Status::kOk) return status;

    std::uint64_t expected_offset = 0;
    std::uint64_t frame_delta = 0;
    if (!CheckedMultiply(index, kLessonRgb565FrameBytes, &frame_delta) ||
        !CheckedAdd(data_offset_, frame_delta, &expected_offset)) {
        return LessonRgb565Status::kOverflow;
    }
    std::uint64_t frame_end = 0;
    if (offset != expected_offset || offset % kSectorBytes != 0) return LessonRgb565Status::kMalformed;
    if (length != kLessonRgb565FrameBytes) return LessonRgb565Status::kUnsupported;
    if (!CheckedAdd(offset, length, &frame_end)) return LessonRgb565Status::kOverflow;
    if (frame_end > file_bytes_ || frame_end > io_.file_size) return LessonRgb565Status::kTruncated;
    if (!io_.read_at(io_.context, offset, destination, length)) return LessonRgb565Status::kIoError;
    return Crc32(destination, length) == expected_crc ? LessonRgb565Status::kOk
                                                      : LessonRgb565Status::kFrameCrcMismatch;
}

LessonRgb565File::~LessonRgb565File() {
    Close();
}

LessonRgb565Status LessonRgb565File::OpenUnderLessonSession(
    const char* path,
    const std::string& assignment_id,
    const std::string& session_id,
    std::uint64_t generation,
    LessonRgb565FileOps ops
) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    CloseLocked();
    if (!IsSafeLessonAssetPath(path)) return LessonRgb565Status::kUnsafePath;
    if (ops.open == nullptr) ops = DefaultFileOps();
    if (!ValidFileOps(ops)) return LessonRgb565Status::kInvalidArgument;

    LessonAssetReadLease read_lease = LessonAssetStorageCoordinator::GetInstance().TryRetainLessonSession(
        assignment_id, session_id, generation);
    if (!read_lease) return LessonRgb565Status::kLeaseUnavailable;

    void* file = ops.open(ops.context, path);
    if (file == nullptr) return LessonRgb565Status::kIoError;
    std::uint64_t file_size = 0;
    if (!ops.size(ops.context, file, &file_size)) {
        ops.close(ops.context, file);
        return LessonRgb565Status::kIoError;
    }

    file_ = file;
    ops_ = ops;
    lesson_read_lease_ = std::move(read_lease);
    assignment_id_ = assignment_id;
    session_id_ = session_id;
    generation_ = generation;
    const LessonRgb565Status status = reader_.Open({this, ReadAt, file_size});
    if (status != LessonRgb565Status::kOk) CloseLocked();
    return status;
}

LessonRgb565Status LessonRgb565File::ReadFrame(
    std::uint32_t index,
    std::uint8_t* destination,
    std::size_t capacity
) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (file_ == nullptr) return LessonRgb565Status::kInvalidArgument;
    LessonAssetReadLease active_lease = LessonAssetStorageCoordinator::GetInstance().TryRetainLessonSession(
        assignment_id_, session_id_, generation_);
    if (!active_lease) return LessonRgb565Status::kLeaseUnavailable;
    return reader_.ReadFrame(index, destination, capacity);
}

void LessonRgb565File::Close() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    CloseLocked();
}

void LessonRgb565File::CloseLocked() {
    reader_ = LessonRgb565Reader{};
    if (file_ != nullptr) {
        ops_.close(ops_.context, file_);
        file_ = nullptr;
    }
    ops_ = {};
    lesson_read_lease_ = {};
    assignment_id_.clear();
    session_id_.clear();
    generation_ = 0;
}

bool LessonRgb565File::is_open() const {
    std::lock_guard<std::mutex> lock(io_mutex_);
    return file_ != nullptr;
}

LessonRgb565Metadata LessonRgb565File::metadata() const {
    std::lock_guard<std::mutex> lock(io_mutex_);
    return reader_.metadata();
}

bool LessonRgb565File::ReadAt(
    void* context,
    std::uint64_t offset,
    std::uint8_t* out,
    std::size_t size
) {
    auto* self = static_cast<LessonRgb565File*>(context);
    return self != nullptr && self->file_ != nullptr &&
           self->ops_.seek(self->ops_.context, self->file_, offset) &&
           self->ops_.read(self->ops_.context, self->file_, out, size) == size;
}

}  // namespace tbot
