#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

class BlockingWifiScanWorkerState {
public:
    struct Request {
        uint64_t ui_generation = 0;
        uint64_t revision = 0;
    };

    bool Publish(Request request) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.ui_generation <= cancelled_through_generation_ ||
            Matches(pending_, request) || Matches(in_flight_, request)) {
            return false;
        }
        pending_ = request;
        notification_delivered_ = false;
        return true;
    }

    bool NeedsWorkerCreation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.has_value() && !worker_created_;
    }

    void ObserveWorkerCreation(bool created) {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_created_ = worker_created_ || created;
    }

    bool NeedsNotification() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return worker_created_ && pending_.has_value() &&
            !notification_delivered_;
    }

    void ObserveNotification(bool delivered) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.has_value() && delivered) {
            notification_delivered_ = true;
        }
    }

    std::optional<Request> TakeNotified() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.has_value() || !notification_delivered_ ||
            in_flight_.has_value()) {
            return std::nullopt;
        }
        in_flight_ = pending_;
        pending_.reset();
        notification_delivered_ = false;
        return in_flight_;
    }

    bool CompleteIfCurrent(const Request& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(in_flight_, request)) {
            return false;
        }
        in_flight_.reset();
        return request.ui_generation > cancelled_through_generation_;
    }

    bool RetryInFlight(const Request& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(in_flight_, request) || pending_.has_value() ||
            request.ui_generation <= cancelled_through_generation_) {
            return false;
        }
        pending_ = request;
        in_flight_.reset();
        notification_delivered_ = false;
        return true;
    }

    void CancelGeneration(uint64_t ui_generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ui_generation > cancelled_through_generation_) {
            cancelled_through_generation_ = ui_generation;
        }
        if (pending_.has_value() &&
            pending_->ui_generation == ui_generation) {
            pending_.reset();
            notification_delivered_ = false;
        }
    }

private:
    static bool Matches(const std::optional<Request>& value,
                        const Request& request) {
        return value.has_value() &&
            value->ui_generation == request.ui_generation &&
            value->revision == request.revision;
    }

    mutable std::mutex mutex_;
    std::optional<Request> pending_;
    std::optional<Request> in_flight_;
    uint64_t cancelled_through_generation_ = 0;
    bool worker_created_ = false;
    bool notification_delivered_ = false;
};
