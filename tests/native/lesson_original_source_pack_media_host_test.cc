// Renderer-v6 SD pack inputs: pack paths follow the sync path policy, the scene is
// returned only with the referenced length and sha256 under a read lease, and every
// pinned original (H.264, VP9 alpha, PNG) opens from its pack key and decodes.
#include "lesson_asset_storage_coordinator.h"
#include "lesson_original_source_allocator.h"
#include "lesson_original_source_pack_media.h"

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tbot;

namespace {
int failures = 0, checks = 0;
void Expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition && failures++ < 20) std::fprintf(stderr, "FAIL %s\n", message.c_str());
}
std::string Hex(const std::uint8_t* bytes) {
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    for (int index = 0; index < 32; ++index) {
        out.push_back(kHex[bytes[index] >> 4]);
        out.push_back(kHex[bytes[index] & 15]);
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;  // R01 fixture manifest, owned scratch dir
    const std::string root = std::string(argv[2]) + "/pack";
    const std::string key = "farm-original/v1-" + std::string(64, 'a');
    std::filesystem::remove_all(argv[2]);
    std::filesystem::create_directories(root + "/" + key);

    // Paths: the ESP's localPath for the same key, percent-encoded basename.
    Expect(OriginalSourcePackPath(root, key, "scene.original-source.v1") == root + "/" + key + "/scene.original-source.v1",
           "scene path");
    Expect(OriginalSourcePackPath(root, key, "original.30000000-0000-4000-8000-000000000001") ==
               root + "/" + key + "/original.30000000-0000-4000-8000-000000000001", "original path");
    Expect(OriginalSourcePackPath(root, key, "a b") == root + "/" + key + "/a%20b", "percent encoding");
    Expect(OriginalSourcePackPath(root, "../escape/v1-" + std::string(64, 'a'), "scene.original-source.v1").empty() &&
               OriginalSourcePackPath(root, "Farm/v1-x", "scene.original-source.v1").empty() &&
               OriginalSourcePackPath(root, key, "").empty(), "non-canonical cache keys and keys are refused");

    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    const auto begun = coordinator.TryBeginLessonSession("assignment-pack", "session-pack");
    Expect(begun.acquired, "lesson session");
    bool lease_available = true;
    const OriginalSourceLeaseSource lease = [&]() {
        return lease_available ? coordinator.TryRetainLessonSession("assignment-pack", "session-pack", begun.generation)
                               : LessonAssetReadLease{};
    };

    // Scene loader.
    const std::string scene = "{\"contractVersion\":\"original-source-scene.v1\"}";
    std::ofstream(root + "/" + key + "/scene.original-source.v1", std::ios::binary) << scene;
    std::uint8_t digest[32];
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(scene.data()), scene.size(), digest, 0);
#else
    mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(scene.data()), scene.size(), digest, 0);
