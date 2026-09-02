#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

class BlufiStagedWifiCredentials {
public:
    struct Snapshot {
        uint64_t epoch;
        std::string ssid;
        std::string password;

        Snapshot(uint64_t candidate_epoch, const std::string& candidate_ssid,
                 const std::string& candidate_password)
            : epoch(candidate_epoch),
              ssid(candidate_ssid),
              password(candidate_password) {}

        ~Snapshot() {
            SecureClear(ssid);
            SecureClear(password);
        }

        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;

        Snapshot(Snapshot&& other) noexcept
            : epoch(other.epoch),
              ssid(other.ssid),
              password(other.password) {
            SecureClear(other.ssid);
            SecureClear(other.password);
        }

        Snapshot& operator=(Snapshot&& other) noexcept {
            if (this != &other) {
                SecureClear(ssid);
                SecureClear(password);
                epoch = other.epoch;
                ssid = other.ssid;
                password = other.password;
                SecureClear(other.ssid);
                SecureClear(other.password);
            }
            return *this;
        }
    };

    ~BlufiStagedWifiCredentials() {
        std::lock_guard<std::mutex> lock(mutex_);
        SecureClear(ssid_);
        SecureClear(password_);
    }

    uint64_t UpdateSsid(const std::string& ssid) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (CompleteLocked()) {
            SecureClear(password_);
            password_received_ = false;
        } else if (ssid_received_ && ssid_ == ssid) {
            return epoch_;
        }
        SecureClear(ssid_);
        ssid_ = ssid;
        ssid_received_ = true;
        claimed_ = false;
        return AdvanceEpochLocked();
    }

    uint64_t UpdatePassword(const std::string& password) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (CompleteLocked()) {
            SecureClear(ssid_);
            ssid_received_ = false;
        } else if (password_received_ && password_ == password) {
            return epoch_;
        }
        SecureClear(password_);
        password_ = password;
        password_received_ = true;
        claimed_ = false;
        return AdvanceEpochLocked();
    }

    uint64_t Invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        SecureClear(ssid_);
        SecureClear(password_);
        ssid_received_ = false;
        password_received_ = false;
        claimed_ = false;
        return AdvanceEpochLocked();
    }

    uint64_t FallbackEpoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return CompleteLocked() ? epoch_ : 0;
    }

    size_t StagedByteCountForTesting() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ssid_.size() + password_.size();
    }

    std::optional<Snapshot> Claim(uint64_t expected_epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected_epoch == 0 || expected_epoch != epoch_ || claimed_ ||
            !CompleteLocked()) {
            return std::nullopt;
        }
        return ClaimLocked();
    }

    std::optional<Snapshot> ClaimCurrent() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (claimed_ || !CompleteLocked()) {
            return std::nullopt;
        }
        return ClaimLocked();
    }

private:
    std::optional<Snapshot> ClaimLocked() {
        Snapshot snapshot{epoch_, ssid_, password_};
        claimed_ = true;
        SecureClear(ssid_);
        SecureClear(password_);
        ssid_received_ = false;
        password_received_ = false;
        return snapshot;
    }

    static void SecureClear(std::string& value) {
        if (!value.empty()) {
            volatile char* bytes = &value[0];
            for (size_t index = 0; index < value.size(); ++index) {
                bytes[index] = '\0';
            }
        }
        value.clear();
    }

    bool CompleteLocked() const { return ssid_received_ && password_received_; }

    uint64_t AdvanceEpochLocked() {
        ++epoch_;
        if (epoch_ == 0) {
            ++epoch_;
        }
        return epoch_;
    }

    mutable std::mutex mutex_;
    uint64_t epoch_ = 0;
    bool ssid_received_ = false;
    bool password_received_ = false;
    bool claimed_ = false;
    std::string ssid_;
    std::string password_;
};
