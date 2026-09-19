#ifndef LESSON_ASSET_RETAINED_SELECTION_H
#define LESSON_ASSET_RETAINED_SELECTION_H
#include <cstdint>
#include <string>
class LessonAssetMutationLease;

struct RetainedSelectionOwner {
    std::string operation_id, request_id, device_id, consumer_id, lesson_row_id;
    std::string lesson_key, manifest_version, manifest_checksum, cache_key, descriptor_checksum;
    std::string assignment_id;
    std::uint64_t request_revision = 0, selection_revision = 0, assignment_version = 0, lesson_version = 0;
};
struct RetainedSelectionState {
    std::uint64_t revision = 0;
    bool active = false;
    RetainedSelectionOwner owner;
};

void ValidateRetainedSelectionOwner(const RetainedSelectionOwner& owner);
RetainedSelectionState ReadRetainedSelection();
void ApplyRetainedSelection(const LessonAssetMutationLease& mutation,
                           const RetainedSelectionOwner& owner, bool release);
bool RetainedSyncAllowed(const std::string& cache_key, std::uint64_t observed_revision,
                         const RetainedSelectionOwner* owner);
bool RetainedEvictionAllowed(const std::string& cache_key);
#endif
