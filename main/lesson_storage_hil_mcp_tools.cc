#include "lesson_storage_hil_mcp_tools.h"

#include "lesson_asset_cache_evict.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_storage_hil_controller.h"
#include "lesson_storage_hil_fixture.h"
#include "mcp_server.h"
#include "physical_sd_identity.h"
#include "sd_fat_session_guard.h"

#include <esp_log.h>

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>

#define TAG "LessonStorageHilMcp"

namespace {

constexpr char kArmTool[] = "self.lesson_assets.hil.arm_fault";
constexpr char kStatusTool[] = "self.lesson_assets.hil.status";
constexpr char kStageTool[] = "self.lesson_assets.hil.stage_fixture";
constexpr char kCleanupTool[] = "self.lesson_assets.hil.cleanup_fixture";
constexpr char kInspectTool[] = "self.lesson_assets.hil.inspect";
constexpr char kInvalidRequest[] = "lesson storage HIL request invalid";
constexpr std::size_t kPreparedPayloadBytes = 4096;
constexpr std::size_t kPreparedResultBytes = 12288;
constexpr std::size_t kInspectionPayloadBytes = 49152;
constexpr std::size_t kInspectionResultBytes = 65536;
constexpr int kMaxThreshold = 512 * 1024;
constexpr int kMaxPauseSeconds = 60;

bool HasHilPrefix(const std::string& cache_key) {
    return cache_key.compare(0, 4, "hil-") == 0 &&
           IsCanonicalLessonCacheKey(cache_key);
}

const cJSON* UniqueField(const cJSON* object, const char* name) {
    const cJSON* match = nullptr;
    const cJSON* field = nullptr;
    cJSON_ArrayForEach(field, object) {
        if (field->string != nullptr && std::strcmp(field->string, name) == 0) {
            if (match != nullptr) {
                return nullptr;  // duplicate
            }
            match = field;
        }
    }
    return match;
}

bool HasExactFields(
    const cJSON* object,
    std::initializer_list<const char*> allowed,
    std::initializer_list<const char*> required
) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    std::array<unsigned char, 8> seen{};
    if (allowed.size() > seen.size()) {
        return false;
    }
    const cJSON* field = nullptr;
    cJSON_ArrayForEach(field, object) {
        if (field->string == nullptr) {
            return false;
        }
        std::size_t index = 0;
        bool known = false;
        for (const char* name : allowed) {
            if (std::strcmp(field->string, name) == 0) {
                known = true;
                if (++seen[index] != 1) {
                    return false;  // duplicate
                }
                break;
            }
            ++index;
        }
        if (!known) {
            return false;  // unknown
        }
    }
    for (const char* name : required) {
        if (UniqueField(object, name) == nullptr) {
            return false;
        }
    }
    return true;
}

bool ExactString(const cJSON* object, const char* name, std::string* output = nullptr) {
    const cJSON* value = UniqueField(object, name);
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return false;
    }
    if (output != nullptr) {
        *output = value->valuestring;
    }
    return true;
}

bool ExactInteger(
    const cJSON* object,
    const char* name,
    int minimum,
    int maximum,
    bool optional
) {
    const cJSON* value = UniqueField(object, name);
    if (value == nullptr) {
        return optional;
    }
    return cJSON_IsNumber(value) &&
           value->valuedouble == static_cast<double>(value->valueint) &&
           value->valueint >= minimum && value->valueint <= maximum;
}

bool ParseOperation(const std::string& text, LessonStorageHilOperation* output) {
    if (text == "evict") {
        *output = LessonStorageHilOperation::kEvict;
        return true;
    }
    if (text == "sync") {
        *output = LessonStorageHilOperation::kSync;
        return true;
    }
    return false;
}

bool ParseCheckpoint(const std::string& text, LessonStorageHilCheckpoint* output) {
    static constexpr std::pair<const char*, LessonStorageHilCheckpoint> kValues[] = {
        {"before_first_unlink", LessonStorageHilCheckpoint::kBeforeFirstUnlink},
        {"after_unlinks", LessonStorageHilCheckpoint::kAfterUnlinks},
        {"before_rmdir", LessonStorageHilCheckpoint::kBeforeRmdir},
        {"before_download_write", LessonStorageHilCheckpoint::kBeforeDownloadWrite},
        {"after_download_bytes", LessonStorageHilCheckpoint::kAfterDownloadBytes},
        {"before_checksum_verify", LessonStorageHilCheckpoint::kBeforeChecksumVerify},
        {"before_commit_rename", LessonStorageHilCheckpoint::kBeforeCommitRename},
    };
    for (const auto& value : kValues) {
        if (text == value.first) {
            *output = value.second;
            return true;
        }
    }
    return false;
}

