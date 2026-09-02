#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

class TbotBlufiAdvertisingLedger {
public:
    // Submission callbacks must be enqueue-only and must not synchronously
    // re-enter GAP completion handling. They run with the submission-order gate
    // held but never with the ledger state mutex held. Bluedroid GAP APIs satisfy
    // the no-reentry contract by posting to BTC and completing in a later event.
    enum class CallbackKind : uint8_t {
        kCompactAdvData,
        kCompactScanResponse,
        kDefaultAdvData,
        kCompactStart,
        kDefaultStart,
    };

    struct Owner {
        uint32_t incarnation = 0;
        uint32_t epoch = 0;
        uint64_t serial = 0;
        CallbackKind kind = CallbackKind::kCompactAdvData;
    };

    struct CompactSubmission {
        Owner adv_data;
        Owner scan_response;
        uint32_t epoch = 0;
    };

    struct ConfigResult {
        bool owned = false;
        bool start_compact = false;
        bool fallback_started = false;
        Owner start_owner;
    };

    struct DefaultConfigResult {
        bool owned = false;
        bool start_submitted = false;
        Owner owner;
        Owner start_owner;
    };

    struct StartResult {
        bool owned = false;
        bool compact_completed = false;
        bool fallback_started = false;
        bool default_failed = false;
        Owner owner;
    };

    std::optional<CompactSubmission> BeginCompact(uint8_t pending_bits) {
        std::lock_guard<std::mutex> lock(mutex_);
        return BeginCompactLocked(pending_bits);
    }

    template <typename SubmitAdv, typename SubmitScan, typename SubmitDefault>
    std::optional<CompactSubmission> BeginCompactAndSubmit(
            uint8_t pending_bits, SubmitAdv&& submit_adv, SubmitScan&& submit_scan,
            SubmitDefault&& submit_default) {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        auto submission = BeginCompact(pending_bits);
        if (!submission) {
            return std::nullopt;
        }
        if (!submit_adv()) {
            Cancel(submission->adv_data);
            if ((pending_bits & kScanResponsePending) != 0) {
                Cancel(submission->scan_response);
            }
            auto fallback = ClaimDefaultFallback();
            if (fallback) {
                submit_default();
            }
            return submission;
        }
        if ((pending_bits & kScanResponsePending) != 0 && !submit_scan()) {
            Cancel(submission->scan_response);
            auto fallback = ClaimDefaultFallback();
            if (fallback) {
                submit_default();
            }
        }
        return submission;
    }

