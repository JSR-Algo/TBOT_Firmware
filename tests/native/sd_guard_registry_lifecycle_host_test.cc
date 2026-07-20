#include "physical_sd_identity.h"
#include "sd_fat_session_guard.h"

#include <cassert>
#include <cstring>
#include <optional>

namespace {

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

}  // namespace

int main() {
    tbot::SdFatSessionGuard guard;
    auto session = guard.Acquire();
    const auto generation = guard.RecordMounted(session);
    bool physically_present = true;
    int volume_reads = 0;
    assert(guard.SetPresenceProbe(session, [&]() { return physically_present; }));
    assert(guard.SetVolumeProbe(session, [&]() {
        ++volume_reads;
        return std::optional<tbot::SdFatVolumeMetadata>{{1, "TBOT"}};
    }));

    auto card = Card();
    tbot::PhysicalSdIdentityRegistry registry;
    assert(registry.ObserveMountedCard(&card, 1, "TBOT", generation));

    auto lifecycle = guard.Snapshot(session);
    assert(lifecycle.present && lifecycle.volume.has_value());
    assert(registry.RefreshAndSnapshot(
               {lifecycle.present, lifecycle.mount_generation,
                tbot::FatVolumeIdentity{lifecycle.volume->serial,
                                        lifecycle.volume->label}}).status ==
           tbot::PhysicalSdIdentityStatus::kAvailable);
    assert(volume_reads == 1);

    physically_present = false;
    lifecycle = guard.Snapshot(session);
    assert(!lifecycle.present);
    assert(!lifecycle.volume.has_value());
    assert(registry.RefreshAndSnapshot(
               {lifecycle.present, lifecycle.mount_generation, std::nullopt}).status ==
           tbot::PhysicalSdIdentityStatus::kUnavailable);
    assert(volume_reads == 1);

    physically_present = true;
    lifecycle = guard.Snapshot(session);
    assert(lifecycle.present);
    assert(lifecycle.volume.has_value());
    assert(volume_reads == 2);
    const auto recovered = registry.RefreshAndSnapshot(
        {lifecycle.present, lifecycle.mount_generation,
         tbot::FatVolumeIdentity{lifecycle.volume->serial,
                                 lifecycle.volume->label}});
    assert(recovered.status == tbot::PhysicalSdIdentityStatus::kAvailable);
    assert(recovered.identity.has_value());
    assert(recovered.identity->mount_generation == generation);
    return 0;
}
