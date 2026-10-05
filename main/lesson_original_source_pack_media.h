#ifndef LESSON_ORIGINAL_SOURCE_PACK_MEDIA_H
#define LESSON_ORIGINAL_SOURCE_PACK_MEDIA_H

#include "lesson_original_source_player.h"
#include "lesson_original_source_session.h"

#include <atomic>
#include <functional>
#include <string>

// Renderer-v6 inputs from the verified SD asset pack. The ESP writes every pack
// asset to <pack root>/<cacheKey>/<percent-encoded key> (the path the sync policy
// validates); the scene reference names the pack's cacheKey, the scene is
// "scene.original-source.v1" and each pinned original is "original.<assetVersionId>".
// Every read happens under a lesson read lease and is checked against the pinned
// length and sha256 before use.
namespace tbot {

inline constexpr char kOriginalSourcePackRoot[] = "/sdcard/tbot/lesson-assets";
inline constexpr char kOriginalSourceSceneAssetKey[] = "scene.original-source.v1";

// SD path of `asset_key` in pack `cache_key`, or empty when either is not canonical.
std::string OriginalSourcePackPath(const std::string& root, const std::string& cache_key,
                                   const std::string& asset_key);

using OriginalSourceLeaseSource = std::function<LessonAssetReadLease()>;

// Scene loader for the controller: reads the scene document of the referenced pack
// under a read lease and returns it only when its length and sha256 match.
OriginalSourceSceneLoader MakeOriginalSourcePackSceneLoader(std::string root, OriginalSourceLeaseSource lease);

class OriginalSourcePackMedia final : public OriginalSourceMediaProvider {
public:
    OriginalSourcePackMedia(std::string root, OriginalSourceLeaseSource lease, OriginalSourceAllocationState* allocations,
                            const std::atomic<bool>* cancelled);
    OriginalSourceStatus Open(const std::string& cache_key, const OriginalSourceOriginal& original,
                              std::unique_ptr<OriginalSourceStream>* out) override;

private:
    std::string root_;
    OriginalSourceLeaseSource lease_;
    OriginalSourceAllocationState* allocations_;
    const std::atomic<bool>* cancelled_;
};

}  // namespace tbot

#endif