bool ParseAction(const std::string& text, LessonStorageHilAction* output) {
    static constexpr std::pair<const char*, LessonStorageHilAction> kValues[] = {
        {"fail", LessonStorageHilAction::kFail},
        {"pause", LessonStorageHilAction::kPause},
        {"no_space", LessonStorageHilAction::kNoSpace},
        {"corrupt_staging", LessonStorageHilAction::kCorruptStaging},
    };
    for (const auto& value : kValues) {
        if (text == value.first) {
            *output = value.second;
            return true;
        }
    }
    return false;
}

bool ParseFixture(const std::string& text, LessonStorageHilFixture* output) {
    if (text == "nested_directory") {
        *output = LessonStorageHilFixture::kNestedDirectory;
        return true;
    }
    if (text == "leaf_regular_file") {
        *output = LessonStorageHilFixture::kLeafRegularFile;
        return true;
    }
    if (text == "preservation_set") {
        *output = LessonStorageHilFixture::kPreservationSet;
        return true;
    }
    return false;
}

bool Compatible(
    LessonStorageHilOperation operation,
    LessonStorageHilCheckpoint checkpoint,
    LessonStorageHilAction action,
    int threshold,
    int declared_asset_bytes,
    int pause_seconds
) {
    const bool pause_ok = action == LessonStorageHilAction::kPause
        ? pause_seconds >= 5 && pause_seconds <= kMaxPauseSeconds
        : pause_seconds == 0;
    if (!pause_ok) {
        return false;
    }
    if (operation == LessonStorageHilOperation::kEvict) {
        if (checkpoint != LessonStorageHilCheckpoint::kBeforeFirstUnlink &&
            checkpoint != LessonStorageHilCheckpoint::kAfterUnlinks &&
            checkpoint != LessonStorageHilCheckpoint::kBeforeRmdir) {
            return false;
        }
        if (action != LessonStorageHilAction::kFail &&
            action != LessonStorageHilAction::kPause) {
            return false;
        }
        return checkpoint == LessonStorageHilCheckpoint::kAfterUnlinks
            ? threshold >= 1 && threshold <= 64 && declared_asset_bytes == 0
            : threshold == 0 && declared_asset_bytes == 0;
    }
    if (checkpoint == LessonStorageHilCheckpoint::kAfterDownloadBytes) {
        return (action == LessonStorageHilAction::kFail ||
                action == LessonStorageHilAction::kPause ||
                action == LessonStorageHilAction::kNoSpace) &&
               threshold >= 1 && declared_asset_bytes >= threshold &&
               declared_asset_bytes <= kMaxThreshold;
    }
    if (threshold != 0 || declared_asset_bytes != 0) {
        return false;
    }
    if (checkpoint == LessonStorageHilCheckpoint::kBeforeChecksumVerify) {
        return action == LessonStorageHilAction::kFail ||
               action == LessonStorageHilAction::kPause ||
               action == LessonStorageHilAction::kCorruptStaging;
    }
    if (checkpoint == LessonStorageHilCheckpoint::kBeforeDownloadWrite ||
        checkpoint == LessonStorageHilCheckpoint::kBeforeCommitRename) {
        return action == LessonStorageHilAction::kFail ||
               action == LessonStorageHilAction::kPause ||
               action == LessonStorageHilAction::kNoSpace;
    }
    return false;
}