    std::optional<Owner> ClaimDefaultFallback() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ClaimDefaultFallbackLocked();
    }

    template <typename SubmitDefault>
    std::optional<Owner> ClaimDefaultFallbackAndSubmit(SubmitDefault&& submit_default) {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        auto owner = ClaimDefaultFallback();
        if (owner) {
            submit_default();
        }
        return owner;
    }

    ConfigResult CompleteCompactConfig(CallbackKind kind, bool callback_present) {
        std::lock_guard<std::mutex> lock(mutex_);
        return CompleteCompactConfigLocked(kind, callback_present, true);
    }

    template <typename SubmitStart, typename SubmitDefault>
    ConfigResult CompleteCompactConfigAndSubmit(
            CallbackKind kind, bool callback_present, bool success,
            SubmitStart&& submit_start, SubmitDefault&& submit_default) {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        ConfigResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result = CompleteCompactConfigLocked(kind, callback_present, success);
        }
        if (!result.owned) {
            return result;
        }
        if (!success) {
            auto fallback = ClaimDefaultFallback();
            result.fallback_started = fallback.has_value();
            if (fallback) {
                submit_default();
            }
            return result;
        }
        if (result.start_compact && !submit_start()) {
            Cancel(result.start_owner);
            result.start_compact = false;
            auto fallback = ClaimDefaultFallback();
            result.fallback_started = fallback.has_value();
            if (fallback) {
                submit_default();
            }
        }
        return result;
    }

    DefaultConfigResult CompleteDefaultConfig(bool callback_present) {
        std::lock_guard<std::mutex> lock(mutex_);
        return CompleteDefaultConfigLocked(callback_present, true);
    }

    template <typename SubmitStart>
    DefaultConfigResult CompleteDefaultConfigAndSubmit(
            bool callback_present, bool success, SubmitStart&& submit_start) {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        DefaultConfigResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result = CompleteDefaultConfigLocked(callback_present, success);
        }
        if (result.owned && success) {
            if (submit_start()) {
                result.start_submitted = true;
            } else {
                Cancel(result.start_owner);
            }
        }
        return result;
    }

    StartResult CompleteStart(bool callback_present) {
        std::lock_guard<std::mutex> lock(mutex_);
        return CompleteStartLocked(callback_present, true);
    }

    template <typename SubmitDefault>
    StartResult CompleteStartAndMaybeFallback(
            bool callback_present, bool success, SubmitDefault&& submit_default) {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        StartResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result = CompleteStartLocked(callback_present, success);
        }
        if (result.owned && result.owner.kind == CallbackKind::kCompactStart && !success) {
            auto fallback = ClaimDefaultFallback();
            result.fallback_started = fallback.has_value();
            if (fallback) {
                submit_default();
            }
        }
        return result;
    }

    void Invalidate() {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_submissions_ = false;
        compact_active_ = false;
        active_epoch_ = 0;
        compact_pending_bits_ = 0;
    }

    void ResetAfterSuccessfulHostDeinit() {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_submissions_ = false;
        compact_active_ = false;
        active_epoch_ = 0;
        compact_pending_bits_ = 0;
        adv_data_.clear();
        scan_response_.clear();
        default_adv_data_.clear();
        starts_.clear();
        ++host_incarnation_;
    }

    bool ActivateAfterSuccessfulHostInit() {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!adv_data_.empty() || !scan_response_.empty() ||
            !default_adv_data_.empty() || !starts_.empty()) {
            return false;
        }
        accepting_submissions_ = true;
        return true;
    }

    bool Cancel(const Owner& owner) {
        std::lock_guard<std::mutex> lock(mutex_);
        return CancelLocked(owner);
    }

    size_t Pending(CallbackKind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return QueueForKindLocked(kind).size();
    }

    uint32_t ActiveEpoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_epoch_;
    }

    uint32_t HostIncarnation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return host_incarnation_;
    }

    static constexpr uint8_t kAdvDataPending = 1U << 0;
    static constexpr uint8_t kScanResponsePending = 1U << 1;

