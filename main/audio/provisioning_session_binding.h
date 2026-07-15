#pragma once

#include <mutex>

#include "wake_word_lifecycle_controller.h"

class ProvisioningSessionBinding {
public:
    using Token = WakeWordLifecycleController::ProvisioningToken;

    class CompletionGuard {
    public:
        CompletionGuard() = default;
        CompletionGuard(const CompletionGuard&) = delete;
        CompletionGuard& operator=(const CompletionGuard&) = delete;
        CompletionGuard(CompletionGuard&& other) noexcept { MoveFrom(other); }
        CompletionGuard& operator=(CompletionGuard&& other) noexcept {
            if (this != &other) {
                Release(false);
                MoveFrom(other);
            }
            return *this;
        }
        ~CompletionGuard() { Release(false); }

        explicit operator bool() const { return owner_ != nullptr; }

        bool ConsumeSuccess() {
            return Release(true);
        }

    private:
        friend class ProvisioningSessionBinding;
        CompletionGuard(ProvisioningSessionBinding* owner, Token token)
            : owner_(owner), token_(token) {}

        bool Release(bool consume) {
            if (owner_ == nullptr) {
                return false;
            }
            auto* owner = owner_;
            owner_ = nullptr;
            return owner->ReleaseCompletion(token_, consume);
        }

        void MoveFrom(CompletionGuard& other) {
            owner_ = other.owner_;
            token_ = other.token_;
            other.owner_ = nullptr;
        }

        ProvisioningSessionBinding* owner_ = nullptr;
        Token token_{};
    };

    class ReservationGuard {
    public:
        ReservationGuard() = default;
        ReservationGuard(const ReservationGuard&) = delete;
        ReservationGuard& operator=(const ReservationGuard&) = delete;
        ReservationGuard(ReservationGuard&& other) noexcept { MoveFrom(other); }
        ReservationGuard& operator=(ReservationGuard&& other) noexcept {
            if (this != &other) {
                Release();
                MoveFrom(other);
            }
            return *this;
        }
        ~ReservationGuard() { Release(); }

        explicit operator bool() const { return owner_ != nullptr; }

        bool Commit(Token token) {
            if (owner_ == nullptr || !token.valid()) {
                return false;
            }
            auto* owner = owner_;
            owner_ = nullptr;
            return owner->CommitReservation(token);
        }

    private:
        friend class ProvisioningSessionBinding;
        explicit ReservationGuard(ProvisioningSessionBinding* owner) : owner_(owner) {}

        void Release() {
            if (owner_ != nullptr) {
                owner_->ReleaseReservation();
                owner_ = nullptr;
            }
        }

        void MoveFrom(ReservationGuard& other) {
            owner_ = other.owner_;
            other.owner_ = nullptr;
        }

        ProvisioningSessionBinding* owner_ = nullptr;
    };

    bool Bind(Token token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completion_active_ || reservation_active_) {
            return false;
        }
        token_ = token;
        return true;
    }

    Token Capture() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return token_;
    }

    bool Matches(Token token) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return token_.generation == token.generation;
    }

    CompletionGuard Claim(Token token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completion_active_ || reservation_active_ ||
            token_.generation != token.generation) {
            return {};
        }
        completion_active_ = true;
        completion_token_ = token;
        return CompletionGuard(this, token);
    }

    ReservationGuard TryReserve() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completion_active_ || reservation_active_) {
            return {};
        }
        reservation_active_ = true;
        return ReservationGuard(this);
    }

private:
    bool CommitReservation(Token token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reservation_active_ || completion_active_ || !token.valid()) {
            return false;
        }
        token_ = token;
        reservation_active_ = false;
        return true;
    }

    void ReleaseReservation() {
        std::lock_guard<std::mutex> lock(mutex_);
        reservation_active_ = false;
    }

    bool ReleaseCompletion(Token token, bool consume) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!completion_active_ || completion_token_.generation != token.generation) {
            return false;
        }
        if (consume && token_.generation == token.generation) {
            token_ = {};
        }
        completion_active_ = false;
        completion_token_ = {};
        return true;
    }

    mutable std::mutex mutex_;
    Token token_{};
    bool reservation_active_ = false;
    bool completion_active_ = false;
    Token completion_token_{};
};