bool ValidateArmRequest(const cJSON* arguments) {
    if (!HasExactFields(
            arguments,
            {"cacheKey", "operation", "checkpoint", "action", "threshold",
             "declaredAssetBytes", "pauseSeconds"},
            {"cacheKey", "operation", "checkpoint", "action"})) {
        return false;
    }
    std::string cache_key;
    std::string operation_text;
    std::string checkpoint_text;
    std::string action_text;
    if (!ExactString(arguments, "cacheKey", &cache_key) || !HasHilPrefix(cache_key) ||
        !ExactString(arguments, "operation", &operation_text) ||
        !ExactString(arguments, "checkpoint", &checkpoint_text) ||
        !ExactString(arguments, "action", &action_text) ||
        !ExactInteger(arguments, "threshold", 0, kMaxThreshold, true) ||
        !ExactInteger(arguments, "declaredAssetBytes", 0, kMaxThreshold, true) ||
        !ExactInteger(arguments, "pauseSeconds", 0, kMaxPauseSeconds, true)) {
        return false;
    }
    LessonStorageHilOperation operation = LessonStorageHilOperation::kEvict;
    LessonStorageHilCheckpoint checkpoint =
        LessonStorageHilCheckpoint::kBeforeFirstUnlink;
    LessonStorageHilAction action = LessonStorageHilAction::kFail;
    if (!ParseOperation(operation_text, &operation) ||
        !ParseCheckpoint(checkpoint_text, &checkpoint) ||
        !ParseAction(action_text, &action)) {
        return false;
    }
    const cJSON* threshold = UniqueField(arguments, "threshold");
    const cJSON* declared = UniqueField(arguments, "declaredAssetBytes");
    const cJSON* pause = UniqueField(arguments, "pauseSeconds");
    return Compatible(
        operation, checkpoint, action,
        threshold == nullptr ? 0 : threshold->valueint,
        declared == nullptr ? 0 : declared->valueint,
        pause == nullptr ? 0 : pause->valueint);
}

bool ValidateFixtureRequest(const cJSON* arguments, bool inspection) {
    if (!HasExactFields(
            arguments,
            inspection ? std::initializer_list<const char*>{
                             "cacheKey", "siblingCacheKey", "schemaVersion"}
                       : std::initializer_list<const char*>{"cacheKey", "fixture", "siblingCacheKey"},
            inspection ? std::initializer_list<const char*>{"cacheKey"}
                       : std::initializer_list<const char*>{"cacheKey", "fixture"})) {
        return false;
    }
    std::string cache_key;
    std::string sibling;
    if (!ExactString(arguments, "cacheKey", &cache_key) || !HasHilPrefix(cache_key) ||
        (inspection && !ExactInteger(arguments, "schemaVersion", 1, 2, true))) {
        return false;
    }
    const cJSON* sibling_field = UniqueField(arguments, "siblingCacheKey");
    if (sibling_field != nullptr &&
        (!cJSON_IsString(sibling_field) || sibling_field->valuestring == nullptr)) {
        return false;
    }
    sibling = sibling_field == nullptr ? "" : sibling_field->valuestring;
    if (inspection) {
        return sibling.empty() || HasHilPrefix(sibling);
    }
    std::string fixture_text;
    LessonStorageHilFixture fixture;
    if (!ExactString(arguments, "fixture", &fixture_text) ||
        !ParseFixture(fixture_text, &fixture)) {
        return false;
    }
    if (fixture != LessonStorageHilFixture::kPreservationSet) {
        return sibling.empty();
    }
    if (!HasHilPrefix(sibling) || sibling == cache_key) {
        return false;
    }
    const std::size_t primary_slash = cache_key.find('/');
    const std::size_t sibling_slash = sibling.find('/');
    if (primary_slash == std::string::npos || sibling_slash == std::string::npos ||
        cache_key.substr(0, primary_slash) != sibling.substr(0, sibling_slash)) {
        return false;
    }
    const std::size_t primary_version_end = cache_key.find('-', primary_slash + 2);
    const std::size_t sibling_version_end = sibling.find('-', sibling_slash + 2);
    return primary_version_end != std::string::npos &&
           sibling_version_end != std::string::npos &&
           cache_key.substr(primary_slash + 2,
                            primary_version_end - primary_slash - 2) !=
               sibling.substr(sibling_slash + 2,
                              sibling_version_end - sibling_slash - 2);
}

const char* OperationName(LessonStorageHilOperation value) {
    return value == LessonStorageHilOperation::kSync ? "sync" : "evict";
}

const char* CheckpointName(LessonStorageHilCheckpoint value) {
    switch (value) {
        case LessonStorageHilCheckpoint::kBeforeFirstUnlink: return "before_first_unlink";
        case LessonStorageHilCheckpoint::kAfterUnlinks: return "after_unlinks";
        case LessonStorageHilCheckpoint::kBeforeRmdir: return "before_rmdir";
        case LessonStorageHilCheckpoint::kBeforeDownloadWrite: return "before_download_write";
        case LessonStorageHilCheckpoint::kAfterDownloadBytes: return "after_download_bytes";
        case LessonStorageHilCheckpoint::kBeforeChecksumVerify: return "before_checksum_verify";
        case LessonStorageHilCheckpoint::kBeforeCommitRename: return "before_commit_rename";
    }
    return "before_first_unlink";
}

