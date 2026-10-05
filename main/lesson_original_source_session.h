#ifndef LESSON_ORIGINAL_SOURCE_SESSION_H
#define LESSON_ORIGINAL_SOURCE_SESSION_H
#include "lesson_asset_storage_coordinator.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
struct AVFormatContext;
struct AVIOContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
namespace tbot {
enum class OriginalSourceStatus { kOk, kEnd, kInvalid, kIntegrity, kIo, kNoMemory,
                                  kCancelled, kUnsupported, kDecode, kMetadata,
                                  kLeaseUnavailable };
enum class OriginalSourceCodec { kH264, kVp9Alpha, kPng };
struct OriginalSourceExpected {
    size_t bytes;
    uint8_t sha256[32];
    unsigned width, height, frames;
    OriginalSourceCodec codec;
};
struct OriginalSourceFrame {
    const uint8_t* planes[4]{};
    int strides[4]{};
    int64_t pts = 0;
    int time_base_num = 0, time_base_den = 0;
    unsigned width = 0, height = 0;
    bool rgba = false;
    bool alpha = false;
};
// One serialized owner. Only the external cancel flag may change concurrently.
// Frames borrow storage until Next/Close. Cancellation cannot interrupt a codec call.
// The allocator hook must notify every failure in the shared serialized owner.
// State outlives all sessions. A new session cannot clear an active owner's fault.
class OriginalSourceAllocationState {
public:
    OriginalSourceAllocationState() = default;
    OriginalSourceAllocationState(const OriginalSourceAllocationState&) = delete;
    OriginalSourceAllocationState& operator=(const OriginalSourceAllocationState&) = delete;
    void NotifyFailure() { failed_.store(true, std::memory_order_relaxed); }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }
private:
    friend class OriginalSourceSession;
    void Acquire() {
        if (owners_ == 0) failed_.store(false, std::memory_order_relaxed);
        ++owners_;
    }
    void Release() { --owners_; }
    std::atomic<bool> failed_{false};
    size_t owners_ = 0;
};
class OriginalSourceSession {
public:
    static constexpr size_t kMaxBytes = 4 * 1024 * 1024;
    OriginalSourceSession() = default;
    ~OriginalSourceSession();
    OriginalSourceSession(const OriginalSourceSession&) = delete;
    OriginalSourceSession& operator=(const OriginalSourceSession&) = delete;
    // The lesson read lease fences SD mutation while the file is snapshotted and
    // verified; it is released when Open returns. Decoding uses only the snapshot.
    OriginalSourceStatus Open(const char* path, const OriginalSourceExpected& expected,
                              LessonAssetReadLease lease,
                              OriginalSourceAllocationState* allocations,
                              const std::atomic<bool>* cancelled = nullptr);
    OriginalSourceStatus Next(OriginalSourceFrame* frame);
    void Close();
private:
    static int Read(void*, uint8_t*, int);
    static int64_t Seek(void*, int64_t, int);
    static int Interrupted(void*);
    bool Cancelled() const;
    bool AllocationFailed() const;
    OriginalSourceStatus Fail(OriginalSourceStatus);
    OriginalSourceStatus NextVp9(OriginalSourceFrame*);
    static constexpr size_t kQueueFrames = 4;
    static constexpr size_t kQueueBytes = 4 * 1024 * 1024;
    struct DecoderState {
        AVCodecContext* codec = nullptr;
        AVPacket* pending = nullptr;
        AVFrame* scratch = nullptr;
        AVFrame* output = nullptr;
        AVFrame* queue[kQueueFrames]{};
        size_t depth = 0;
        bool has_pending = false, drain_sent = false, eof = false;
        int64_t last_pts = 0;
        bool has_pts = false;
    } vp9_[2];
    size_t queued_bytes_ = 0;
    bool demux_eof_ = false, ended_ = false;
    OriginalSourceExpected expected_{};
    OriginalSourceAllocationState* allocations_ = nullptr;
    const std::atomic<bool>* cancelled_ = nullptr;
    uint8_t* bytes_ = nullptr;
    size_t position_ = 0;
    AVIOContext* io_ = nullptr;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_ = -1;
    unsigned frames_ = 0;
    bool flushing_ = false;
};
}
#endif
