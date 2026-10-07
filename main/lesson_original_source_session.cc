#include "lesson_original_source_session.h"
#include "lesson_original_source_profile.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <climits>
#include <memory>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
#include <libavutil/intreadwrite.h>
}
namespace tbot {
namespace {
// The snapshot is read with POSIX read(): on the ESP32-S3, unbuffered stdio fread()
// reads byte by byte (~90 ms per 4 KiB measured on the robot, 256 KiB in 5.8 s versus
// 53 ms with read(), BE08 R16), and a buffered FILE would allocate a hidden buffer.
class SnapshotFile {
public:
    explicit SnapshotFile(const char* path) : fd_(::open(path, O_RDONLY)) {}
    ~SnapshotFile() { Close(); }
    SnapshotFile(const SnapshotFile&) = delete;
    SnapshotFile& operator=(const SnapshotFile&) = delete;
    explicit operator bool() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    void Close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    // Reads exactly `count` bytes; false on error or early end of file.
    bool ReadFully(uint8_t* output, size_t count) {
        while (count > 0) {
            const ssize_t got = ::read(fd_, output, count);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return false;
            output += got;
            count -= static_cast<size_t>(got);
        }
        return true;
    }

private:
    int fd_;
};
OriginalSourceStatus Error(int error) {
    return error == AVERROR(ENOMEM) ? OriginalSourceStatus::kNoMemory
                                   : OriginalSourceStatus::kDecode;
}
}
OriginalSourceSession::~OriginalSourceSession() { Close(); }
bool OriginalSourceSession::Cancelled() const {
    return cancelled_ && cancelled_->load(std::memory_order_relaxed);
}
bool OriginalSourceSession::AllocationFailed() const {
    return allocations_ && allocations_->failed();
}
int OriginalSourceSession::Interrupted(void* opaque) {
    return static_cast<OriginalSourceSession*>(opaque)->Cancelled();
}
int OriginalSourceSession::Read(void* opaque, uint8_t* output, int count) {
    auto& self = *static_cast<OriginalSourceSession*>(opaque);
    if (self.Cancelled()) return AVERROR_EXIT;
    if (count <= 0) return AVERROR(EINVAL);
    const size_t remaining = self.expected_.bytes - self.position_;
    if (!remaining) return AVERROR_EOF;
    const size_t copied = std::min(remaining, static_cast<size_t>(count));
    std::memcpy(output, self.bytes_ + self.position_, copied);
    self.position_ += copied;
    return static_cast<int>(copied);
}
int64_t OriginalSourceSession::Seek(void* opaque, int64_t offset, int whence) {
    auto& self = *static_cast<OriginalSourceSession*>(opaque);
    if (self.Cancelled()) return AVERROR_EXIT;
    if (whence == AVSEEK_SIZE) return self.expected_.bytes;
    whence &= ~AVSEEK_FORCE;
    int64_t base = 0;
    if (whence == SEEK_CUR) base = self.position_;
    else if (whence == SEEK_END) base = self.expected_.bytes;
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (offset < -base || offset > static_cast<int64_t>(self.expected_.bytes) - base)
        return AVERROR(EINVAL);
    self.position_ = static_cast<size_t>(base + offset);
    return self.position_;
}
void OriginalSourceSession::Close() {
    for (auto& side : vp9_) {
        av_packet_free(&side.pending);
        av_frame_free(&side.scratch);
        av_frame_free(&side.output);
        for (auto*& queued : side.queue) av_frame_free(&queued);
        avcodec_free_context(&side.codec);
        side = {};
    }
    queued_bytes_ = 0;
    demux_eof_ = ended_ = false;
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_);
    avformat_close_input(&format_);
    if (io_) {
        // libavformat may replace the original AVIO buffer during probing.
        av_freep(&io_->buffer);
        avio_context_free(&io_);
    }
    av_freep(&bytes_);
    if (allocations_) allocations_->Release();
    allocations_ = nullptr;
    cancelled_ = nullptr;
    position_ = 0;
    frames_ = 0;
    stream_ = -1;
    flushing_ = false;
}
OriginalSourceStatus OriginalSourceSession::Fail(OriginalSourceStatus status) {
    const auto* allocations = allocations_;
    const auto* cancelled = cancelled_;
    Close();
    // Cleanup can trigger allocation failures or cancellation.
    if (status == OriginalSourceStatus::kCancelled ||
        (cancelled && cancelled->load(std::memory_order_relaxed)))
        return OriginalSourceStatus::kCancelled;
    if (allocations && allocations->failed()) return OriginalSourceStatus::kNoMemory;
    return status;
}
OriginalSourceStatus OriginalSourceSession::Open(
    const char* path, const OriginalSourceExpected& expected, LessonAssetReadLease lease,
    OriginalSourceAllocationState* allocations, const std::atomic<bool>* cancelled) {
    Close();
    if (!allocations) return OriginalSourceStatus::kInvalid;
    if (!lease) return OriginalSourceStatus::kLeaseUnavailable;
    allocations_ = allocations;
    allocations_->Acquire();
    if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    expected_ = expected;
    cancelled_ = cancelled;
    if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
    if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    if (!path || !*path || !expected.bytes || expected.bytes > kMaxBytes ||
        !expected.width || expected.width > 512 || !expected.height ||
        expected.height > 512 || !expected.frames || expected.frames > 10000)
        return Fail(OriginalSourceStatus::kInvalid);
    AVCodecID id;
    switch (expected.codec) {
        case OriginalSourceCodec::kH264: id = AV_CODEC_ID_H264; break;
        case OriginalSourceCodec::kVp9Alpha: id = AV_CODEC_ID_VP9; break;
        case OriginalSourceCodec::kPng: id = AV_CODEC_ID_PNG; break;
        default: return Fail(OriginalSourceStatus::kUnsupported);
    }
    SnapshotFile file(path);
    if (!file) return Fail(OriginalSourceStatus::kIo);
    const off_t size = ::lseek(file.fd(), 0, SEEK_END);
    if (size < 0) return Fail(OriginalSourceStatus::kIo);
    if (static_cast<size_t>(size) != expected.bytes) return Fail(OriginalSourceStatus::kIntegrity);
    if (::lseek(file.fd(), 0, SEEK_SET) != 0) return Fail(OriginalSourceStatus::kIo);
    bytes_ = static_cast<uint8_t*>(av_malloc(expected.bytes));
    if (!bytes_) return Fail(OriginalSourceStatus::kNoMemory);
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), &av_free);
    if (!sha) return Fail(OriginalSourceStatus::kNoMemory);
    if (av_sha_init(sha.get(), 256)) return Fail(OriginalSourceStatus::kDecode);
    for (size_t read = 0; read < expected.bytes;) {
        if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
        if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
        size_t count = std::min(size_t{4096}, expected.bytes - read);
        {
            auto& profile = tbot::OriginalSourceProfileCounters();
            std::uint64_t read_us = 0;
            bool ok;
            {
                tbot::OriginalSourceProfileScope timed(&read_us);
                ok = file.ReadFully(bytes_ + read, count);
            }
            profile.read_us += read_us;
            if (read == 0) profile.first_read_us += read_us;
            profile.max_read_us = std::max(profile.max_read_us, read_us);
            profile.slow_reads += read_us > 100000;
            if (!ok) return Fail(OriginalSourceStatus::kIo);
        }
        {
            tbot::OriginalSourceProfileScope timed(&tbot::OriginalSourceProfileCounters().hash_us);
            av_sha_update(sha.get(), bytes_ + read, count);
        }
        read += count;
    }
    {
        // The snapshot must end exactly at the expected length.
        uint8_t extra = 0;
        ssize_t tail;
        do tail = ::read(file.fd(), &extra, 1); while (tail < 0 && errno == EINTR);
        if (tail != 0) return Fail(OriginalSourceStatus::kIntegrity);
    }
    uint8_t hash[32];
    av_sha_final(sha.get(), hash);
    if (std::memcmp(hash, expected.sha256, sizeof(hash)))
        return Fail(OriginalSourceStatus::kIntegrity);
    file.Close();
    sha.reset();
    if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
    if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    uint8_t* buffer = static_cast<uint8_t*>(av_malloc(4096));
    if (!buffer) return Fail(OriginalSourceStatus::kNoMemory);
    io_ = avio_alloc_context(buffer, 4096, 0, this, &Read, nullptr, &Seek);
    if (!io_) {
        av_free(buffer);
        return Fail(OriginalSourceStatus::kNoMemory);
    }
    format_ = avformat_alloc_context();
    if (!format_) return Fail(OriginalSourceStatus::kNoMemory);
    format_->pb = io_;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;
    format_->interrupt_callback = {&Interrupted, this};
    int result;
    {
        tbot::OriginalSourceProfileScope timed(&tbot::OriginalSourceProfileCounters().demux_open_us);
        result = avformat_open_input(&format_, nullptr, nullptr, nullptr);
    }
    if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
    if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    if (result < 0) return Fail(Error(result));
    stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_ < 0) return Fail(OriginalSourceStatus::kUnsupported);
    AVCodecParameters* parameters = format_->streams[stream_]->codecpar;
    if (parameters->codec_id != id) return Fail(OriginalSourceStatus::kMetadata);
    if ((parameters->width && parameters->width != static_cast<int>(expected.width)) ||
        (parameters->height && parameters->height != static_cast<int>(expected.height)))
        return Fail(OriginalSourceStatus::kMetadata);
    const AVCodec* decoder = avcodec_find_decoder(id);
    if (!decoder) return Fail(OriginalSourceStatus::kUnsupported);
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_) return Fail(OriginalSourceStatus::kNoMemory);
    result = avcodec_parameters_to_context(codec_, parameters);
    if (result < 0) return Fail(Error(result));
    codec_->thread_count = 1;
    codec_->max_pixels = expected.width * expected.height;
    codec_->pkt_timebase = format_->streams[stream_]->time_base;
    {
        tbot::OriginalSourceProfileScope timed(&tbot::OriginalSourceProfileCounters().codec_open_us);
        result = avcodec_open2(codec_, decoder, nullptr);
    }
    if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
    if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    if (result < 0) return Fail(Error(result));
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
    if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    if (!packet_ || !frame_) return Fail(OriginalSourceStatus::kNoMemory);
    if (expected.codec == OriginalSourceCodec::kVp9Alpha) {
        // Native VP9 elementary alpha does not inherit container color extradata.
        vp9_[0].codec = codec_;
        codec_ = nullptr;
        vp9_[1].codec = avcodec_alloc_context3(decoder);
        if (!vp9_[1].codec) return Fail(OriginalSourceStatus::kNoMemory);
        vp9_[1].codec->thread_count = 1;
        vp9_[1].codec->max_pixels = expected.width * expected.height;
        vp9_[1].codec->pkt_timebase = format_->streams[stream_]->time_base;
        {
            tbot::OriginalSourceProfileScope timed(&tbot::OriginalSourceProfileCounters().codec_open_us);
            result = avcodec_open2(vp9_[1].codec, decoder, nullptr);
        }
        if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
        if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
        if (result < 0) return Fail(Error(result));
        for (auto& side : vp9_) {
            side.pending = av_packet_alloc();
            side.scratch = av_frame_alloc();
            side.output = av_frame_alloc();
            if (!side.pending || !side.scratch || !side.output)
                return Fail(OriginalSourceStatus::kNoMemory);
        }
        if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
        if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    }
    return OriginalSourceStatus::kOk;
}
namespace {
size_t RetainedBytes(const AVFrame* frame) {
    size_t bytes = 0;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i)
        if (frame->buf[i]) bytes += frame->buf[i]->size;
    for (int i = 0; i < frame->nb_extended_buf; ++i)
        bytes += frame->extended_buf[i]->size;
    return bytes;
}
}
OriginalSourceStatus OriginalSourceSession::NextVp9(OriginalSourceFrame* output) {
    for (auto& side : vp9_) av_frame_unref(side.output);
    for (;;) {
        if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
        if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
        if (vp9_[0].depth && vp9_[1].depth) {
            if (vp9_[0].queue[0]->pts != vp9_[1].queue[0]->pts)
                return Fail(OriginalSourceStatus::kMetadata);
            if (++frames_ > expected_.frames) return Fail(OriginalSourceStatus::kMetadata);
            for (auto& side : vp9_) {
                queued_bytes_ -= RetainedBytes(side.queue[0]);
                av_frame_move_ref(side.output, side.queue[0]);
                av_frame_free(&side.queue[0]);
                for (size_t i = 1; i < side.depth; ++i) side.queue[i-1] = side.queue[i];
                side.queue[--side.depth] = nullptr;
            }
            if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
            if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
            const auto* color = vp9_[0].output;
            for (int i = 0; i < 3; ++i) {
                output->planes[i] = color->data[i];
                output->strides[i] = color->linesize[i];
            }
            output->planes[3] = vp9_[1].output->data[0];
            output->strides[3] = vp9_[1].output->linesize[0];
            output->width = color->width;
            output->height = color->height;
            output->alpha = true;
            output->pts = color->pts;
            output->time_base_num = format_->streams[stream_]->time_base.num;
            output->time_base_den = format_->streams[stream_]->time_base.den;
            return OriginalSourceStatus::kOk;
        }
        bool progress = false;
        for (auto& side : vp9_) {
            if (side.eof) continue;
            int result = avcodec_receive_frame(side.codec, side.scratch);
            if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
            if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
            if (result == 0) {
                auto* received = side.scratch;
                if (received->pts == AV_NOPTS_VALUE ||
                    (side.has_pts && received->pts <= side.last_pts) ||
                    received->width != static_cast<int>(expected_.width) ||
                    received->height != static_cast<int>(expected_.height))
                    return Fail(OriginalSourceStatus::kMetadata);
                if (received->format != AV_PIX_FMT_YUV420P)
                    return Fail(OriginalSourceStatus::kUnsupported);
                const size_t bytes = RetainedBytes(received);
                if (side.depth == kQueueFrames || bytes > kQueueBytes - queued_bytes_)
                    return Fail(OriginalSourceStatus::kNoMemory);
                AVFrame* owned = av_frame_alloc();
                if (!owned) return Fail(OriginalSourceStatus::kNoMemory);
                av_frame_move_ref(owned, received);
                side.queue[side.depth++] = owned;
                queued_bytes_ += bytes;
                side.last_pts = owned->pts;
                side.has_pts = true;
                progress = true;
            } else if (result == AVERROR_EOF) {
                if (!side.drain_sent) return Fail(OriginalSourceStatus::kDecode);
                side.eof = true;
                progress = true;
            } else if (result != AVERROR(EAGAIN)) return Fail(Error(result));
        }
        if (vp9_[0].depth && vp9_[1].depth) continue;
        if (vp9_[0].eof && vp9_[1].eof) {
            const bool complete = !vp9_[0].depth && !vp9_[1].depth &&
                !vp9_[0].has_pending && !vp9_[1].has_pending && frames_ == expected_.frames;
            const auto status = Fail(complete ? OriginalSourceStatus::kEnd
                                             : OriginalSourceStatus::kMetadata);
            ended_ = status == OriginalSourceStatus::kEnd;
            return status;
        }
        if (!vp9_[0].has_pending && !vp9_[1].has_pending && !demux_eof_) {
            int result;
            do {
                av_packet_unref(packet_);
                result = av_read_frame(format_, packet_);
                if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
                if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
            } while (result >= 0 && packet_->stream_index != stream_);
            if (result == AVERROR_EOF) demux_eof_ = true;
            else if (result < 0) return Fail(Error(result));
            else {
                size_t size = 0;
                const uint8_t* data = av_packet_get_side_data(
                    packet_, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &size);
                if (!data || size <= 8 || AV_RB64(data) != 1 ||
                    size - 8 > static_cast<size_t>(INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE))
                    return Fail(OriginalSourceStatus::kMetadata);
                result = av_new_packet(vp9_[1].pending, static_cast<int>(size - 8));
                if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
                if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
                if (result < 0) return Fail(Error(result));
                std::memcpy(vp9_[1].pending->data, data + 8, size - 8);
                vp9_[1].pending->pts = packet_->pts;
                vp9_[1].pending->dts = packet_->dts;
                vp9_[1].pending->duration = packet_->duration;
                vp9_[1].pending->time_base = format_->streams[stream_]->time_base;
                // Only corruption is shared; keyframe/discard flags describe color coding.
                vp9_[1].pending->flags = packet_->flags & AV_PKT_FLAG_CORRUPT;
                av_packet_move_ref(vp9_[0].pending, packet_);
                vp9_[0].has_pending = vp9_[1].has_pending = true;
            }
            progress = true;
        }
        for (auto& side : vp9_) {
            const bool drain = demux_eof_ && !side.has_pending && !side.drain_sent;
            if (!side.has_pending && !drain) continue;
            int result = avcodec_send_packet(side.codec, drain ? nullptr : side.pending);
            if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
            if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
            if (result == 0) {
                if (drain) side.drain_sent = true;
                else { av_packet_unref(side.pending); side.has_pending = false; }
                progress = true;
            } else if (result != AVERROR(EAGAIN)) return Fail(Error(result));
        }
        // Neither decoder may demand receive and input simultaneously without advancing.
        if (!progress) return Fail(OriginalSourceStatus::kDecode);
    }
}
OriginalSourceStatus OriginalSourceSession::Next(OriginalSourceFrame* output) {
    if (output) *output = {};
    if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
    if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
    if (!output) return Fail(OriginalSourceStatus::kInvalid);
    if (ended_) return OriginalSourceStatus::kEnd;
    if (vp9_[0].codec) return NextVp9(output);
    if (!codec_) return Fail(OriginalSourceStatus::kInvalid);
    av_frame_unref(frame_);
    for (;;) {
        if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
        if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
        int result = avcodec_receive_frame(codec_, frame_);
        if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
        if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
        if (result == 0) {
            if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
            if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
            if (++frames_ > expected_.frames || frame_->width != static_cast<int>(expected_.width) ||
                frame_->height != static_cast<int>(expected_.height) || frame_->pts == AV_NOPTS_VALUE)
                return Fail(OriginalSourceStatus::kMetadata);
            const bool rgba = frame_->format == AV_PIX_FMT_RGBA;
            if (!rgba && frame_->format != AV_PIX_FMT_YUV420P)
                return Fail(OriginalSourceStatus::kUnsupported);
            for (int i = 0; i < 4; ++i) {
                output->planes[i] = frame_->data[i];
                output->strides[i] = frame_->linesize[i];
            }
            output->width = frame_->width;
            output->height = frame_->height;
            output->rgba = rgba;
            output->pts = frame_->pts;
            output->time_base_num = format_->streams[stream_]->time_base.num;
            output->time_base_den = format_->streams[stream_]->time_base.den;
            return OriginalSourceStatus::kOk;
        }
        if (result == AVERROR_EOF)
            return Fail(frames_ == expected_.frames ? OriginalSourceStatus::kEnd
                                                   : OriginalSourceStatus::kMetadata);
        if (result != AVERROR(EAGAIN) || flushing_) return Fail(Error(result));
        do {
            av_packet_unref(packet_);
            result = av_read_frame(format_, packet_);
            if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
            if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
        } while (result >= 0 && packet_->stream_index != stream_);
        if (result == AVERROR_EOF) {
            flushing_ = true;
            result = avcodec_send_packet(codec_, nullptr);
        } else if (result >= 0) {
            result = avcodec_send_packet(codec_, packet_);
            av_packet_unref(packet_);
        }
        if (Cancelled()) return Fail(OriginalSourceStatus::kCancelled);
        if (AllocationFailed()) return Fail(OriginalSourceStatus::kNoMemory);
        if (result < 0) return Fail(Error(result));
    }
}
}