const char* ActionName(LessonStorageHilAction value) {
    switch (value) {
        case LessonStorageHilAction::kFail: return "fail";
        case LessonStorageHilAction::kPause: return "pause";
        case LessonStorageHilAction::kNoSpace: return "no_space";
        case LessonStorageHilAction::kCorruptStaging: return "corrupt_staging";
    }
    return "fail";
}

int SchemaVersion(const PropertyList& properties) {
    return properties["schemaVersion"].value<int>();
}

void AddPhysicalSdIdentity(
    cJSON* payload,
    const tbot::SdFatSessionLease& sd_session
) {
    auto& sd_guard = tbot::SdFatSessionGuard::GetInstance();
    const auto lifecycle = sd_guard.Snapshot(sd_session);
    const auto snapshot = tbot::PhysicalSdIdentityRegistry::GetInstance()
                              .RefreshAndSnapshot({
                                  lifecycle.present,
                                  lifecycle.mount_generation,
                                  lifecycle.volume.has_value()
                                      ? std::optional<tbot::FatVolumeIdentity>{
                                            {lifecycle.volume->serial,
                                             lifecycle.volume->label}}
                                      : std::nullopt,
                              });
    CheckedCJsonAddNumberToObject(payload, "schemaVersion", 2);
    auto storage = MakeCheckedCJsonObject();
    CheckedCJsonAddStringToObject(
        storage.get(), "status", tbot::PhysicalSdIdentityStatusName(snapshot.status));
    CheckedCJsonAddStringToObject(storage.get(), "kind", "sdmmc-fat");
    if (snapshot.status == tbot::PhysicalSdIdentityStatus::kAvailable &&
        snapshot.identity.has_value()) {
        const auto& value = *snapshot.identity;
        CheckedCJsonAddStringToObject(
            storage.get(), "cidFingerprint", value.cid_fingerprint.c_str());
        auto cid = MakeCheckedCJsonObject();
        CheckedCJsonAddNumberToObject(
            cid.get(), "manufacturerId", value.cid.manufacturer_id);
        CheckedCJsonAddNumberToObject(cid.get(), "oemId", value.cid.oem_id);
        CheckedCJsonAddStringToObject(
            cid.get(), "productName", value.cid.product_name.c_str());
        CheckedCJsonAddNumberToObject(cid.get(), "revision", value.cid.revision);
        CheckedCJsonAddNumberToObject(cid.get(), "serial", value.cid.serial);
        CheckedCJsonAddNumberToObject(
            cid.get(), "manufacturingDate", value.cid.manufacturing_date);
        CheckedCJsonAddItemToObject(storage.get(), "cid", std::move(cid));
        CheckedCJsonAddNumberToObject(
            storage.get(), "capacitySectors", value.capacity_sectors);
        CheckedCJsonAddNumberToObject(
            storage.get(), "sectorSizeBytes", value.sector_size_bytes);
        CheckedCJsonAddNumberToObject(
            storage.get(), "capacityBytes", value.capacity_bytes);
        CheckedCJsonAddNumberToObject(
            storage.get(), "mountGeneration", value.mount_generation);
        CheckedCJsonAddStringToObject(
            storage.get(), "volumeSerial", value.volume_serial.c_str());
        CheckedCJsonAddStringToObject(
            storage.get(), "volumeLabel", value.volume_label.c_str());
    }
    CheckedCJsonAddItemToObject(payload, "storageIdentity", std::move(storage));
}

void AddUnavailablePhysicalSdIdentity(cJSON* payload) {
    CheckedCJsonAddNumberToObject(payload, "schemaVersion", 2);
    auto storage = MakeCheckedCJsonObject();
    CheckedCJsonAddStringToObject(storage.get(), "status", "unavailable");
    CheckedCJsonAddStringToObject(storage.get(), "kind", "sdmmc-fat");
    CheckedCJsonAddItemToObject(payload, "storageIdentity", std::move(storage));
}

