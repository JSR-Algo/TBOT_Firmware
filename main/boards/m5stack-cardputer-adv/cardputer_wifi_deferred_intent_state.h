#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

class CardputerWifiDeferredIntentState {
public:
    enum class Kind : uint8_t {
        kCredentials,
        kReconnect,
    };

    struct Intent {
        Kind kind = Kind::kReconnect;
        uint64_t ui_generation = 0;
        uint64_t revision = 0;
        std::string ssid;
        std::string password;
    };

    struct Result {
        uint64_t ui_generation = 0;
        uint64_t revision = 0;
        bool connected = false;
    };

    bool PublishCredentials(uint64_t ui_generation, std::string ssid,
                            std::string password) {
        return Publish(Intent{Kind::kCredentials, ui_generation, 0,
                              std::move(ssid), std::move(password)});
    }

    bool PublishReconnect(uint64_t ui_generation) {
        return Publish(Intent{Kind::kReconnect, ui_generation, 0, {}, {}});
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
            !notification_delivered_ && !in_flight_.has_value() &&
            !result_.has_value();
    }

    std::optional<uint64_t> ArmNotification() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_created_ || !pending_.has_value() ||
            notification_delivered_ || in_flight_.has_value() ||
            result_.has_value()) {
            return std::nullopt;
        }
        notification_delivered_ = true;
        return pending_->revision;
    }

    void RollbackNotification(uint64_t revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.has_value() && pending_->revision == revision) {
            notification_delivered_ = false;
        }
    }

    std::optional<Intent> TakeNotified() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.has_value() || !notification_delivered_ ||
            in_flight_.has_value() || result_.has_value()) {
            return std::nullopt;
        }
        in_flight_ = std::move(pending_);
        pending_.reset();
        notification_delivered_ = false;
        return in_flight_;
    }

    bool NeedsCredentialPersistence(const Intent& intent) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Matches(in_flight_, intent) && intent.kind == Kind::kCredentials &&
            persisted_credentials_revision_ != intent.revision;
    }

    bool MarkCredentialsPersisted(const Intent& intent) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(in_flight_, intent) || intent.kind != Kind::kCredentials) {
            return false;
        }
        persisted_credentials_revision_ = intent.revision;
        return true;
    }

    bool StoreConnectionResult(const Intent& intent, bool connected) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(in_flight_, intent) || intent.kind != Kind::kCredentials) {
            return false;
        }
        if (pending_.has_value() && pending_->revision > intent.revision) {
            in_flight_.reset();
            return true;
        }
        if (intent.ui_generation <= cancelled_through_generation_) {
            in_flight_.reset();
            return true;
        }
        result_ = Result{intent.ui_generation, intent.revision, connected};
        result_delivery_scheduled_ = false;
        in_flight_.reset();
        return true;
    }

    std::optional<Result> ClaimResultForDelivery() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!result_.has_value() || result_delivery_scheduled_) {
            return std::nullopt;
        }
        result_delivery_scheduled_ = true;
        return result_;
    }

    void ObserveResultDelivery(const Result& result, bool delivered) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Matches(result_, result) && !delivered) {
            result_delivery_scheduled_ = false;
        }
    }

    bool CompleteResult(const Result& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(result_, result)) {
            return false;
        }
        result_.reset();
        result_delivery_scheduled_ = false;
        return true;
    }

    bool CompleteReconnect(const Intent& intent) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(in_flight_, intent) || intent.kind != Kind::kReconnect) {
            return false;
        }
        in_flight_.reset();
        return true;
    }

    bool RetryInFlight(const Intent& intent) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Matches(in_flight_, intent)) {
            return false;
        }
        if (!pending_.has_value() &&
            intent.ui_generation > cancelled_through_generation_) {
            pending_ = std::move(in_flight_);
        }
        in_flight_.reset();
        notification_delivered_ = false;
        return pending_.has_value();
    }

    void CancelGeneration(uint64_t ui_generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_through_generation_ =
            std::max(cancelled_through_generation_, ui_generation);
        if (pending_.has_value() &&
            pending_->ui_generation == ui_generation) {
            pending_.reset();
            notification_delivered_ = false;
        }
    }

private:
    bool Publish(Intent intent) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (intent.ui_generation <= cancelled_through_generation_) {
            return false;
        }
        intent.revision = ++last_revision_;
        pending_ = std::move(intent);
        notification_delivered_ = false;
        return true;
    }

    static bool Matches(const std::optional<Intent>& value,
                        const Intent& intent) {
        return value.has_value() &&
            value->ui_generation == intent.ui_generation &&
            value->revision == intent.revision && value->kind == intent.kind;
    }

    static bool Matches(const std::optional<Result>& value,
                        const Result& result) {
        return value.has_value() &&
            value->ui_generation == result.ui_generation &&
            value->revision == result.revision;
    }

    mutable std::mutex mutex_;
    std::optional<Intent> pending_;
    std::optional<Intent> in_flight_;
    std::optional<Result> result_;
    uint64_t last_revision_ = 0;
    uint64_t cancelled_through_generation_ = 0;
    bool worker_created_ = false;
    bool notification_delivered_ = false;
    bool result_delivery_scheduled_ = false;
    uint64_t persisted_credentials_revision_ = 0;
};
