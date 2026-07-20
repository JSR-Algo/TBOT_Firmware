#include "physical_sd_identity.h"

#include <cassert>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using tbot::PhysicalSdIdentityRegistry;
using tbot::PhysicalSdIdentityStatus;
using tbot::PhysicalSdLifecycleObservation;

namespace {

void WriteLe16(std::uint8_t* value, std::uint16_t number) {
    value[0] = static_cast<std::uint8_t>(number);
    value[1] = static_cast<std::uint8_t>(number >> 8);
}

void WriteLe32(std::uint8_t* value, std::uint32_t number) {
    value[0] = static_cast<std::uint8_t>(number);
    value[1] = static_cast<std::uint8_t>(number >> 8);
    value[2] = static_cast<std::uint8_t>(number >> 16);
    value[3] = static_cast<std::uint8_t>(number >> 24);
}

std::array<std::uint8_t, 512> Fat16Like(
    std::uint16_t total_sectors,
    std::uint16_t fat_sectors,
    std::uint16_t root_entries
) {
    std::array<std::uint8_t, 512> sector{};
    sector[0] = 0xeb;
    WriteLe16(sector.data() + 11, 512);
    sector[13] = 1;
    WriteLe16(sector.data() + 14, 1);
    sector[16] = 2;
    WriteLe16(sector.data() + 17, root_entries);
    WriteLe16(sector.data() + 19, total_sectors);
    WriteLe16(sector.data() + 22, fat_sectors);
    sector[38] = 0x29;
    WriteLe32(sector.data() + 39, 0x7a31f09c);
    std::memcpy(sector.data() + 43, "TBOT_HIL   ", 11);
    sector[510] = 0x55;
    sector[511] = 0xaa;
    return sector;
}

sdmmc_card_t Card() {
    sdmmc_card_t card{};
    card.cid.mfg_id = 0x1b;
    card.cid.oem_id = 0x534d;
    std::memcpy(card.cid.name, "00000", 5);
    card.cid.revision = 0x10;
    card.cid.serial = 0x4a5f7d3d;
    card.cid.date = 0x17b;
    card.csd.capacity = 62333952;
    card.csd.sector_size = 512;
    return card;
}

void TestValidIdentity() {
    PhysicalSdIdentityRegistry registry;
    auto card = Card();
    assert(registry.ObserveMountedCard(&card, 0x7a31f09c, "TBOT_HIL"));
    const auto snapshot = registry.Snapshot();
    assert(snapshot.status == PhysicalSdIdentityStatus::kAvailable);
    assert(snapshot.identity.has_value());
    const auto& identity = *snapshot.identity;
    assert(identity.cid_fingerprint ==
           "1b-534d-3030303030-10-4a5f7d3d-17b");
    assert(identity.cid.manufacturer_id == 0x1b);
    assert(identity.cid.oem_id == 0x534d);
    assert(identity.cid.product_name == "00000");
    assert(identity.cid.revision == 0x10);
    assert(identity.cid.serial == 0x4a5f7d3dU);
    assert(identity.cid.manufacturing_date == 0x17b);
    assert(identity.capacity_sectors == 62333952U);
    assert(identity.sector_size_bytes == 512U);
    assert(identity.capacity_bytes == 31914983424ULL);
    assert(identity.mount_generation == 1);
    assert(identity.volume_serial == "7a31f09c");
    assert(identity.volume_label == "TBOT_HIL");
}

void TestMissingAndUninitializedCardsFailClosed() {
    PhysicalSdIdentityRegistry missing;
    assert(!missing.ObserveMountedCard(nullptr, 1, "TBOT"));
    assert(missing.Snapshot().status == PhysicalSdIdentityStatus::kInvalid);

    auto card = Card();
    std::memset(&card.cid, 0, sizeof(card.cid));
    PhysicalSdIdentityRegistry zero_cid;
    assert(!zero_cid.ObserveMountedCard(&card, 1, "TBOT"));
    assert(zero_cid.Snapshot().status == PhysicalSdIdentityStatus::kInvalid);

    card = Card();
    card.csd.capacity = 0;
    PhysicalSdIdentityRegistry zero_capacity;
    assert(!zero_capacity.ObserveMountedCard(&card, 1, "TBOT"));
    assert(zero_capacity.Snapshot().status == PhysicalSdIdentityStatus::kInvalid);

    card = Card();
    card.csd.sector_size = 0;
    PhysicalSdIdentityRegistry zero_sector;
    assert(!zero_sector.ObserveMountedCard(&card, 1, "TBOT"));
    assert(zero_sector.Snapshot().status == PhysicalSdIdentityStatus::kInvalid);
}

void TestMalformedCidAndVolumeFailClosed() {
    auto card = Card();
    card.cid.name[1] = '\x01';
    PhysicalSdIdentityRegistry control_name;
    assert(!control_name.ObserveMountedCard(&card, 1, "TBOT"));

    card = Card();
    card.cid.serial = 0;
    PhysicalSdIdentityRegistry zero_serial;
    assert(!zero_serial.ObserveMountedCard(&card, 1, "TBOT"));

    card = Card();
    PhysicalSdIdentityRegistry zero_volume;
    assert(!zero_volume.ObserveMountedCard(&card, 0, "TBOT"));

    PhysicalSdIdentityRegistry long_label;
    assert(!long_label.ObserveMountedCard(&card, 1, "TWELVE_CHARS"));

    PhysicalSdIdentityRegistry control_label;
    assert(!control_label.ObserveMountedCard(&card, 1, "TBOT\nHIL"));

    card = Card();
    std::memset(card.cid.name, 0, sizeof(card.cid.name));
    std::memcpy(card.cid.name, "FOUR", 4);
    PhysicalSdIdentityRegistry short_product;
    assert(!short_product.ObserveMountedCard(&card, 1, "TBOT"));

    card = Card();
    std::memset(card.cid.name, 0, sizeof(card.cid.name));
    std::memcpy(card.cid.name, "SIX123", 6);
    PhysicalSdIdentityRegistry long_product;
    assert(!long_product.ObserveMountedCard(&card, 1, "TBOT"));
}

void TestRepeatIsIdempotentAndSwapIsTerminal() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 0x7a31f09c, "TBOT_HIL"));
    assert(registry.ObserveMountedCard(&card, 0x7a31f09c, "TBOT_HIL"));
    assert(registry.Snapshot().status == PhysicalSdIdentityStatus::kAvailable);

    auto replacement = card;
    replacement.cid.serial += 1;
    assert(!registry.ObserveMountedCard(&replacement, 0x7a31f09c, "TBOT_HIL"));
    auto swapped = registry.Snapshot();
    assert(swapped.status == PhysicalSdIdentityStatus::kCardSwapped);
    assert(!swapped.identity.has_value());

    assert(!registry.ObserveMountedCard(&card, 0x7a31f09c, "TBOT_HIL"));
    assert(registry.Snapshot().status == PhysicalSdIdentityStatus::kCardSwapped);
}