const char* FixtureCodeName(LessonStorageHilFixtureCode code) {
    switch (code) {
        case LessonStorageHilFixtureCode::kStaged: return "staged";
        case LessonStorageHilFixtureCode::kCleaned: return "cleaned";
        case LessonStorageHilFixtureCode::kInspected: return "inspected";
        case LessonStorageHilFixtureCode::kInvalidCacheKey: return "invalid_cache_key";
        case LessonStorageHilFixtureCode::kInvalidSibling: return "invalid_sibling";
        case LessonStorageHilFixtureCode::kLeaseRefused: return "lease_refused";
        case LessonStorageHilFixtureCode::kUnexpectedExistingNode: return "unexpected_existing_node";
        case LessonStorageHilFixtureCode::kSentinelMismatch: return "sentinel_mismatch";
        case LessonStorageHilFixtureCode::kIoFailed: return "io_failed";
    }
    return "io_failed";
}

const char* ArmCodeName(LessonStorageHilArmCode code) {
    switch (code) {
        case LessonStorageHilArmCode::kArmed: return "armed";
        case LessonStorageHilArmCode::kAlreadyArmed: return "already_armed";
        case LessonStorageHilArmCode::kInvalidCacheKey: return "invalid_cache_key";
        case LessonStorageHilArmCode::kInvalidCombination: return "invalid_combination";
        case LessonStorageHilArmCode::kInvalidThreshold: return "invalid_threshold";
        case LessonStorageHilArmCode::kSequenceExhausted: return "sequence_exhausted";
    }
    return "invalid_combination";
}

cJSON* AddReservedString(cJSON* object, const char* name, std::size_t capacity) {
    const std::string reserved(capacity - 1, 'X');
    CheckedCJsonAddStringToObject(object, name, reserved.c_str());
    return cJSON_GetObjectItem(object, name);
}

void SetReservedString(cJSON* node, const char* text, std::size_t capacity) {
    const std::size_t length = std::strlen(text);
    if (node == nullptr || node->valuestring == nullptr || length >= capacity) {
        ThrowCJsonAllocationFailure();
    }
    std::memcpy(node->valuestring, text, length + 1);
}

cJSON* AddNumber(cJSON* object, const char* name) {
    CheckedCJsonAddNumberToObject(object, name, 0);
    return cJSON_GetObjectItem(object, name);
}

cJSON* AddBool(cJSON* object, const char* name) {
    CheckedCJsonAddBoolToObject(object, name, false);
    return cJSON_GetObjectItem(object, name);
}

PreparedMcpTextResult SealLessonStorageHilResponse(
    CheckedCJsonPtr payload,
    std::size_t payload_bytes = kPreparedPayloadBytes,
    std::size_t result_bytes = kPreparedResultBytes
) {
    return PreparedMcpTextResult(std::move(payload), payload_bytes, result_bytes);
}

