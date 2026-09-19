#include "lesson_asset_retained_parser.h"
#include "checked_cjson.h"
#include <cmath>
#include <set>
#include <stdexcept>

namespace {
[[noreturn]] void Invalid() { throw std::runtime_error("invalid_retained_device_operation"); }
const cJSON* Field(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }
std::string Text(const cJSON* object, const char* key) {
    const auto field = Field(object, key);
    if (!cJSON_IsString(field) || !field->valuestring) Invalid();
    return field->valuestring;
}
std::uint64_t Revision(const cJSON* value, bool zero = false) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < (zero ? 0 : 1) || value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble) Invalid();
    return static_cast<std::uint64_t>(value->valuedouble);
}
void Exact(const cJSON* object, std::set<std::string> fields) {
    if (!cJSON_IsObject(object)) Invalid();
    const cJSON* item;
    cJSON_ArrayForEach(item, object) {
        if (!item->string || fields.erase(item->string) != 1) Invalid();
    }
    if (!fields.empty()) Invalid();
}
}  // namespace

RetainedDeviceOperation ParseRetainedDeviceOperation(const cJSON* operation) {
    const auto action = Text(operation, "action");
    if (action != "bind" && action != "release") Invalid();
    std::set<std::string> fields{"contractVersion", "action", "operationId", "requestId", "requestRevision",
        "deviceId", "consumerIdentity", "desiredSelectionRevision", "selection"};
    if (action == "bind") { fields.insert("assignmentId"); fields.insert("assignmentVersion"); }
    Exact(operation, fields);
    if (Text(operation, "contractVersion") != "retained-assignment-pack.v1") Invalid();
    RetainedDeviceOperation result; result.release = action == "release";
    auto& o = result.owner;
    o.operation_id = Text(operation, "operationId"); o.request_id = Text(operation, "requestId");
    o.request_revision = Revision(Field(operation, "requestRevision")); o.device_id = Text(operation, "deviceId");
    o.consumer_id = Text(operation, "consumerIdentity"); o.selection_revision = Revision(Field(operation, "desiredSelectionRevision"));
    const auto selection = Field(operation, "selection");
    Exact(selection, {"lessonRowId", "lessonKey", "lessonVersion", "profile", "manifestVersion", "manifestChecksum", "cacheKey", "packDescriptorChecksum"});
    if (Text(selection, "profile") != "espTft") Invalid();
    o.lesson_row_id = Text(selection, "lessonRowId"); o.lesson_key = Text(selection, "lessonKey");
    o.lesson_version = Revision(Field(selection, "lessonVersion")); o.manifest_version = Text(selection, "manifestVersion");
    o.manifest_checksum = Text(selection, "manifestChecksum"); o.cache_key = Text(selection, "cacheKey");
    o.descriptor_checksum = Text(selection, "packDescriptorChecksum");
    if (!result.release) {
        o.assignment_id = Text(operation, "assignmentId"); o.assignment_version = Revision(Field(operation, "assignmentVersion"));
    }
    ValidateRetainedSelectionOwner(o);
    return result;
}
std::uint64_t ParseRetainedSyncRevision(const cJSON* pack) {
    const auto revision = Field(pack, "selectionRevision");
    return revision ? Revision(revision, true) : 0;
}
void AddRetainedDeviceReceipt(cJSON* response, const RetainedDeviceOperation& operation) {
    const auto& o = operation.owner;
    CheckedCJsonAddStringToObject(response, "contractVersion", "retained-assignment-device.v1");
    CheckedCJsonAddStringToObject(response, "operationId", o.operation_id.c_str());
    CheckedCJsonAddStringToObject(response, "requestId", o.request_id.c_str());
    CheckedCJsonAddNumberToObject(response, "requestRevision", static_cast<double>(o.request_revision));
    CheckedCJsonAddStringToObject(response, "deviceId", o.device_id.c_str());
    CheckedCJsonAddStringToObject(response, "consumerIdentity", o.consumer_id.c_str());
    CheckedCJsonAddNumberToObject(response, "desiredSelectionRevision", static_cast<double>(o.selection_revision));
    CheckedCJsonAddStringToObject(response, "cacheKey", o.cache_key.c_str());
    CheckedCJsonAddStringToObject(response, "packDescriptorChecksum", o.descriptor_checksum.c_str());
    CheckedCJsonAddStringToObject(response, "state", operation.release ? "released" : "bound");
    if (!operation.release) {
        CheckedCJsonAddStringToObject(response, "assignmentId", o.assignment_id.c_str());
        CheckedCJsonAddNumberToObject(response, "assignmentVersion", static_cast<double>(o.assignment_version));
    }
}
void AddRetainedDeviceState(cJSON* response, const RetainedSelectionState& state) {
    if (state.revision) { AddRetainedDeviceReceipt(response, {state.owner, !state.active}); return; }
    CheckedCJsonAddStringToObject(response, "contractVersion", "retained-assignment-device.v1");
    CheckedCJsonAddStringToObject(response, "state", "unowned");
    CheckedCJsonAddNumberToObject(response, "desiredSelectionRevision", 0);
}
