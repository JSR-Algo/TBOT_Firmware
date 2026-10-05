#include "lesson_original_source_pack_media.h"

#include "lesson_asset_sync_path_policy.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

namespace tbot {
namespace {

// The ESP's quote(key, safe='') and the firmware's pack path encoding.
std::string EncodeSegment(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : value) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                          ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4) & 0x0f]);
            encoded.push_back(kHex[ch & 0x0f]);
        }
    }
    return encoded;
}

bool DecodeSha256(const std::string& hex, std::uint8_t out[32]) {
    if (!IsExactLowerLessonAssetSha256(hex)) return false;
    for (int index = 0; index < 32; ++index) {
        const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        out[index] = static_cast<std::uint8_t>(nibble(hex[index * 2]) * 16 + nibble(hex[index * 2 + 1]));
    }
    return true;
}

class SessionStream final : public OriginalSourceStream {
public:
    OriginalSourceSession session;
    OriginalSourceStatus Next(OriginalSourceFrame* frame) override { return session.Next(frame); }
};

}  // namespace

std::string OriginalSourcePackPath(const std::string& root, const std::string& cache_key,
                                   const std::string& asset_key) {
    // Validate with the sync policy against its canonical root, then place it under `root`.
    const std::string canonical = std::string(kOriginalSourcePackRoot) + "/" + cache_key + "/" + EncodeSegment(asset_key);
    if (ValidateLessonAssetSyncPath(cache_key, canonical, asset_key).code != LessonAssetSyncPathCode::kValid) return {};
    return root + "/" + cache_key + "/" + EncodeSegment(asset_key);
}

OriginalSourceSceneLoader MakeOriginalSourcePackSceneLoader(std::string root, OriginalSourceLeaseSource lease) {
    return [root = std::move(root), lease = std::move(lease)](const std::string& cache_key, const std::string& sha256,
                                                              std::uint32_t bytes, std::string* json) -> const char* {
        std::uint8_t expected[32];
        if (!DecodeSha256(sha256, expected)) return "scene sha256 is not canonical";
        if (bytes == 0 || bytes > kOriginalSourceSceneMaxBytes) return "scene size is out of range";
        const std::string path = OriginalSourcePackPath(root, cache_key, kOriginalSourceSceneAssetKey);
        if (path.empty()) return "scene cache key is not canonical";
        LessonAssetReadLease held = lease ? lease() : LessonAssetReadLease{};
        if (!held) return "lesson read lease unavailable";
        std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
        if (!file) return "scene is not in the pack";
        std::string data(bytes, '\0');
        if (std::fread(&data[0], 1, bytes, file.get()) != bytes) return "scene is shorter than the reference";
        if (std::fgetc(file.get()) != EOF) return "scene is longer than the reference";
        // mbedTLS, not FFmpeg's hasher: routed FFmpeg allocations are refused outside a
        // decode session, and the scene is read before any session exists.
        std::uint8_t actual[32];
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
        if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), actual, 0) != 0) {
            return "scene hash unavailable";
        }
#else
        if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(data.data()), data.size(), actual, 0) != 0) {
            return "scene hash unavailable";
        }
#endif
        if (std::memcmp(actual, expected, sizeof(actual)) != 0) return "scene sha256 differs from the reference";
        *json = std::move(data);
        return nullptr;
    };
}

OriginalSourcePackMedia::OriginalSourcePackMedia(std::string root, OriginalSourceLeaseSource lease,
                                                 OriginalSourceAllocationState* allocations,
                                                 const std::atomic<bool>* cancelled)
    : root_(std::move(root)), lease_(std::move(lease)), allocations_(allocations), cancelled_(cancelled) {}

OriginalSourceStatus OriginalSourcePackMedia::Open(const std::string& cache_key, const OriginalSourceOriginal& original,
                                                   std::unique_ptr<OriginalSourceStream>* out) {
    OriginalSourceExpected expected{};
    if (!DecodeSha256(original.sha256, expected.sha256)) return OriginalSourceStatus::kInvalid;
    expected.bytes = original.bytes;
    expected.width = original.width;
    expected.height = original.height;
    expected.frames = original.frame_count;
    switch (original.codec) {
        case OriginalSourceCodecId::kH264: expected.codec = OriginalSourceCodec::kH264; break;
        case OriginalSourceCodecId::kVp9Alpha: expected.codec = OriginalSourceCodec::kVp9Alpha; break;
        case OriginalSourceCodecId::kPng: expected.codec = OriginalSourceCodec::kPng; break;
    }
    const std::string path = OriginalSourcePackPath(root_, cache_key, "original." + original.asset_version_id);
    if (path.empty()) return OriginalSourceStatus::kInvalid;
    auto stream = std::make_unique<SessionStream>();
    const OriginalSourceStatus status =
        stream->session.Open(path.c_str(), expected, lease_ ? lease_() : LessonAssetReadLease{}, allocations_, cancelled_);
    if (status != OriginalSourceStatus::kOk) return status;
    *out = std::move(stream);
    return OriginalSourceStatus::kOk;
}

}  // namespace tbot
