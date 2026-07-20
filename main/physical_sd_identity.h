#ifndef TBOT_PHYSICAL_SD_IDENTITY_H
#define TBOT_PHYSICAL_SD_IDENTITY_H

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <sdmmc_cmd.h>

namespace tbot {

enum class PhysicalSdIdentityStatus {
    kUnavailable,
    kAvailable,
    kInvalid,
    kCardSwapped,
};

struct PhysicalSdCid {
    std::uint8_t manufacturer_id{};
    std::uint16_t oem_id{};
    std::string product_name;
    std::uint8_t revision{};
    std::uint32_t serial{};
    std::uint16_t manufacturing_date{};

    bool operator==(const PhysicalSdCid& other) const;
};

struct PhysicalSdIdentity {
    PhysicalSdCid cid;
    std::string cid_fingerprint;
    std::uint64_t capacity_sectors{};
    std::uint32_t sector_size_bytes{};
    std::uint64_t capacity_bytes{};
    std::uint64_t mount_generation{};
    std::string volume_serial;
    std::string volume_label;

    bool operator==(const PhysicalSdIdentity& other) const;
};

struct PhysicalSdIdentitySnapshot {
    PhysicalSdIdentityStatus status{PhysicalSdIdentityStatus::kUnavailable};
    std::optional<PhysicalSdIdentity> identity;
};

struct FatVolumeIdentity {
    std::uint32_t serial{};
    std::string label;
};

struct PhysicalSdLifecycleObservation {
    bool present{false};
    std::uint64_t mount_generation{0};
    std::optional<FatVolumeIdentity> volume;
};

std::optional<FatVolumeIdentity> ParseFatVolumeIdentity(
    const std::uint8_t* sector,
    std::size_t sector_bytes,
    std::uint32_t expected_sector_size
);

class PhysicalSdIdentityRegistry {
public:
    static PhysicalSdIdentityRegistry& GetInstance();
    bool ObserveMountedCard(
        const sdmmc_card_t* card,
        std::uint32_t volume_serial,
        std::string_view volume_label,
        std::uint64_t mount_generation = 1
    );
    void RecordUnavailable(std::uint64_t mount_generation = 0);
    PhysicalSdIdentitySnapshot Snapshot() const;
    PhysicalSdIdentitySnapshot RefreshAndSnapshot(
        PhysicalSdLifecycleObservation lifecycle
    );

private:
    PhysicalSdIdentitySnapshot SnapshotLocked() const;

    mutable std::mutex state_mutex_;
    PhysicalSdIdentityStatus status_{PhysicalSdIdentityStatus::kUnavailable};
    std::optional<PhysicalSdIdentity> identity_;
};

const char* PhysicalSdIdentityStatusName(PhysicalSdIdentityStatus status);

}  // namespace tbot

#endif
