#ifndef WIFI_CONFIG_INTENT_STATE_H
#define WIFI_CONFIG_INTENT_STATE_H

#include <cstdint>
#include <mutex>

class WifiConfigIntentState {
public:
    static constexpr uint32_t kConditional = 1u << 0;
    static constexpr uint32_t kExplicit = 1u << 1;
    static constexpr uint32_t kNotify = 1u << 2;

    struct Value {
        uint32_t flags;
        uint32_t request_generation;
        uint32_t recovery_generation;
    };

    void Request(uint32_t flags, uint32_t recovery_generation = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        flags_ |= flags;
        if ((flags & kConditional) != 0) {
            recovery_generation_ = recovery_generation;
        }
        ++request_generation_;
    }

    Value Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {flags_, request_generation_, recovery_generation_};
    }

    bool ConsumeIfCurrent(uint32_t request_generation, uint32_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_generation_ != request_generation) {
            return false;
        }
        flags_ &= ~flags;
        return true;
    }

    void ConsumeAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        flags_ = 0;
    }

    uint32_t LoadFlags() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return flags_;
    }

private:
    mutable std::mutex mutex_;
    uint32_t flags_ = 0;
    uint32_t request_generation_ = 0;
    uint32_t recovery_generation_ = 0;
};

#endif  // WIFI_CONFIG_INTENT_STATE_H