#endif
    const std::string actual_sha = Hex(digest);
    std::string wrong_sha = actual_sha;
    std::reverse(wrong_sha.begin(), wrong_sha.end());
    {
        const auto loader = MakeOriginalSourcePackSceneLoader(root, lease);
        std::string json;
        Expect(loader(key, actual_sha, scene.size(), &json) == nullptr && json == scene, "verified scene is returned");
        Expect(std::string(loader(key, wrong_sha, scene.size(), &json)) == "scene sha256 differs from the reference",
               "wrong scene digest is refused");
        Expect(std::string(loader(key, actual_sha, scene.size() - 1, &json)) == "scene is longer than the reference",
               "longer scene is refused");
        Expect(std::string(loader(key, actual_sha, scene.size() + 1, &json)) == "scene is shorter than the reference",
               "shorter scene is refused");
        Expect(std::string(loader("farm-original/v2-" + std::string(64, 'a'), actual_sha, scene.size(), &json)) ==
                   "scene is not in the pack", "another pack's scene is absent");
        Expect(std::string(loader(key, "ABC", scene.size(), &json)) == "scene sha256 is not canonical",
               "non-canonical digest");
        lease_available = false;
        Expect(std::string(loader(key, actual_sha, scene.size(), &json)) == "lesson read lease unavailable",
               "no read without a lesson lease");
        lease_available = true;
    }

    // Originals from the R01 fixture manifest:
    // <path> <sha256> <bytes> <width> <height> <frames> <codec> <reference>
    OriginalSourceAllocationState allocations;
    OriginalSourceAllocatorBackend host{
        nullptr,
        [](void*, std::size_t alignment, std::size_t bytes) -> void* {
            void* pointer = nullptr;
            return posix_memalign(&pointer, std::max(alignment, sizeof(void*)), bytes) == 0 ? pointer : nullptr;
        },
        [](void*, void* pointer) { std::free(pointer); },
        [](void*, void*, std::size_t requested) { return requested; }};
    OriginalSourceAllocator allocator(allocations, host);
    Expect(allocator.Bind(), "allocator bound");
    OriginalSourcePackMedia media(root, lease, &allocations, nullptr);
    std::ifstream manifest(argv[1]);
    std::string line;
    int index = 0, codecs_seen[3] = {};
    while (std::getline(manifest, line)) {
        std::istringstream row(line);
        std::string path, sha, reference;
        unsigned bytes, width, height, frames, codec;
        if (!(row >> path >> sha >> bytes >> width >> height >> frames >> codec >> reference)) continue;
        OriginalSourceOriginal original;
        original.asset_version_id = "00000000-0000-4000-8000-00000000000" + std::to_string(index++);
        original.sha256 = sha;
        original.bytes = bytes;
        original.width = width;
        original.height = height;
        original.frame_count = frames;
        original.codec = static_cast<OriginalSourceCodecId>(codec);
        std::filesystem::copy_file(path, root + "/" + key + "/original." + original.asset_version_id);
        std::unique_ptr<OriginalSourceStream> stream;
        Expect(media.Open(key, original, &stream) == OriginalSourceStatus::kOk && stream, "open " + path);
        unsigned decoded = 0;
        OriginalSourceFrame frame;
        OriginalSourceStatus status = OriginalSourceStatus::kOk;
        while (stream && (status = stream->Next(&frame)) == OriginalSourceStatus::kOk) {
            Expect(frame.width == width && frame.height == height, "frame size " + path);
            ++decoded;
        }
        Expect(status == OriginalSourceStatus::kEnd && decoded == frames, "decodes every pinned frame " + path);
        ++codecs_seen[codec];
        // A different pinned digest is an integrity failure, not a substitute.
        OriginalSourceOriginal wrong = original;
        std::reverse(wrong.sha256.begin(), wrong.sha256.end());
        std::unique_ptr<OriginalSourceStream> refused;
        Expect(media.Open(key, wrong, &refused) == OriginalSourceStatus::kIntegrity && !refused,
               "digest mismatch refused " + path);
        // Another pack does not hold it.
        Expect(media.Open("farm-original/v2-" + std::string(64, 'a'), original, &refused) == OriginalSourceStatus::kIo,
               "absent from another pack " + path);
    }
    Expect(codecs_seen[0] > 0 && codecs_seen[1] > 0 && codecs_seen[2] > 0, "H.264, VP9 alpha and PNG covered");
    lease_available = false;
    {
        OriginalSourceOriginal any;
        any.asset_version_id = "00000000-0000-4000-8000-000000000000";
        any.sha256 = std::string(64, 'a');
        any.bytes = 1;
        any.width = any.height = any.frame_count = 1;
        std::unique_ptr<OriginalSourceStream> refused;
        Expect(media.Open(key, any, &refused) == OriginalSourceStatus::kLeaseUnavailable, "open needs a read lease");
    }
    Expect(allocator.Unbind(), "every decoder buffer released");
    coordinator.EndLessonSession("assignment-pack", "session-pack", begun.generation);
    if (failures != 0) {
        std::fprintf(stderr, "%d failures of %d checks\n", failures, checks);
        return 1;
    }
    std::printf("PASS original-source pack media: %d checks, %d originals\n", checks, index);
    return 0;
}
