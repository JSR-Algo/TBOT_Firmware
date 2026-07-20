#include "physical_sd_identity.h"

#include <cctype>
#include <cstdio>
#include <limits>

namespace tbot {
namespace {

std::uint16_t ReadLe16(const std::uint8_t* value) {
    return static_cast<std::uint16_t>(value[0]) |
           (static_cast<std::uint16_t>(value[1]) << 8);
}

std::uint32_t ReadLe32(const std::uint8_t* value) {
    return static_cast<std::uint32_t>(value[0]) |
           (static_cast<std::uint32_t>(value[1]) << 8) |
           (static_cast<std::uint32_t>(value[2]) << 16) |
           (static_cast<std::uint32_t>(value[3]) << 24);
}

bool IsPrintableAscii(std::string_view value) {
    for (const unsigned char character : value) {
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
    }
    return true;
}

bool DecodeProductName(const char (&raw)[8], std::string* output) {
    constexpr std::size_t kSdProductNameBytes = 5;
    for (std::size_t index = 0; index < kSdProductNameBytes; ++index) {
        const auto character = static_cast<unsigned char>(raw[index]);
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
    }
    for (std::size_t index = kSdProductNameBytes; index < sizeof(raw); ++index) {
        if (raw[index] != '\0') {
            return false;
        }
    }
    *output = std::string(raw, kSdProductNameBytes);
    return true;
}

std::string Hex(std::uint64_t value, int width) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%0*llx", width,
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string HexProductName(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (const unsigned char character : value) {
        encoded += Hex(character, 2);
    }
    return encoded;
}

std::optional<PhysicalSdIdentity> BuildIdentity(
    const sdmmc_card_t* card,
    std::uint32_t volume_serial,
    std::string_view volume_label,
    std::uint64_t mount_generation
) {
    if (card == nullptr || volume_serial == 0 || mount_generation == 0 ||
        volume_label.size() > 11 ||
        !IsPrintableAscii(volume_label)) {
        return std::nullopt;
    }
    if (card->cid.mfg_id <= 0 || card->cid.mfg_id > 0xff ||
        card->cid.oem_id <= 0 || card->cid.oem_id > 0xffff ||
        card->cid.revision < 0 || card->cid.revision > 0xff ||
        card->cid.serial == 0 || card->cid.date <= 0 || card->cid.date > 0xfff ||
        card->csd.capacity <= 0 || card->csd.sector_size <= 0) {
        return std::nullopt;
    }

    std::string product_name;
    if (!DecodeProductName(card->cid.name, &product_name)) {
        return std::nullopt;
    }

    const auto capacity_sectors = static_cast<std::uint64_t>(card->csd.capacity);
    const auto sector_size = static_cast<std::uint64_t>(card->csd.sector_size);
    if (capacity_sectors > std::numeric_limits<std::uint64_t>::max() / sector_size ||
        sector_size > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }

    PhysicalSdIdentity result;
    result.cid.manufacturer_id = static_cast<std::uint8_t>(card->cid.mfg_id);
    result.cid.oem_id = static_cast<std::uint16_t>(card->cid.oem_id);
    result.cid.product_name = product_name;
    result.cid.revision = static_cast<std::uint8_t>(card->cid.revision);
    result.cid.serial = static_cast<std::uint32_t>(card->cid.serial);
    result.cid.manufacturing_date = static_cast<std::uint16_t>(card->cid.date);
    result.cid_fingerprint =
        Hex(result.cid.manufacturer_id, 2) + "-" +
        Hex(result.cid.oem_id, 4) + "-" + HexProductName(product_name) + "-" +
        Hex(result.cid.revision, 2) + "-" + Hex(result.cid.serial, 8) + "-" +
        Hex(result.cid.manufacturing_date, 3);
    result.capacity_sectors = capacity_sectors;
    result.sector_size_bytes = static_cast<std::uint32_t>(sector_size);
    result.capacity_bytes = capacity_sectors * sector_size;
    result.mount_generation = mount_generation;
    result.volume_serial = Hex(volume_serial, 8);
    result.volume_label = std::string(volume_label);
    return result;
}

bool HasSameStableIdentity(
    const PhysicalSdIdentity& left,
    const PhysicalSdIdentity& right
) {
    return left.cid == right.cid &&
           left.cid_fingerprint == right.cid_fingerprint &&
           left.capacity_sectors == right.capacity_sectors &&
           left.sector_size_bytes == right.sector_size_bytes &&
           left.capacity_bytes == right.capacity_bytes &&
           left.mount_generation == right.mount_generation &&
           left.volume_serial == right.volume_serial;
}

}  // namespace

std::optional<FatVolumeIdentity> ParseFatVolumeIdentity(
    const std::uint8_t* sector,
    std::size_t sector_bytes,
    std::uint32_t expected_sector_size
) {
    if (sector == nullptr || sector_bytes < 512 || expected_sector_size < 512 ||
        sector_bytes < expected_sector_size || sector[510] != 0x55 || sector[511] != 0xaa ||
        ReadLe16(sector + 11) != expected_sector_size) {
        return std::nullopt;
    }
    const std::uint8_t sectors_per_cluster = sector[13];
    const std::uint16_t reserved_sectors = ReadLe16(sector + 14);
    const std::uint8_t fat_count = sector[16];
    const std::uint16_t root_entries = ReadLe16(sector + 17);
    const std::uint16_t fat16_sectors = ReadLe16(sector + 22);
    const std::uint32_t fat32_sectors = ReadLe32(sector + 36);
    const std::uint32_t total_sectors = ReadLe16(sector + 19) != 0
        ? ReadLe16(sector + 19)
        : ReadLe32(sector + 32);
    if ((sector[0] != 0xeb && sector[0] != 0xe9) ||
        (expected_sector_size & (expected_sector_size - 1)) != 0 ||
        sectors_per_cluster == 0 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 || (fat_count != 1 && fat_count != 2) ||
        total_sectors == 0) {
        return std::nullopt;
    }
    const std::uint64_t fat_sectors =
        fat16_sectors != 0 ? fat16_sectors : fat32_sectors;
    if (fat_sectors == 0) {
        return std::nullopt;
    }
    const std::uint64_t root_bytes =
        static_cast<std::uint64_t>(root_entries) * 32U;
    const std::uint64_t root_sectors =
        (root_bytes + expected_sector_size - 1U) / expected_sector_size;
    const std::uint64_t fat_region =
        static_cast<std::uint64_t>(fat_count) * fat_sectors;
    const std::uint64_t first_data_sector =
        static_cast<std::uint64_t>(reserved_sectors) + fat_region + root_sectors;
    if (first_data_sector >= total_sectors) {
        return std::nullopt;
    }
    const std::uint64_t data_sectors = total_sectors - first_data_sector;
    const std::uint64_t cluster_count = data_sectors / sectors_per_cluster;
    if (cluster_count == 0) {
        return std::nullopt;
    }
    const int fat_bits = cluster_count < 4085 ? 12 :
                         (cluster_count < 65525 ? 16 : 32);
    const bool fat32 = fat_bits == 32;
    if ((fat32 && (root_entries != 0 || fat16_sectors != 0 || fat32_sectors == 0)) ||
        (!fat32 && (root_entries == 0 || fat16_sectors == 0))) {
        return std::nullopt;
    }
    const std::uint64_t fat_bytes = fat_sectors * expected_sector_size;
    const std::uint64_t addressable_clusters = fat_bits == 12
        ? (fat_bytes * 2U) / 3U
        : fat_bytes / static_cast<std::uint64_t>(fat_bits / 8);
    if (addressable_clusters < cluster_count + 2U) {
        return std::nullopt;
    }
    const std::size_t serial_offset = fat32 ? 67 : 39;
    const std::size_t label_offset = fat32 ? 71 : 43;
    const std::size_t extended_signature_offset = fat32 ? 66 : 38;
    if (sector[extended_signature_offset] != 0x28 &&
        sector[extended_signature_offset] != 0x29) {
        return std::nullopt;
    }
    if (fat32) {
        const std::uint64_t root_cluster = ReadLe32(sector + 44);
        if (root_cluster < 2 || root_cluster >= cluster_count + 2U) {
            return std::nullopt;
        }
    }
    const std::uint32_t serial = ReadLe32(sector + serial_offset);
    if (serial == 0) {
        return std::nullopt;
    }
    std::string label(reinterpret_cast<const char*>(sector + label_offset), 11);
    while (!label.empty() && label.back() == ' ') {
        label.pop_back();
    }
    if (!IsPrintableAscii(label)) {
        return std::nullopt;
    }
    return FatVolumeIdentity{serial, label};
}

bool PhysicalSdCid::operator==(const PhysicalSdCid& other) const {
    return manufacturer_id == other.manufacturer_id && oem_id == other.oem_id &&
           product_name == other.product_name && revision == other.revision &&
           serial == other.serial && manufacturing_date == other.manufacturing_date;
}

bool PhysicalSdIdentity::operator==(const PhysicalSdIdentity& other) const {
    return cid == other.cid && cid_fingerprint == other.cid_fingerprint &&
           capacity_sectors == other.capacity_sectors &&
           sector_size_bytes == other.sector_size_bytes &&
           capacity_bytes == other.capacity_bytes &&
           mount_generation == other.mount_generation &&
           volume_serial == other.volume_serial && volume_label == other.volume_label;
}

bool PhysicalSdIdentityRegistry::ObserveMountedCard(
    const sdmmc_card_t* card,
    std::uint32_t volume_serial,
    std::string_view volume_label,
    std::uint64_t mount_generation
) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_ == PhysicalSdIdentityStatus::kInvalid ||
        status_ == PhysicalSdIdentityStatus::kCardSwapped) {
        return false;
    }
    const auto candidate =
        BuildIdentity(card, volume_serial, volume_label, mount_generation);
    if (!candidate.has_value()) {
        status_ = PhysicalSdIdentityStatus::kInvalid;
        return false;
    }
    if (identity_.has_value()) {
        if (!HasSameStableIdentity(*identity_, *candidate)) {
            status_ = PhysicalSdIdentityStatus::kCardSwapped;
            return false;
        }
        if (identity_->volume_label != candidate->volume_label) {
            status_ = PhysicalSdIdentityStatus::kInvalid;
            return false;
        }
    }
    identity_ = candidate;
    status_ = PhysicalSdIdentityStatus::kAvailable;
    return true;
}

