#pragma once

#include "audio/audio_output_drain.h"
#include <atomic>
#include <mutex>

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t) &&
              alignof(std::atomic<uint32_t>) >= alignof(uint32_t),
              "The I2S ISR counter requires aligned 32-bit storage");

template <typename Disable, typename Unregister, typename Delete, typename Release>
bool Es8311ReleaseIsrContext(Disable disable, Unregister unregister, Delete remove, Release release) {
    if (!disable() || !unregister() || !remove()) return false;
    release();
    return true;
}

// Only the EOF counter is touched by the ISR. Publication locks are never held
// across driver calls; readers try once and conservatively report Busy.
class Es8311HardwareDrain {
public:
    Es8311HardwareDrain(std::atomic<uint32_t>& eof, uint32_t descriptors)
        : eof_(eof), required_eofs_(descriptors + 1) {}

    AudioOutputDrainSnapshot Snapshot() const {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        const auto epoch = epoch_.load(std::memory_order_relaxed);
        if (!lock.owns_lock()) return {epoch, AudioOutputDrainState::Busy};
        if (failed_) return {epoch, AudioOutputDrainState::Failed};
        if (writing_) return {epoch, AudioOutputDrainState::Busy};
        // One complete DMA ring plus a descriptor accounts for callback/clear
        // ordering. This is a digital DMA fence, not an analog audibility claim.
        const bool drained = submitted_ &&
            uint32_t(eof_.load(std::memory_order_relaxed) - baseline_) >= required_eofs_;
        return {epoch, drained ? AudioOutputDrainState::Drained : AudioOutputDrainState::Pending};
    }
    void BeginWrite() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++writing_;
        submitted_ = false;
    }
    void FinishWrite(bool success) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ |= !success;
        baseline_ = eof_.load(std::memory_order_relaxed);
        submitted_ = success;
        --writing_;
    }
    void Fail() {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
    }
    void Invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch_.fetch_add(1, std::memory_order_relaxed);
        submitted_ = false;
    }
    bool Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (writing_) {
            failed_ = true;
            return false;
        }
        epoch_.fetch_add(1, std::memory_order_relaxed);
        failed_ = false;
        submitted_ = false;
        return true;
    }
private:
    std::atomic<uint32_t>& eof_;
    const uint32_t required_eofs_;
    mutable std::mutex mutex_;
    std::atomic<uint32_t> epoch_{1};
    uint32_t baseline_ = 0;
    uint32_t writing_ = 0;
    bool submitted_ = false;
    bool failed_ = false;
};
