#pragma once

#include <atomic>
#include <cstdint>

// One writer, serialized by the audio queue mutex. Readers never take a lock.
// Only 32-bit atomic loads/stores are used (64-bit atomics may lock on Xtensa).
class AudioResetEpochPublication {
public:
    // Mark busy BEFORE changing the queue-owned fence epoch.
    void BeginReset() {
        const uint32_t sequence = sequence_.load(std::memory_order_seq_cst);
        if (sequence != UINT32_MAX) {
            sequence_.store(sequence + 1, std::memory_order_seq_cst);
        }
    }

    void Publish(uint64_t epoch) {
        const uint32_t sequence = sequence_.load(std::memory_order_seq_cst);
        // Reset 2^31 saturates the sequence: reads remain Busy permanently,
        // rather than permitting sequence-wrap ABA.
        if (sequence == UINT32_MAX) {
            return;
        }
        low_.store(static_cast<uint32_t>(epoch), std::memory_order_seq_cst);
        high_.store(static_cast<uint32_t>(epoch >> 32), std::memory_order_seq_cst);
        sequence_.store(sequence + 1, std::memory_order_seq_cst);
    }

    // A single attempt; failed capture leaves out untouched. The shared SC
    // order prevents either half from crossing the checked sequence interval.
    bool TryRead(uint64_t& out) const {
        const uint32_t before = sequence_.load(std::memory_order_seq_cst);
        if (before & 1U) {
            return false;
        }
        const uint32_t low = low_.load(std::memory_order_seq_cst);
        const uint32_t high = high_.load(std::memory_order_seq_cst);
        const uint32_t after = sequence_.load(std::memory_order_seq_cst);
        if (before != after) {
            return false;
        }
        out = (static_cast<uint64_t>(high) << 32) | low;
        return true;
    }

private:
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint32_t> low_{0};
    std::atomic<uint32_t> high_{0};
};
