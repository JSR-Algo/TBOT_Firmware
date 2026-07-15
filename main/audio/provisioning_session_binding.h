#pragma once

#include <mutex>

#include "wake_word_lifecycle_controller.h"

class ProvisioningSessionBinding {
public:
    using Token = WakeWordLifecycleController::ProvisioningToken;

    void Bind(Token token) {
        std::lock_guard<std::mutex> lock(mutex_);
        token_ = token;
    }

    Token Capture() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return token_;
    }

    bool Matches(Token token) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return token_.generation == token.generation;
    }

    bool ClearIfMatches(Token token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token_.generation != token.generation) {
            return false;
        }
        token_ = {};
        return true;
    }

private:
    mutable std::mutex mutex_;
    Token token_{};
};