PhysicalSdIdentityRegistry& PhysicalSdIdentityRegistry::GetInstance() {
    static PhysicalSdIdentityRegistry instance;
    return instance;
}

void PhysicalSdIdentityRegistry::RecordUnavailable(
    std::uint64_t mount_generation
) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_ == PhysicalSdIdentityStatus::kInvalid ||
        status_ == PhysicalSdIdentityStatus::kCardSwapped) {
        return;
    }
    if (identity_.has_value() && mount_generation != 0 &&
        mount_generation != identity_->mount_generation) {
        status_ = PhysicalSdIdentityStatus::kCardSwapped;
        return;
    }
    status_ = PhysicalSdIdentityStatus::kUnavailable;
}

PhysicalSdIdentitySnapshot PhysicalSdIdentityRegistry::Snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return SnapshotLocked();
}

PhysicalSdIdentitySnapshot PhysicalSdIdentityRegistry::RefreshAndSnapshot(
    PhysicalSdLifecycleObservation lifecycle
) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_ == PhysicalSdIdentityStatus::kInvalid ||
        status_ == PhysicalSdIdentityStatus::kCardSwapped) {
        return SnapshotLocked();
    }
    if (identity_.has_value() && lifecycle.mount_generation != 0 &&
        lifecycle.mount_generation != identity_->mount_generation) {
        status_ = PhysicalSdIdentityStatus::kCardSwapped;
        return SnapshotLocked();
    }
    if (!lifecycle.present) {
        status_ = PhysicalSdIdentityStatus::kUnavailable;
        return SnapshotLocked();
    }
    if (lifecycle.mount_generation == 0 || !identity_.has_value() ||
        !lifecycle.volume.has_value()) {
        status_ = PhysicalSdIdentityStatus::kInvalid;
        return SnapshotLocked();
    }
    if (identity_->volume_serial != Hex(lifecycle.volume->serial, 8) ||
        identity_->volume_label != lifecycle.volume->label) {
        status_ = PhysicalSdIdentityStatus::kInvalid;
        return SnapshotLocked();
    }
    status_ = PhysicalSdIdentityStatus::kAvailable;
    return SnapshotLocked();
}

PhysicalSdIdentitySnapshot PhysicalSdIdentityRegistry::SnapshotLocked() const {
    return {
        status_,
        status_ == PhysicalSdIdentityStatus::kAvailable ? identity_ : std::nullopt,
    };
}

const char* PhysicalSdIdentityStatusName(PhysicalSdIdentityStatus status) {
    switch (status) {
        case PhysicalSdIdentityStatus::kUnavailable: return "unavailable";
        case PhysicalSdIdentityStatus::kAvailable: return "available";
        case PhysicalSdIdentityStatus::kInvalid: return "invalid";
        case PhysicalSdIdentityStatus::kCardSwapped: return "card_swapped";
    }
    return "invalid";
}

}  // namespace tbot