void TestChangedGeometryOrVolumeIsSwap() {
    auto card = Card();
    PhysicalSdIdentityRegistry geometry;
    assert(geometry.ObserveMountedCard(&card, 1, "TBOT"));
    auto larger = card;
    larger.csd.capacity += 1;
    assert(!geometry.ObserveMountedCard(&larger, 1, "TBOT"));
    assert(geometry.Snapshot().status == PhysicalSdIdentityStatus::kCardSwapped);

    PhysicalSdIdentityRegistry volume;
    assert(volume.ObserveMountedCard(&card, 1, "TBOT"));
    assert(!volume.ObserveMountedCard(&card, 2, "TBOT"));
    assert(volume.Snapshot().status == PhysicalSdIdentityStatus::kCardSwapped);
}

void TestLabelOnlyMutationIsInvalidNotCardSwap() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 1, "TBOT"));
    assert(!registry.ObserveMountedCard(&card, 1, "RENAMED"));
    const auto snapshot = registry.Snapshot();
    assert(snapshot.status == PhysicalSdIdentityStatus::kInvalid);
    assert(!snapshot.identity.has_value());
}

void TestEveryPublicationRechecksPresenceAndMountGeneration() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(
        &card, 0x7a31f09c, "TBOT_HIL", 41));
    const auto first = registry.RefreshAndSnapshot(
        {true, 41, tbot::FatVolumeIdentity{0x7a31f09c, "TBOT_HIL"}});
    assert(first.status == PhysicalSdIdentityStatus::kAvailable);
    const auto second = registry.RefreshAndSnapshot(
        {true, 42, tbot::FatVolumeIdentity{0x7a31f09c, "TBOT_HIL"}});
    assert(second.status == PhysicalSdIdentityStatus::kCardSwapped);
    assert(!second.identity.has_value());
}

