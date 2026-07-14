#include "lesson_asset_sync_attestation.h"

#include <cctype>
#include <string>

namespace {

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool IsSha256Hex(const std::string& value) {
    if (value.size() != 64) return false;
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

}  // namespace

void AddLessonAssetSyncAttestation(
    cJSON* response,
    const char* cache_key,
    const char* manifest_checksum,
    int asset_count,
    int verified_count,
    int failed_count
) {
    const std::string manifest_checksum_value =
        manifest_checksum == nullptr ? "" : Trim(manifest_checksum);
    const std::string cache_key_value = cache_key == nullptr ? "" : Trim(cache_key);
    const bool manifest_checksum_valid =
        manifest_checksum != nullptr &&
        manifest_checksum_value == manifest_checksum &&
        IsSha256Hex(manifest_checksum_value);
    const bool cache_key_matches_manifest =
        manifest_checksum_valid && !cache_key_value.empty() &&
        cache_key_value.find(manifest_checksum_value) != std::string::npos;
    const bool pack_verified =
        asset_count > 0 && verified_count == asset_count && failed_count == 0 &&
        cache_key_matches_manifest;

    if (pack_verified) {
        cJSON_AddStringToObject(response, "manifestChecksum", manifest_checksum);
    }
    cJSON_AddBoolToObject(response, "ready", pack_verified);
}
