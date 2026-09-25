#include "lesson_asset_retained_selection.h"
#include "lesson_asset_retained_parser.h"
#include "lesson_asset_storage_coordinator.h"
#include "checked_cjson.h"
#include <nvs.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream file(argv[1]);
    const std::string raw((std::istreambuf_iterator<char>(file)), {});
    CheckedCJsonPtr vectors(cJSON_Parse(raw.c_str()));
    const cJSON* item;
    RetainedSelectionOwner owner;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(vectors.get(), "valid")) {
        const auto op = cJSON_GetObjectItem(item, "operation");
        if (std::string(cJSON_GetObjectItem(op, "action")->valuestring) == "bind")
            owner = ParseRetainedDeviceOperation(op).owner;
    }
    int checks = 0;
    const auto expect = [&](bool value) { ++checks; if (!value) throw std::runtime_error("NVS check " + std::to_string(checks)); };
    auto lease = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("nvs-fixture");
    expect(ReadRetainedSelection().revision == 0);
    ApplyRetainedSelection(lease, owner, false);
    expect(ReadRetainedSelection().active && retained_nvs_open_handles == 0);
    const auto original = retained_nvs_blob;
    const std::string mode(argv[2]);
    retained_nvs_failure = mode;
    if (mode == "corrupt") retained_nvs_blob = "incomplete record";
    bool refused = false;
    try {
        if (mode == "set" || mode == "commit") {
            auto release = owner; release.selection_revision++; release.request_revision++;
            release.assignment_id.clear(); release.assignment_version = 0;
            ApplyRetainedSelection(lease, release, true);
        } else (void)ReadRetainedSelection();
    } catch (const std::runtime_error&) { refused = true; }
    expect(refused && retained_nvs_open_handles == 0);
    expect(!RetainedSyncAllowed(owner.cache_key, owner.selection_revision, &owner));
    expect(!RetainedEvictionAllowed(owner.cache_key));
    retained_nvs_failure.clear();
    if (mode == "set" || mode == "commit") {
        expect(!RetainedSyncAllowed(owner.cache_key, owner.selection_revision, &owner));
    } else {
        retained_nvs_blob = original;
        expect(RetainedSyncAllowed(owner.cache_key, owner.selection_revision, &owner));
    }
    std::cout << "ESP NVS API fault " << mode << " OK (" << checks << " checks); injected NVS API\n";
}
