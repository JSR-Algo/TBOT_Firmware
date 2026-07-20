#ifndef TBOT_SD_FAT_SESSION_GUARD_H
#define TBOT_SD_FAT_SESSION_GUARD_H

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace tbot {

struct SdFatVolumeMetadata {
    std::uint32_t serial{0};
    std::string label;
};

struct SdFatLifecycleSnapshot {
    bool present{false};
    std::uint64_t mount_generation{0};
    std::optional<SdFatVolumeMetadata> volume;
};

class SdFatSessionGuard;

class SdFatSessionLease {
public:
    SdFatSessionLease() = default;
    SdFatSessionLease(SdFatSessionLease&& other) noexcept;
    SdFatSessionLease& operator=(SdFatSessionLease&& other) noexcept;
    ~SdFatSessionLease();
    explicit operator bool() const;

    SdFatSessionLease(const SdFatSessionLease&) = delete;
    SdFatSessionLease& operator=(const SdFatSessionLease&) = delete;

private:
    friend class SdFatSessionGuard;
    SdFatSessionLease(SdFatSessionGuard* guard, std::uint64_t reservation_token);
    void Release();

    SdFatSessionGuard* guard_{nullptr};
    std::uint64_t reservation_token_{0};
};

class SdFatSessionGuard {
public:
    static SdFatSessionGuard& GetInstance();

    SdFatSessionLease Acquire();
    SdFatSessionLease TryAcquire();
    std::uint64_t RecordMounted(const SdFatSessionLease& lease);
    void RecordUnmounted(const SdFatSessionLease& lease);
    bool SetPresenceProbe(
        const SdFatSessionLease& lease,
        std::function<bool()> probe
    );
    bool SetVolumeProbe(
        const SdFatSessionLease& lease,
        std::function<std::optional<SdFatVolumeMetadata>()> probe
    );
    SdFatLifecycleSnapshot Snapshot(const SdFatSessionLease& lease);

private:
    friend class SdFatSessionLease;
    std::uint64_t AllocateTokenLocked();
    bool OwnsLocked(const SdFatSessionLease& lease) const;
    void Release(std::uint64_t token);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool reservation_active_{false};
    std::uint64_t active_reservation_token_{0};
    std::uint64_t next_reservation_token_{0};
    bool present_{false};
    std::uint64_t mount_generation_{0};
    std::function<bool()> presence_probe_;
    std::function<std::optional<SdFatVolumeMetadata>()> volume_probe_;
};

}  // namespace tbot

#endif
