#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

// Durable, allocation-free reservation polled by the process-lifetime WiFi
// manager worker. A failed timer/queue/scheduler signal never clears the tuple.
class BlufiWifiScanRetryState {
public:
    struct ExactRequest {
        uint64_t request_id = 0;
        uint32_t setup_generation = 0;
        uint64_t ble_session_state = 0;
        uint64_t ble_connection_epoch = 0;
        bool save_results = false;
        bool send_list = false;
        uint64_t revision = 0;
        bool dispatch_enqueuing = false;
        bool dispatch_scheduled = false;
    };

    bool Publish(ExactRequest exact) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.has_value() &&
                active_->request_id > exact.request_id) {
                return false;
            }
            PublishLocked(exact);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool RepublishIfUnchanged(ExactRequest exact) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if ((active_.has_value() &&
                 active_->revision != exact.revision) ||
                (!active_.has_value() && last_revision_ != exact.revision)) {
                return false;
            }
            PublishLocked(exact);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::optional<ExactRequest> Snapshot() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return active_;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<ExactRequest> BeginDispatch() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_.has_value() || active_->dispatch_enqueuing ||
                active_->dispatch_scheduled) {
                return std::nullopt;
            }
            active_->dispatch_enqueuing = true;
            return active_;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool CompleteDispatch(const ExactRequest& exact, bool scheduled) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_.has_value() || exact.revision == 0 ||
                active_->revision != exact.revision ||
                !active_->dispatch_enqueuing) {
                return false;
            }
            active_->dispatch_enqueuing = false;
            active_->dispatch_scheduled = scheduled;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool IsCurrentRevision(const ExactRequest& exact) const noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return active_.has_value() && exact.revision != 0 &&
                active_->revision == exact.revision;
        } catch (...) {
            return false;
        }
    }

    bool ClearIfExact(const ExactRequest& exact) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_.has_value() || exact.revision == 0 ||
                active_->revision != exact.revision) {
                return false;
            }
            active_.reset();
            return true;
        } catch (...) {
            return false;
        }
    }

    struct SignalResult {
        bool esp_timer = false;
        bool freertos_timer = false;
        bool application = false;
        bool manager = false;
    };

    template <typename EspTimer, typename FreeRtosTimer,
              typename ApplicationSchedule, typename ManagerNotify>
    SignalResult SignalPublished(EspTimer&& esp_timer,
                                 FreeRtosTimer&& freertos_timer,
                                 ApplicationSchedule&& application,
                                 ManagerNotify&& manager) noexcept {
        SignalResult result;
        result.esp_timer = Invoke(esp_timer);
        if (!result.esp_timer) {
            result.freertos_timer = Invoke(freertos_timer);
        }
        if (!result.esp_timer && !result.freertos_timer) {
            result.application = Invoke(application);
        }
        // Always notify the bounded polling worker, even when an accelerator
        // succeeded; exact revision claiming coalesces duplicate wakeups.
        result.manager = Invoke(manager);
        return result;
    }

private:
    void PublishLocked(ExactRequest exact) noexcept {
        ++last_revision_;
        if (last_revision_ == 0) {
            ++last_revision_;
        }
        exact.revision = last_revision_;
        exact.dispatch_enqueuing = false;
        exact.dispatch_scheduled = false;
        active_ = exact;
    }

    template <typename Signal>
    static bool Invoke(Signal& signal) noexcept {
        try {
            return signal();
        } catch (...) {
            return false;
        }
    }

    mutable std::mutex mutex_;
    uint64_t last_revision_ = 0;
    std::optional<ExactRequest> active_;
};
