#pragma once

#include <cstdint>
#include <mutex>

class ProvisioningAudioWorkerState {
public:
    struct Completion {
        bool accepted = false;
        bool restart_required = false;
    };

    bool Bind(uint64_t generation, bool restart_required) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0 || active_generation_ != 0) {
            return false;
        }
        active_generation_ = generation;
        restart_required_ = restart_required;
        return true;
    }

    Completion Consume(uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0 || active_generation_ != generation) {
            return {};
        }
        const bool restart_required = restart_required_;
        active_generation_ = 0;
        restart_required_ = false;
        return {true, restart_required};
    }

private:
    std::mutex mutex_;
    uint64_t active_generation_ = 0;
    bool restart_required_ = false;
};
