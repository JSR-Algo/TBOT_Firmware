#include "lesson_asset_retained_selection.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_asset_pack_activation.h"
#include "lesson_asset_cache_evict.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <filesystem>

static int checks = 0;
static void Expect(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("retained selection check failed " + std::to_string(checks));
}
template<class Callback> static void Refuses(Callback callback) {
    bool refused = false;
    try { callback(); } catch (const std::runtime_error&) { refused = true; }
    Expect(refused);
}
static RetainedSelectionOwner Owner() {
    RetainedSelectionOwner owner;
    owner.operation_id = "10000000-0000-0000-0000-000000000001";
    owner.request_id = "20000000-0000-0000-0000-000000000001";
    owner.device_id = "30000000-0000-0000-0000-000000000001";
    owner.consumer_id = "40000000-0000-0000-0000-000000000001";
    owner.lesson_row_id = "50000000-0000-0000-0000-000000000001";
    owner.assignment_id = "60000000-0000-0000-0000-000000000001";
    owner.request_revision = 2; owner.selection_revision = 2; owner.assignment_version = 1;
    owner.lesson_key = "retained-farm"; owner.lesson_version = 1;
    owner.manifest_version = "teebot-lesson-renderer.v5";
    owner.manifest_checksum = std::string(64, 'a');
    owner.descriptor_checksum = std::string(64, 'b');
    owner.cache_key = owner.lesson_key + "/v1-" + owner.manifest_checksum;
    return owner;
}
int main(int argc, char** argv) {
    if (argc != 2 || !std::getenv("TBOT_RETAINED_TEST_STATE_PATH")) return 2;
    const auto owner = Owner();
    if (std::string(argv[1]) == "restart") {
        const auto state = ReadRetainedSelection();
        Expect(state.revision == 3 && !state.active);
        Expect(!RetainedSyncAllowed(owner.cache_key, 2, nullptr));
        std::cout << "retained restart OK (" << checks << " checks)\n";
        return 0;
    }
    Expect(ReadRetainedSelection().revision == 0);
    {
        auto lease = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("retained");
        Expect(static_cast<bool>(lease));
        ApplyRetainedSelection(lease, owner, false);
        ApplyRetainedSelection(lease, owner, false);
    }
    Expect(ReadRetainedSelection().active);
    const auto asset_root = std::getenv("TBOT_RETAINED_TEST_ASSET_ROOT");
    if (!asset_root) return 2;
    std::filesystem::create_directories(std::string(asset_root) + "/" + owner.cache_key);
    std::ofstream(std::string(asset_root) + "/" + owner.cache_key + "/asset.bin") << "owned-test";
    Expect(!ActivateLessonAssetPack(owner.lesson_key, owner.cache_key, owner.manifest_checksum, true).activated);
    Expect(!EvictLessonAssetCacheKey(owner.cache_key, false).evicted);
    Expect(!RetainedSyncAllowed(owner.cache_key, 0, nullptr));
    Expect(RetainedSyncAllowed(owner.cache_key, 2, &owner));
    Expect(!RetainedEvictionAllowed(owner.cache_key));
    auto stale = owner; stale.selection_revision = 1;
    Refuses([&] {
        auto lease = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("retained");
        ApplyRetainedSelection(lease, stale, false);
    });
    Expect(ReadRetainedSelection().revision == 2);
    auto other = owner; other.request_id[0] = '9'; other.selection_revision = 3;
    Refuses([&] {
        auto lease = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("retained");
        ApplyRetainedSelection(lease, other, false);
    });
    Expect(ReadRetainedSelection().owner.request_id == owner.request_id);
    auto released = owner; released.selection_revision = 3; released.request_revision = 3;
    released.operation_id[0] = '9'; released.assignment_id.clear(); released.assignment_version = 0;
    {
        const auto session = LessonAssetStorageCoordinator::GetInstance().TryBeginLessonSession("assignment", "session");
        Expect(session.acquired);
        auto lease = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("retained");
        Refuses([&] { ApplyRetainedSelection(lease, released, true); });
        Expect(ReadRetainedSelection().active);
        LessonAssetStorageCoordinator::GetInstance().EndLessonSession("assignment", "session", session.generation);
    }
    {
        auto lease = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("retained");
        ApplyRetainedSelection(lease, released, true);
        ApplyRetainedSelection(lease, released, true);
    }
    Expect(!ReadRetainedSelection().active);
    Expect(!RetainedSyncAllowed(owner.cache_key, 2, nullptr));
    Expect(RetainedSyncAllowed(owner.cache_key, 3, nullptr));
    Expect(!RetainedSyncAllowed(owner.cache_key, 3, &owner));
    std::cout << "retained selection OK (" << checks << " checks)\n";
}