void TestMissingCardPresenceFailsClosedAtPublication() {
    PhysicalSdIdentityRegistry missing;
    assert(missing.RefreshAndSnapshot({false, 0, std::nullopt}).status ==
           PhysicalSdIdentityStatus::kUnavailable);

    auto card = Card();
    PhysicalSdIdentityRegistry removed;
    assert(removed.ObserveMountedCard(&card, 1, "TBOT", 7));
    assert(removed.RefreshAndSnapshot({false, 7, std::nullopt}).status ==
           PhysicalSdIdentityStatus::kUnavailable);
    auto replacement = card;
    replacement.cid.serial += 1;
    assert(!removed.ObserveMountedCard(&replacement, 1, "TBOT", 8));
    assert(removed.Snapshot().status == PhysicalSdIdentityStatus::kCardSwapped);
}

void TestInvalidPresenceGenerationFailsClosedAtPublication() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 1, "TBOT", 1));
    const auto snapshot = registry.RefreshAndSnapshot(
        {true, 0, tbot::FatVolumeIdentity{1, "TBOT"}});
    assert(snapshot.status == PhysicalSdIdentityStatus::kInvalid);
    assert(!snapshot.identity.has_value());
}

void TestRegistrySnapshotsAreThreadSafeValueCopies() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 1, "TBOT", 9));

    std::vector<std::thread> workers;
    for (int index = 0; index < 8; ++index) {
        workers.emplace_back([&registry]() {
            for (int iteration = 0; iteration < 500; ++iteration) {
                const auto snapshot = registry.RefreshAndSnapshot(
                    {true, 9, tbot::FatVolumeIdentity{1, "TBOT"}});
                assert(snapshot.status == PhysicalSdIdentityStatus::kAvailable);
                assert(snapshot.identity.has_value());
                assert(snapshot.identity->cid.serial == 0x4a5f7d3dU);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

void TestRemountGenerationDetectsReplacementWithoutProtocolReset() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 1, "TBOT", 10));
    assert(registry.RefreshAndSnapshot(
               {true, 10, tbot::FatVolumeIdentity{1, "TBOT"}}).status ==
           PhysicalSdIdentityStatus::kAvailable);
    assert(registry.RefreshAndSnapshot(
               {true, 11, tbot::FatVolumeIdentity{1, "TBOT"}}).status ==
           PhysicalSdIdentityStatus::kCardSwapped);
}

void TestSameMountVolumeMutationIsInvalid() {
    auto card = Card();
    PhysicalSdIdentityRegistry serial;
    assert(serial.ObserveMountedCard(&card, 1, "TBOT", 3));
    assert(serial.RefreshAndSnapshot(
               {true, 3, tbot::FatVolumeIdentity{2, "TBOT"}}).status ==
           PhysicalSdIdentityStatus::kInvalid);

    PhysicalSdIdentityRegistry label;
    assert(label.ObserveMountedCard(&card, 1, "TBOT", 3));
    assert(label.RefreshAndSnapshot(
               {true, 3, tbot::FatVolumeIdentity{1, "RENAMED"}}).status ==
           PhysicalSdIdentityStatus::kInvalid);
}

void TestSameCardRemountPolicyIsTerminalSwap() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 1, "TBOT", 1));
    assert(registry.RefreshAndSnapshot({false, 1, std::nullopt}).status ==
           PhysicalSdIdentityStatus::kUnavailable);
    assert(!registry.ObserveMountedCard(&card, 1, "TBOT", 2));
    assert(registry.Snapshot().status == PhysicalSdIdentityStatus::kCardSwapped);
}