std::string CallArmFault(const PropertyList& properties) {
    const std::string cache_key = properties["cacheKey"].value<std::string>();
    const std::string operation_text = properties["operation"].value<std::string>();
    const std::string checkpoint_text = properties["checkpoint"].value<std::string>();
    const std::string action_text = properties["action"].value<std::string>();
    LessonStorageHilOperation operation = LessonStorageHilOperation::kEvict;
    LessonStorageHilCheckpoint checkpoint =
        LessonStorageHilCheckpoint::kBeforeFirstUnlink;
    LessonStorageHilAction action = LessonStorageHilAction::kFail;
    ParseOperation(operation_text, &operation);
    ParseCheckpoint(checkpoint_text, &checkpoint);
    ParseAction(action_text, &action);

    auto payload = MakeCheckedCJsonObject();
    cJSON* cache_key_node = AddReservedString(
        payload.get(), "cacheKey", kLessonAssetCacheKeyMaxBytes + 1);
    cJSON* status_node = AddReservedString(payload.get(), "status", 32);
    cJSON* operation_node = AddReservedString(payload.get(), "operation", 16);
    cJSON* checkpoint_node = AddReservedString(payload.get(), "checkpoint", 32);
    cJSON* action_node = AddReservedString(payload.get(), "action", 24);
    cJSON* threshold_node = AddNumber(payload.get(), "threshold");
    cJSON* declared_node = AddNumber(payload.get(), "declaredAssetBytes");
    cJSON* pause_node = AddNumber(payload.get(), "pauseSeconds");
    cJSON* sequence_node = AddNumber(payload.get(), "armSequence");
    PreparedMcpTextResult response = SealLessonStorageHilResponse(std::move(payload));

    const LessonStorageHilArmRequest request{
        cache_key, operation, checkpoint, action,
        static_cast<std::uint32_t>(properties["threshold"].value<int>()),
        static_cast<std::uint32_t>(properties["declaredAssetBytes"].value<int>()),
        static_cast<std::uint32_t>(properties["pauseSeconds"].value<int>()),
    };
    const LessonStorageHilArmResult result =
        LessonStorageHilController::GetInstance().Arm(request);
    const LessonStorageHilStatus status =
        LessonStorageHilController::GetInstance().Status();
    SetReservedString(cache_key_node, status.cache_key.data(),
                      kLessonAssetCacheKeyMaxBytes + 1);
    SetReservedString(status_node, ArmCodeName(result.code), 32);
    SetReservedString(operation_node, OperationName(status.operation), 16);
    SetReservedString(checkpoint_node, CheckpointName(status.checkpoint), 32);
    SetReservedString(action_node, ActionName(status.action), 24);
    cJSON_SetNumberValue(threshold_node, status.threshold);
    cJSON_SetNumberValue(declared_node, status.declared_asset_bytes);
    cJSON_SetNumberValue(pause_node, status.pause_seconds);
    cJSON_SetNumberValue(sequence_node, result.arm_sequence);
    [[maybe_unused]] const std::uint64_t evidence_sequence =
        result.code == LessonStorageHilArmCode::kArmed
            ? result.arm_sequence
            : LessonStorageHilController::GetInstance().NextEvidenceSequence();
    ESP_LOGW(TAG,
             "HIL_STORAGE_ARM status=%s arm_sequence=%" PRIu64
             " sequence=%" PRIu64,
             ArmCodeName(result.code),
             result.arm_sequence, evidence_sequence);
    return response.Finish();
}

std::string CallStatusForSchema(int schema_version) {
    const LessonStorageHilStatus status =
        LessonStorageHilController::GetInstance().Status();
    auto payload = MakeCheckedCJsonObject();
    const char* state = status.armed ? "armed" :
        (status.consumed ? "consumed" : (status.reached ? "reached" : "idle"));
    CheckedCJsonAddStringToObject(payload.get(), "status", state);
    CheckedCJsonAddStringToObject(payload.get(), "cacheKey", status.cache_key.data());
    CheckedCJsonAddBoolToObject(payload.get(), "armed", status.armed);
    CheckedCJsonAddBoolToObject(payload.get(), "reached", status.reached);
    CheckedCJsonAddBoolToObject(payload.get(), "consumed", status.consumed);
    CheckedCJsonAddStringToObject(payload.get(), "operation", OperationName(status.operation));
    CheckedCJsonAddStringToObject(payload.get(), "checkpoint", CheckpointName(status.checkpoint));
    CheckedCJsonAddStringToObject(payload.get(), "action", ActionName(status.action));
    CheckedCJsonAddNumberToObject(payload.get(), "threshold", status.threshold);
    CheckedCJsonAddNumberToObject(payload.get(), "declaredAssetBytes", status.declared_asset_bytes);
    CheckedCJsonAddNumberToObject(payload.get(), "pauseSeconds", status.pause_seconds);
    CheckedCJsonAddNumberToObject(payload.get(), "armSequence", status.arm_sequence);
    CheckedCJsonAddNumberToObject(payload.get(), "reachedSequence", status.reached_sequence);
    CheckedCJsonAddNumberToObject(payload.get(), "consumedSequence", status.consumed_sequence);
    if (schema_version == 2) {
        auto sd_session = tbot::SdFatSessionGuard::GetInstance().TryAcquire();
        if (sd_session) {
            AddPhysicalSdIdentity(payload.get(), sd_session);
        } else {
            AddUnavailablePhysicalSdIdentity(payload.get());
        }
    }
    PreparedMcpTextResult response = SealLessonStorageHilResponse(std::move(payload));
    return response.Finish();
}

std::string CallStatus(const PropertyList& properties) {
    return CallStatusForSchema(SchemaVersion(properties));
}

