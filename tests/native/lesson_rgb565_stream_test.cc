#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "lesson_asset_storage_coordinator.h"
#include "lesson_rgb565_stream.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kIndexRecordBytes = 16;
constexpr std::size_t kFrameBytes = 320U * 480U * 2U;
constexpr char kSafePath[] = "/sdcard/tbot/lesson-assets/w01-d01/v1-test/cue.trgb";

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson_rgb565_stream test failed: " << message << '\n';
        std::exit(1);
    }
}

void PutLe16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void PutLe32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void PutLe64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::uint32_t Crc32(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::uint32_t Align512(std::uint32_t value) {
    return (value + 511U) & ~511U;
}

void RefreshHeaderCrc(Bytes& bytes) {
    PutLe32(bytes, 52, 0);
    PutLe32(bytes, 52, Crc32(bytes.data(), kHeaderBytes));
}

Bytes MakeStream(std::uint32_t frame_count = 2) {
    const std::uint32_t index_offset = 64;
    const std::uint32_t data_offset = Align512(index_offset + frame_count * kIndexRecordBytes);
    const std::uint64_t file_bytes = static_cast<std::uint64_t>(data_offset) +
                                     static_cast<std::uint64_t>(frame_count) * kFrameBytes;
    Bytes bytes(static_cast<std::size_t>(file_bytes), 0);
    std::memcpy(bytes.data(), "TBOTRGB1", 8);
    PutLe16(bytes, 8, 64);
    PutLe16(bytes, 10, 1);
    PutLe16(bytes, 12, 320);
    PutLe16(bytes, 14, 480);
    PutLe16(bytes, 16, 480);
    PutLe16(bytes, 18, 320);
    PutLe16(bytes, 20, 10);
    PutLe16(bytes, 22, 1);
    PutLe32(bytes, 24, frame_count);
    PutLe32(bytes, 28, frame_count * 100U);
    PutLe32(bytes, 32, kFrameBytes);
    PutLe32(bytes, 36, index_offset);
    PutLe32(bytes, 40, data_offset);
    PutLe32(bytes, 44, 1);
    PutLe32(bytes, 48, 1);
    PutLe64(bytes, 56, file_bytes);

    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        const std::uint64_t frame_offset = static_cast<std::uint64_t>(data_offset) +
                                           static_cast<std::uint64_t>(frame) * kFrameBytes;
        std::fill_n(bytes.data() + static_cast<std::size_t>(frame_offset), kFrameBytes,
                    static_cast<std::uint8_t>(0x20U + frame));
        const std::size_t record = index_offset + frame * kIndexRecordBytes;
        PutLe64(bytes, record, frame_offset);
        PutLe32(bytes, record + 8, kFrameBytes);
        PutLe32(bytes, record + 12,
                Crc32(bytes.data() + static_cast<std::size_t>(frame_offset), kFrameBytes));
    }
    RefreshHeaderCrc(bytes);
    return bytes;
}

struct FakeFile {
    Bytes bytes;
    std::uint64_t cursor = 0;
    int opens = 0;
    int closes = 0;
};

void* FakeOpen(void* context, const char*) {
    auto* file = static_cast<FakeFile*>(context);
    ++file->opens;
    return file;
}

bool FakeSize(void*, void* raw_file, std::uint64_t* size) {
    *size = static_cast<FakeFile*>(raw_file)->bytes.size();
    return true;
}

bool FakeSeek(void*, void* raw_file, std::uint64_t offset) {
    auto* file = static_cast<FakeFile*>(raw_file);
    if (offset > file->bytes.size()) return false;
    file->cursor = offset;
    return true;
}

std::size_t FakeRead(void*, void* raw_file, std::uint8_t* out, std::size_t size) {
    auto* file = static_cast<FakeFile*>(raw_file);
    if (file->cursor > file->bytes.size() ||
        size > file->bytes.size() - static_cast<std::size_t>(file->cursor)) {
        return 0;
    }
    std::memcpy(out, file->bytes.data() + static_cast<std::size_t>(file->cursor), size);
    file->cursor += size;
    return size;
}

void FakeClose(void*, void* raw_file) {
    ++static_cast<FakeFile*>(raw_file)->closes;
}

tbot::LessonRgb565FileOps Ops(FakeFile* file) {
    return {file, FakeOpen, FakeSize, FakeSeek, FakeRead, FakeClose};
}

struct Session {
    std::string assignment;
    std::string session;
    std::uint64_t generation;

    Session() : assignment("assignment-rgb565"), session("session-rgb565") {
        const auto result = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession(
            assignment, session);
        Require(result.acquired, "lesson session begins");
        generation = result.generation;
    }

    ~Session() {
        LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    }
};

tbot::LessonRgb565Status Open(
    tbot::LessonRgb565File& stream,
    FakeFile& file,
    const Session& session,
    const char* path = kSafePath
) {
    return stream.OpenUnderLessonSession(
        path, session.assignment, session.session, session.generation, Ops(&file));
}

void ExpectOpenStatus(Bytes bytes, tbot::LessonRgb565Status expected, const char* message) {
    Session session;
    FakeFile file{std::move(bytes)};
    tbot::LessonRgb565File stream;
    Require(Open(stream, file, session) == expected, message);
}

