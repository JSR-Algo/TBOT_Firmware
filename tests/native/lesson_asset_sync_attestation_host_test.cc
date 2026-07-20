#include <cJSON.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "lesson_asset_sync_attestation.h"

namespace {

int checks = 0;

void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void ExpectResult(
    const char* cache_key,
    const char* manifest_checksum,
    int asset_count,
    int verified_count,
    int failed_count,
    bool expected_ready,
    const char* expected_checksum
) {
    cJSON* response = cJSON_CreateObject();
    AddLessonAssetSyncAttestation(
        response,
        cache_key,
        manifest_checksum,
        asset_count,
        verified_count,
        failed_count
    );

    const cJSON* ready = cJSON_GetObjectItem(response, "ready");
    const cJSON* checksum = cJSON_GetObjectItem(response, "manifestChecksum");
    Expect(cJSON_IsBool(ready), "ready must be a JSON boolean");
    Expect(cJSON_IsTrue(ready) == expected_ready, "ready value mismatch");
    if (expected_checksum == nullptr) {
        Expect(checksum == nullptr, "invalid pack must omit manifestChecksum");
    } else {
        Expect(cJSON_IsString(checksum), "verified pack must include manifestChecksum");
        Expect(std::string(checksum->valuestring) == expected_checksum,
               "manifestChecksum must exactly echo the request");
    }
    cJSON_Delete(response);
}

}  // namespace

int main() {
    const std::string checksum(64, 'a');
    const std::string cache_key = "pip-farm/v1-" + checksum;

    ExpectResult(cache_key.c_str(), checksum.c_str(), 2, 2, 0, true, checksum.c_str());
    ExpectResult(cache_key.c_str(), nullptr, 2, 2, 0, false, nullptr);
    ExpectResult(cache_key.c_str(), (" " + checksum).c_str(), 2, 2, 0, false, nullptr);
    ExpectResult("pip-farm/v1-bbbbbbbb", checksum.c_str(), 2, 2, 0, false, nullptr);
    ExpectResult(cache_key.c_str(), checksum.c_str(), 2, 1, 1, false, nullptr);
    ExpectResult(cache_key.c_str(), checksum.c_str(), 0, 0, 0, false, nullptr);
    ExpectResult(cache_key.c_str(), checksum.c_str(), 2, 1, 0, false, nullptr);

    std::cout << "lesson asset sync attestation host test OK (" << checks << " checks)"
              << std::endl;
    return 0;
}