std::string CallFixtureMutation(const PropertyList& properties, bool cleanup) {
    const std::string cache_key = properties["cacheKey"].value<std::string>();
    const std::string sibling = properties["siblingCacheKey"].value<std::string>();
    const std::string fixture_text = properties["fixture"].value<std::string>();
    LessonStorageHilFixture fixture;
    ParseFixture(fixture_text, &fixture);

    auto payload = MakeCheckedCJsonObject();
    CheckedCJsonAddStringToObject(payload.get(), "cacheKey", cache_key.c_str());
    CheckedCJsonAddStringToObject(payload.get(), "siblingCacheKey", sibling.c_str());
    CheckedCJsonAddStringToObject(payload.get(), "fixture", fixture_text.c_str());
    cJSON* status_node = AddReservedString(payload.get(), "status", 40);
    cJSON* changed_node = AddBool(payload.get(), "changed");
    PreparedMcpTextResult response = SealLessonStorageHilResponse(std::move(payload));

    auto lease = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation(
        cleanup ? "hil_fixture_cleanup" : "hil_fixture_stage");
    LessonStorageHilFixtureResult result{
        LessonStorageHilFixtureCode::kLeaseRefused, false, cache_key, sibling};
    if (lease) {
        result = cleanup
            ? CleanupLessonStorageHilFixture(lease, cache_key, fixture, sibling)
            : StageLessonStorageHilFixture(lease, cache_key, fixture, sibling);
    }
    SetReservedString(status_node, FixtureCodeName(result.code), 40);
    cJSON_SetBoolValue(changed_node, result.changed);
    [[maybe_unused]] const auto sequence = LessonStorageHilController::GetInstance()
                              .NextEvidenceSequence();
    ESP_LOGW(TAG,
             "HIL_STORAGE_FIXTURE operation=%s status=%s changed=%d sequence=%" PRIu64,
             cleanup ? "cleanup" : "stage", FixtureCodeName(result.code),
             result.changed ? 1 : 0, sequence);
    return response.Finish();
}

std::string CallFixtureStage(const PropertyList& properties) {
    return CallFixtureMutation(properties, false);
}

std::string CallFixtureCleanup(const PropertyList& properties) {
    return CallFixtureMutation(properties, true);
}

std::string CallInspect(const PropertyList& properties) {
    const std::string cache_key = properties["cacheKey"].value<std::string>();
    const std::string sibling = properties["siblingCacheKey"].value<std::string>();
    auto sd_session = tbot::SdFatSessionGuard::GetInstance().TryAcquire();
    if (!sd_session) {
        auto payload = MakeCheckedCJsonObject();
        CheckedCJsonAddStringToObject(payload.get(), "cacheKey", cache_key.c_str());
        CheckedCJsonAddStringToObject(payload.get(), "siblingCacheKey", sibling.c_str());
        CheckedCJsonAddStringToObject(payload.get(), "status", "lease_refused");
        CheckedCJsonAddBoolToObject(payload.get(), "truncated", false);
        CheckedCJsonAddItemToObject(payload.get(), "entries", MakeCheckedCJsonArray());
        if (SchemaVersion(properties) == 2) {
            AddUnavailablePhysicalSdIdentity(payload.get());
        }
        PreparedMcpTextResult response = SealLessonStorageHilResponse(
            std::move(payload), kInspectionPayloadBytes, kInspectionResultBytes);
        return response.Finish();
    }
    const LessonStorageHilInspection inspection =
        InspectLessonStorageHilStorage(cache_key, sibling);
    auto payload = MakeCheckedCJsonObject();
    CheckedCJsonAddStringToObject(payload.get(), "cacheKey", inspection.cache_key.c_str());
    CheckedCJsonAddStringToObject(payload.get(), "siblingCacheKey", inspection.sibling_cache_key.c_str());
    CheckedCJsonAddStringToObject(payload.get(), "status", "inspected");
    CheckedCJsonAddBoolToObject(payload.get(), "truncated", inspection.truncated);
    auto entries = MakeCheckedCJsonArray();
    for (const auto& entry : inspection.entries) {
        auto item = MakeCheckedCJsonObject();
        CheckedCJsonAddStringToObject(item.get(), "label", entry.label.c_str());
        CheckedCJsonAddStringToObject(item.get(), "nodeType", entry.node_type.c_str());
        CheckedCJsonAddNumberToObject(item.get(), "bytes", entry.bytes);
        CheckedCJsonAddStringToObject(item.get(), "sha256", entry.sha256.c_str());
        CheckedCJsonAddItemToArray(entries.get(), std::move(item));
    }
    CheckedCJsonAddItemToObject(payload.get(), "entries", std::move(entries));
    if (SchemaVersion(properties) == 2) {
        AddPhysicalSdIdentity(payload.get(), sd_session);
    }
    PreparedMcpTextResult response = SealLessonStorageHilResponse(
        std::move(payload), kInspectionPayloadBytes, kInspectionResultBytes);
    return response.Finish();
}

}  // namespace

