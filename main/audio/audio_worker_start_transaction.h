#pragma once

#include <array>
#include <cstdint>
#include <utility>

class AudioWorkerStartTransaction {
public:
    enum class Worker : uint8_t {
        kOpusCodec,
        kAudioInput,
        kAudioOutput,
    };

    static constexpr uint32_t kIdleReclaimDelayMs = 10;
    static constexpr uint32_t kMaxRearmAttempts = 2;

    template <typename CreateWorker, typename Rollback>
    static bool StartOnce(CreateWorker&& create_worker,
                          Rollback&& rollback) {
        constexpr std::array<Worker, 3> kCreationOrder = {
            Worker::kOpusCodec,
            Worker::kAudioInput,
            Worker::kAudioOutput,
        };
        for (const Worker worker : kCreationOrder) {
            if (!create_worker(worker)) {
                rollback();
                return false;
            }
        }
        return true;
    }

    template <typename Delay, typename StartAttempt>
    static bool Rearm(Delay&& delay, StartAttempt&& start_attempt) {
        delay(kIdleReclaimDelayMs);
        for (uint32_t attempt = 1; attempt <= kMaxRearmAttempts; ++attempt) {
            if (start_attempt(attempt)) {
                return true;
            }
            if (attempt < kMaxRearmAttempts) {
                delay(kIdleReclaimDelayMs);
            }
        }
        return false;
    }
};