private:
    DefaultConfigResult CompleteDefaultConfigLocked(
            bool callback_present, bool success) {
        DefaultConfigResult result;
        if (!callback_present || default_adv_data_.empty()) {
            return result;
        }
        result.owner = default_adv_data_.front();
        default_adv_data_.pop_front();
        if (!IsCurrentLocked(result.owner)) {
            return result;
        }
        result.owned = true;
        if (success) {
            result.start_owner = PushLocked(
                CallbackKind::kDefaultStart, result.owner.epoch);
        }
        return result;
    }
    std::optional<CompactSubmission> BeginCompactLocked(uint8_t pending_bits) {
        if (!accepting_submissions_ || !adv_data_.empty() || !scan_response_.empty() ||
            !default_adv_data_.empty() || !starts_.empty()) {
            return std::nullopt;
        }
        active_epoch_ = ++last_epoch_;
        compact_active_ = true;
        compact_pending_bits_ = pending_bits;
        CompactSubmission result;
        result.epoch = active_epoch_;
        result.adv_data = PushLocked(CallbackKind::kCompactAdvData, active_epoch_);
        if ((pending_bits & kScanResponsePending) != 0) {
            result.scan_response = PushLocked(
                CallbackKind::kCompactScanResponse, active_epoch_);
        }
        return result;
    }

    std::optional<Owner> ClaimDefaultFallbackLocked() {
        if (!compact_active_ || active_epoch_ == 0 || !default_adv_data_.empty()) {
            return std::nullopt;
        }
        compact_active_ = false;
        compact_pending_bits_ = 0;
        return PushLocked(CallbackKind::kDefaultAdvData, active_epoch_);
    }

    ConfigResult CompleteCompactConfigLocked(
            CallbackKind kind, bool callback_present, bool success) {
        ConfigResult result;
        auto& queue = QueueForKindLocked(kind);
        if (!callback_present || queue.empty()) {
            return result;
        }
        const Owner owner = queue.front();
        queue.pop_front();
        if (!IsCurrentLocked(owner) || !compact_active_) {
            return result;
        }
        result.owned = true;
        if (!success) {
            return result;
        }
        const uint8_t bit = kind == CallbackKind::kCompactAdvData
            ? kAdvDataPending : kScanResponsePending;
        if ((compact_pending_bits_ & bit) == 0) {
            return result;
        }
        compact_pending_bits_ &= static_cast<uint8_t>(~bit);
        if (compact_pending_bits_ == 0) {
            result.start_owner = PushLocked(CallbackKind::kCompactStart, owner.epoch);
            result.start_compact = true;
        }
        return result;
    }

    StartResult CompleteStartLocked(bool callback_present, bool success) {
        StartResult result;
        if (!callback_present || starts_.empty()) {
            return result;
        }
        result.owner = starts_.front();
        starts_.pop_front();
        if (!IsCurrentLocked(result.owner)) {
            return result;
        }
        result.owned = true;
        if (result.owner.kind == CallbackKind::kDefaultStart && !success) {
            result.default_failed = true;
        }
        if (result.owner.kind == CallbackKind::kCompactStart && compact_active_) {
            if (success) {
                compact_active_ = false;
                result.compact_completed = true;
            }
        }
        return result;
    }

    Owner PushLocked(CallbackKind kind, uint32_t epoch) {
        Owner owner{host_incarnation_, epoch, ++last_serial_, kind};
        QueueForKindLocked(kind).push_back(owner);
        return owner;
    }

    bool CancelLocked(const Owner& owner) {
        auto& queue = QueueForKindLocked(owner.kind);
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->serial == owner.serial && it->incarnation == owner.incarnation) {
                queue.erase(it);
                return true;
            }
        }
        return false;
    }

    bool IsCurrentLocked(const Owner& owner) const {
        return owner.incarnation == host_incarnation_ &&
               owner.epoch != 0 && owner.epoch == active_epoch_;
    }

    std::deque<Owner>& QueueForKindLocked(CallbackKind kind) {
        switch (kind) {
            case CallbackKind::kCompactAdvData: return adv_data_;
            case CallbackKind::kCompactScanResponse: return scan_response_;
            case CallbackKind::kDefaultAdvData: return default_adv_data_;
            case CallbackKind::kCompactStart:
            case CallbackKind::kDefaultStart: return starts_;
        }
        return starts_;
    }

    const std::deque<Owner>& QueueForKindLocked(CallbackKind kind) const {
        return const_cast<TbotBlufiAdvertisingLedger*>(this)->QueueForKindLocked(kind);
    }

    mutable std::mutex mutex_;
    mutable std::mutex submission_mutex_;
    uint32_t host_incarnation_ = 1;
    uint32_t last_epoch_ = 0;
    uint32_t active_epoch_ = 0;
    uint64_t last_serial_ = 0;
    uint8_t compact_pending_bits_ = 0;
    bool compact_active_ = false;
    bool accepting_submissions_ = false;
    std::deque<Owner> adv_data_;
    std::deque<Owner> scan_response_;
    std::deque<Owner> default_adv_data_;
    std::deque<Owner> starts_;
};