#ifdef TBOT_LESSON_STORAGE_HIL_MCP_TOOLS_TESTING
std::string CallLessonStorageHilStatusForTest(int schema_version) {
    return CallStatusForSchema(schema_version);
}

std::string CallLessonStorageHilInspectForTest(
    int schema_version,
    const std::string& cache_key,
    const std::string& sibling_cache_key
) {
    return CallInspect(PropertyList({
        Property("cacheKey", kPropertyTypeString, cache_key),
        Property("siblingCacheKey", kPropertyTypeString, sibling_cache_key),
        Property("schemaVersion", kPropertyTypeInteger, schema_version, 1, 2),
    }));
}
#endif

bool IsExactLessonStorageHilToolName(const std::string& tool_name) {
    return tool_name == kArmTool || tool_name == kStatusTool ||
           tool_name == kStageTool || tool_name == kCleanupTool ||
           tool_name == kInspectTool;
}

const char* ValidateLessonStorageHilRawArguments(
    const std::string& tool_name,
    const cJSON* arguments
) {
    bool valid = false;
    if (tool_name == kArmTool) {
        valid = ValidateArmRequest(arguments);
    } else if (tool_name == kStatusTool) {
        valid = HasExactFields(arguments, {"schemaVersion"}, {}) &&
                ExactInteger(arguments, "schemaVersion", 1, 2, true);
    } else if (tool_name == kStageTool || tool_name == kCleanupTool) {
        valid = ValidateFixtureRequest(arguments, false);
    } else if (tool_name == kInspectTool) {
        valid = ValidateFixtureRequest(arguments, true);
    }
    return valid ? nullptr : kInvalidRequest;
}

void RegisterLessonStorageHilMcpTools(McpServer& server) {
    server.AddUserOnlyTool(kArmTool,
        "Arm one exact attended lesson-storage fault.",
        PropertyList({
            Property("cacheKey", kPropertyTypeString),
            Property("operation", kPropertyTypeString),
            Property("checkpoint", kPropertyTypeString),
            Property("action", kPropertyTypeString),
            Property("threshold", kPropertyTypeInteger, 0, 0, 524288),
            Property("declaredAssetBytes", kPropertyTypeInteger, 0, 0, 524288),
            Property("pauseSeconds", kPropertyTypeInteger, 0, 0, 60),
        }), PreparedMcpCall{}, CallArmFault);
    server.AddUserOnlyTool(kStatusTool,
        "Read the attended lesson-storage fault state.",
        PropertyList({
            Property("schemaVersion", kPropertyTypeInteger, 1, 1, 2),
        }), PreparedMcpCall{}, CallStatus);
    server.AddUserOnlyTool(kStageTool,
        "Stage one fixed lesson-storage HIL fixture.",
        PropertyList({
            Property("cacheKey", kPropertyTypeString),
            Property("fixture", kPropertyTypeString),
            Property("siblingCacheKey", kPropertyTypeString, std::string()),
        }), PreparedMcpCall{}, CallFixtureStage);
    server.AddUserOnlyTool(kCleanupTool,
        "Clean one exact fixed lesson-storage HIL fixture.",
        PropertyList({
            Property("cacheKey", kPropertyTypeString),
            Property("fixture", kPropertyTypeString),
            Property("siblingCacheKey", kPropertyTypeString, std::string()),
        }), PreparedMcpCall{}, CallFixtureCleanup);
    server.AddUserOnlyTool(kInspectTool,
        "Read bounded lesson-storage fingerprints.",
        PropertyList({
            Property("cacheKey", kPropertyTypeString),
            Property("siblingCacheKey", kPropertyTypeString, std::string()),
            Property("schemaVersion", kPropertyTypeInteger, 1, 1, 2),
        }), PreparedMcpCall{}, CallInspect);
}