void TestValidOpenAndFrameRead() {
    Session session;
    FakeFile file{MakeStream()};
    tbot::LessonRgb565File stream;
    Require(Open(stream, file, session) == tbot::LessonRgb565Status::kOk,
            "valid TRGB opens");
    const auto metadata = stream.metadata();
    Require(metadata.stored_width == 320 && metadata.stored_height == 480,
            "panel-native geometry is parsed");
    Require(metadata.display_width == 480 && metadata.display_height == 320,
            "display geometry is parsed");
    Require(metadata.fps == 10 && metadata.frame_count == 2 && metadata.duration_ms == 200,
            "timeline metadata is parsed");
    Require(metadata.frame_bytes == kFrameBytes, "frame size metadata is parsed");

    Bytes output(kFrameBytes);
    Require(stream.ReadFrame(1, output.data(), output.size()) == tbot::LessonRgb565Status::kOk,
            "indexed frame reads and verifies CRC");
    Require(std::all_of(output.begin(), output.end(), [](std::uint8_t value) { return value == 0x21; }),
            "requested frame payload is returned");
    Require(stream.ReadFrame(2, output.data(), output.size()) ==
                tbot::LessonRgb565Status::kFrameOutOfRange,
            "out-of-range frame is rejected");
    Require(stream.ReadFrame(0, output.data(), output.size() - 1) ==
                tbot::LessonRgb565Status::kSmallBuffer,
            "small output buffer is rejected");
}

void TestHeaderRejections() {
    Bytes bytes = MakeStream();
    bytes[0] = 'X';
    RefreshHeaderCrc(bytes);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kMalformed,
                     "bad magic is rejected");

    bytes = MakeStream();
    bytes[12] ^= 1;
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kHeaderCrcMismatch,
                     "bad header CRC is rejected");

    bytes = MakeStream();
    PutLe32(bytes, 24, std::numeric_limits<std::uint32_t>::max());
    PutLe32(bytes, 28, 0);
    RefreshHeaderCrc(bytes);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kOverflow,
                     "duration arithmetic overflow is rejected");

    bytes = MakeStream();
    PutLe16(bytes, 12, 321);
    RefreshHeaderCrc(bytes);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kUnsupported,
                     "wrong geometry is rejected");

    bytes = MakeStream();
    PutLe16(bytes, 20, 20);
    RefreshHeaderCrc(bytes);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kUnsupported,
                     "wrong timing is rejected");

    bytes = MakeStream();
    PutLe32(bytes, 44, 2);
    RefreshHeaderCrc(bytes);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kUnsupported,
                     "wrong pixel format is rejected");

    bytes = MakeStream();
    PutLe32(bytes, 48, 2);
    RefreshHeaderCrc(bytes);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kUnsupported,
                     "wrong orientation is rejected");
}

void TestIndexAndFileRejections() {
    Bytes bytes = MakeStream();
    PutLe64(bytes, 64, 513);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kMalformed,
                     "non-aligned frame offset is rejected");

    bytes = MakeStream();
    PutLe64(bytes, 80, 512U + kFrameBytes + 512U);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kMalformed,
                     "noncontiguous frame offset is rejected");

    bytes = MakeStream();
    PutLe32(bytes, 64 + 8, kFrameBytes - 2);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kUnsupported,
                     "wrong indexed frame size is rejected");

    bytes = MakeStream();
    bytes.resize(79);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kTruncated,
                     "truncated index is rejected");

    bytes = MakeStream();
    PutLe64(bytes, 56, bytes.size() - 1);
    RefreshHeaderCrc(bytes);
    ExpectOpenStatus(std::move(bytes), tbot::LessonRgb565Status::kMalformed,
                     "declared file size must be exact");
}

void TestFrameCrcAndLeaseLoss() {
    Session session;
    FakeFile file{MakeStream()};
    tbot::LessonRgb565File stream;
    Require(Open(stream, file, session) == tbot::LessonRgb565Status::kOk,
            "CRC fixture opens");
    file.bytes.back() ^= 0xff;
    Bytes output(kFrameBytes);
    Require(stream.ReadFrame(1, output.data(), output.size()) ==
                tbot::LessonRgb565Status::kFrameCrcMismatch,
            "frame CRC mismatch is rejected");

    LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
    Require(stream.ReadFrame(0, output.data(), output.size()) ==
                tbot::LessonRgb565Status::kLeaseUnavailable,
            "lost lesson session lease rejects frame reads");
}

void TestUnsafePaths() {
    Session session;
    FakeFile file{MakeStream()};
    tbot::LessonRgb565File stream;
    Require(Open(stream, file, session,
                 "/sdcard/tbot/lesson-assets/w01-d01/../secret.trgb") ==
                tbot::LessonRgb565Status::kUnsafePath,
            "path traversal is rejected");
    Require(file.opens == 0, "unsafe path is rejected before file open");
    Require(Open(stream, file, session, "/tmp/cue.trgb") ==
                tbot::LessonRgb565Status::kUnsafePath,
            "non-SD lesson path is rejected");
}

}  // namespace

int main() {
    TestValidOpenAndFrameRead();
    TestHeaderRejections();
    TestIndexAndFileRejections();
    TestFrameCrcAndLeaseLoss();
    TestUnsafePaths();
    std::cout << "lesson RGB565 stream host tests passed\n";
    return 0;
}