void TestGenerationChangeWinsOverUnavailableAndRemainsTerminal() {
    auto card = Card();

    PhysicalSdIdentityRegistry setup_failure;
    assert(setup_failure.ObserveMountedCard(&card, 1, "TBOT", 1));
    setup_failure.RecordUnavailable(2);
    assert(setup_failure.Snapshot().status ==
           PhysicalSdIdentityStatus::kCardSwapped);
    assert(!setup_failure.ObserveMountedCard(&card, 1, "TBOT", 2));
    assert(setup_failure.Snapshot().status ==
           PhysicalSdIdentityStatus::kCardSwapped);

    PhysicalSdIdentityRegistry presence_failure;
    assert(presence_failure.ObserveMountedCard(&card, 1, "TBOT", 1));
    assert(presence_failure.RefreshAndSnapshot({false, 2, std::nullopt}).status ==
           PhysicalSdIdentityStatus::kCardSwapped);
    assert(!presence_failure.ObserveMountedCard(&card, 1, "TBOT", 2));
    assert(presence_failure.Snapshot().status ==
           PhysicalSdIdentityStatus::kCardSwapped);
}

void TestSameGenerationTemporaryAbsenceCanRecover() {
    auto card = Card();
    PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 1, "TBOT", 5));
    const auto unavailable =
        registry.RefreshAndSnapshot({false, 5, std::nullopt});
    assert(unavailable.status == PhysicalSdIdentityStatus::kUnavailable);
    assert(!unavailable.identity.has_value());
    const auto recovered = registry.RefreshAndSnapshot(
        {true, 5, tbot::FatVolumeIdentity{1, "TBOT"}});
    assert(recovered.status == PhysicalSdIdentityStatus::kAvailable);
    assert(recovered.identity.has_value());
    assert(recovered.identity->mount_generation == 5);
}

void TestFat32VolumeIdentityParsesFromBoundSector() {
    std::array<std::uint8_t, 512> sector{};
    sector[11] = 0x00;
    sector[12] = 0x02;
    sector[0] = 0xeb;
    sector[13] = 8;
    sector[14] = 32;
    sector[16] = 2;
    sector[32] = 0x00;
    sector[33] = 0x00;
    sector[34] = 0x10;
    sector[35] = 0x00;
    WriteLe32(sector.data() + 36, 1024);
    sector[44] = 2;
    sector[17] = 0;
    sector[18] = 0;
    sector[22] = 0;
    sector[23] = 0;
    sector[67] = 0x9c;
    sector[68] = 0xf0;
    sector[69] = 0x31;
    sector[70] = 0x7a;
    sector[66] = 0x29;
    std::memcpy(sector.data() + 71, "TBOT_HIL   ", 11);
    sector[510] = 0x55;
    sector[511] = 0xaa;

    const auto identity = tbot::ParseFatVolumeIdentity(sector.data(), sector.size(), 512);
    assert(identity.has_value());
    assert(identity->serial == 0x7a31f09cU);
    assert(identity->label == "TBOT_HIL");
}

void TestFat12AndFat16VolumeIdentityParseWithExtendedSignature() {
    auto fat12 = Fat16Like(3000, 9, 224);
    fat12[38] = 0x28;
    assert(tbot::ParseFatVolumeIdentity(fat12.data(), fat12.size(), 512));

    auto fat16 = Fat16Like(10000, 40, 512);
    const auto identity =
        tbot::ParseFatVolumeIdentity(fat16.data(), fat16.size(), 512);
    assert(identity.has_value());
    assert(identity->serial == 0x7a31f09cU);
}

