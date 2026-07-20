#ifndef LESSON_ASSET_SYNC_ATTESTATION_H
#define LESSON_ASSET_SYNC_ATTESTATION_H

#include <cJSON.h>

void AddLessonAssetSyncAttestation(
    cJSON* response,
    const char* cache_key,
    const char* manifest_checksum,
    int asset_count,
    int verified_count,
    int failed_count
);

#endif  // LESSON_ASSET_SYNC_ATTESTATION_H
