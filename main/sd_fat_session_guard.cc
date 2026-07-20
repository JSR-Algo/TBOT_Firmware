#include "sd_fat_session_guard.h"

#include <limits>
#include <utility>

namespace tbot {

SdFatSessionLease::SdFatSessionLease(
    SdFatSessionGuard* guard,
    std::uint64_t reservation_token
) : guard_(guard), reservation_token_(reservation_token) {}

SdFatSessionLease::SdFatSessionLease(SdFatSessionLease&& other) noexcept
    : guard_(other.guard_), reservation_token_(other.reservation_token_) {
    other.guard_ = nullptr;
    other.reservation_token_ = 0;
}

SdFatSessionLease& SdFatSessionLease::operator=(
    SdFatSessionLease&& other
) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    guard_ = other.guard_;
    reservation_token_ = other.reservation_token_;
    other.guard_ = nullptr;
    other.reservation_token_ = 0;
    return *this;
}

SdFatSessionLease::~SdFatSessionLease() {
    Release();
}

SdFatSessionLease::operator bool() const {
    return guard_ != nullptr && reservation_token_ != 0;
}

void SdFatSessionLease::Release() {
    if (guard_ != nullptr && reservation_token_ != 0) {
        guard_->Release(reservation_token_);
    }
    guard_ = nullptr;
    reservation_token_ = 0;
}

SdFatSessionGuard& SdFatSessionGuard::GetInstance() {
    // Leases may be retained by other process-lifetime singletons. Keep the
    // guard alive through teardown so their destructors can release safely.
    static auto* guard = new SdFatSessionGuard();
    return *guard;
}

SdFatSessionLease SdFatSessionGuard::Acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return !reservation_active_; });
    const auto token = AllocateTokenLocked();
    return SdFatSessionLease(this, token);
}

SdFatSessionLease SdFatSessionGuard::TryAcquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reservation_active_) {
        return {};
    }
    return SdFatSessionLease(this, AllocateTokenLocked());
}

std::uint64_t SdFatSessionGuard::RecordMounted(
    const SdFatSessionLease& lease
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!OwnsLocked(lease) ||
        mount_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return 0;
    }
    ++mount_generation_;
    present_ = true;
    presence_probe_ = {};
    volume_probe_ = {};
    return mount_generation_;
}

void SdFatSessionGuard::RecordUnmounted(const SdFatSessionLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (OwnsLocked(lease)) {
        present_ = false;
        presence_probe_ = {};
        volume_probe_ = {};
    }
}

bool SdFatSessionGuard::SetPresenceProbe(
    const SdFatSessionLease& lease,
    std::function<bool()> probe
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!OwnsLocked(lease) || !probe) {
        return false;
    }
    presence_probe_ = std::move(probe);
    return true;
}

bool SdFatSessionGuard::SetVolumeProbe(
    const SdFatSessionLease& lease,
    std::function<std::optional<SdFatVolumeMetadata>()> probe
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!OwnsLocked(lease) || !probe) {
        return false;
    }
    volume_probe_ = std::move(probe);
    return true;
}

SdFatLifecycleSnapshot SdFatSessionGuard::Snapshot(
    const SdFatSessionLease& lease
) {
    bool present = false;
    std::uint64_t mount_generation = 0;
    std::function<bool()> presence_probe;
    std::function<std::optional<SdFatVolumeMetadata>()> volume_probe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!OwnsLocked(lease)) {
            return {};
        }
        present = present_;
        mount_generation = mount_generation_;
        presence_probe = presence_probe_;
        volume_probe = volume_probe_;
    }

    if (presence_probe) {
        const bool probed_present = presence_probe();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!OwnsLocked(lease)) {
            return {};
        }
        present_ = probed_present;
        present = probed_present;
    }
    if (!present) {
        return {false, mount_generation, std::nullopt};
    }
    std::optional<SdFatVolumeMetadata> volume;
    if (volume_probe) {
        volume = volume_probe();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!OwnsLocked(lease) || mount_generation_ != mount_generation) {
            return {};
        }
        present = present_;
    }
    return {present, mount_generation, present ? std::move(volume) : std::nullopt};
}

std::uint64_t SdFatSessionGuard::AllocateTokenLocked() {
    ++next_reservation_token_;
    if (next_reservation_token_ == 0) {
        ++next_reservation_token_;
    }
    reservation_active_ = true;
    active_reservation_token_ = next_reservation_token_;
    return active_reservation_token_;
}

bool SdFatSessionGuard::OwnsLocked(const SdFatSessionLease& lease) const {
    return reservation_active_ && lease.guard_ == this &&
        lease.reservation_token_ == active_reservation_token_;
}

void SdFatSessionGuard::Release(std::uint64_t token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reservation_active_ || token != active_reservation_token_) {
            return;
        }
        reservation_active_ = false;
        active_reservation_token_ = 0;
    }
    condition_.notify_one();
}

}  // namespace tbot