void TestFatParserRejectsHostileExtendedSignatureAndRegionArithmetic() {
    auto no_extended_signature = Fat16Like(10000, 40, 512);
    no_extended_signature[38] = 0;
    assert(!tbot::ParseFatVolumeIdentity(
        no_extended_signature.data(), no_extended_signature.size(), 512));

    auto regions_exceed_volume = Fat16Like(100, 40, 512);
    assert(!tbot::ParseFatVolumeIdentity(
        regions_exceed_volume.data(), regions_exceed_volume.size(), 512));

    auto fat32 = std::array<std::uint8_t, 512>{};
    fat32[0] = 0xeb;
    WriteLe16(fat32.data() + 11, 512);
    fat32[13] = 8;
    WriteLe16(fat32.data() + 14, 32);
    fat32[16] = 2;
    WriteLe32(fat32.data() + 32, 0x00100000);
    WriteLe32(fat32.data() + 36, 1024);
    WriteLe32(fat32.data() + 44, 200000);
    fat32[66] = 0x29;
    WriteLe32(fat32.data() + 67, 0x7a31f09c);
    std::memcpy(fat32.data() + 71, "TBOT_HIL   ", 11);
    fat32[510] = 0x55;
    fat32[511] = 0xaa;
    assert(!tbot::ParseFatVolumeIdentity(fat32.data(), fat32.size(), 512));

    WriteLe32(fat32.data() + 44, 2);
    WriteLe32(fat32.data() + 36, 0xffffffffU);
    assert(!tbot::ParseFatVolumeIdentity(fat32.data(), fat32.size(), 512));

    WriteLe32(fat32.data() + 32, 10000);
    WriteLe32(fat32.data() + 36, 40);
    assert(!tbot::ParseFatVolumeIdentity(fat32.data(), fat32.size(), 512));
}

void TestFatVolumeIdentityRejectsMalformedOrUnboundSector() {
    std::array<std::uint8_t, 512> sector{};
    sector[11] = 0x00;
    sector[12] = 0x02;
    sector[510] = 0x55;
    sector[511] = 0xaa;
    assert(!tbot::ParseFatVolumeIdentity(sector.data(), sector.size(), 4096));
    assert(!tbot::ParseFatVolumeIdentity(sector.data(), sector.size(), 512));
    sector[67] = 1;
    assert(!tbot::ParseFatVolumeIdentity(sector.data(), 511, 512));
    assert(!tbot::ParseFatVolumeIdentity(nullptr, sector.size(), 512));

    std::array<std::uint8_t, 512> fake{};
    fake[11] = 0x00;
    fake[12] = 0x02;
    fake[67] = 1;
    std::memset(fake.data() + 71, ' ', 11);
    fake[510] = 0x55;
    fake[511] = 0xaa;
    assert(!tbot::ParseFatVolumeIdentity(fake.data(), fake.size(), 512));
}

void TestUnavailableSignalNeverDowngradesTerminalFailure() {
    auto card = Card();
    PhysicalSdIdentityRegistry invalid;
    assert(!invalid.ObserveMountedCard(nullptr, 1, "TBOT"));
    invalid.RecordUnavailable();
    assert(invalid.Snapshot().status == PhysicalSdIdentityStatus::kInvalid);

    PhysicalSdIdentityRegistry swapped;
    assert(swapped.ObserveMountedCard(&card, 1, "TBOT"));
    auto replacement = card;
    replacement.cid.serial += 1;
    assert(!swapped.ObserveMountedCard(&replacement, 1, "TBOT"));
    swapped.RecordUnavailable();
    assert(swapped.Snapshot().status == PhysicalSdIdentityStatus::kCardSwapped);
}

}  // namespace

int main() {
    TestValidIdentity();
    TestMissingAndUninitializedCardsFailClosed();
    TestMalformedCidAndVolumeFailClosed();
    TestRepeatIsIdempotentAndSwapIsTerminal();
    TestChangedGeometryOrVolumeIsSwap();
    TestLabelOnlyMutationIsInvalidNotCardSwap();
    TestEveryPublicationRechecksPresenceAndMountGeneration();
    TestMissingCardPresenceFailsClosedAtPublication();
    TestInvalidPresenceGenerationFailsClosedAtPublication();
    TestRegistrySnapshotsAreThreadSafeValueCopies();
    TestRemountGenerationDetectsReplacementWithoutProtocolReset();
    TestSameMountVolumeMutationIsInvalid();
    TestSameCardRemountPolicyIsTerminalSwap();
    TestGenerationChangeWinsOverUnavailableAndRemainsTerminal();
    TestSameGenerationTemporaryAbsenceCanRecover();
    TestFat32VolumeIdentityParsesFromBoundSector();
    TestFat12AndFat16VolumeIdentityParseWithExtendedSignature();
    TestFatParserRejectsHostileExtendedSignatureAndRegionArithmetic();
    TestFatVolumeIdentityRejectsMalformedOrUnboundSector();
    TestUnavailableSignalNeverDowngradesTerminalFailure();
    return 0;
}
