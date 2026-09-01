#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include <wifi_scan_lease_coordinator.h>

class BlockingWifiScanRetryState {
public:
    struct Token {
        WifiScanLeaseCoordinator::Lease recovered_lease;
        uint64_t ui_generation = 0;
    };

    void Publish(Token token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token.ui_generation <= cancelled_through_generation_) {
            return;
        }
        token_ = token;
    }

    void PublishIfAbsent(Token token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token.ui_generation > cancelled_through_generation_ &&
            !token_.has_value()) {
            token_ = token;
        }
    }

    std::optional<Token> Peek(uint64_t ui_generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!token_.has_value() || token_->ui_generation != ui_generation) {
            return std::nullopt;
        }
        return token_;
    }

    bool ConsumeIfExact(const Token& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(token)) {
            return false;
        }
        token_.reset();
        return true;
    }

    void CancelGeneration(uint64_t ui_generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ui_generation > cancelled_through_generation_) {
            cancelled_through_generation_ = ui_generation;
        }
        if (token_.has_value() && token_->ui_generation == ui_generation) {
            token_.reset();
        }
    }

private:
    bool Matches(const Token& token) const {
        return token_.has_value() &&
            token_->ui_generation == token.ui_generation &&
            token_->recovered_lease.owner == token.recovered_lease.owner &&
            token_->recovered_lease.lease_id ==
                token.recovered_lease.lease_id &&
            token_->recovered_lease.driver_incarnation ==
                token.recovered_lease.driver_incarnation;
    }

    mutable std::mutex mutex_;
    std::optional<Token> token_;
    uint64_t cancelled_through_generation_ = 0;
};
