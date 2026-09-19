#include "mcp_server.h"
#include "lesson_asset_retained_parser.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_asset_pack_activation.h"
#include "lesson_asset_cache_evict.h"
#include "lesson_asset_sync_path_policy.h"
#include "lesson_asset_sync_attestation.h"
#include "lesson_asset_http_transfer.h"
#include "lesson_trgb_size_policy.h"
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#define ESP_LOGW(...) ((void)0)

static std::map<std::string, std::function<ReturnValue(const PropertyList&)>> callbacks;
static int downloads = 0, checks = 0;
static bool fail_download = false;
static const std::string bytes = "owned native transfer fixture";
static std::map<std::string, std::string> media_files;
static std::string Hash(const std::string& value) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(value.data(), static_cast<CC_LONG>(value.size()), hash);
    std::ostringstream out;
    for (const auto byte : hash) out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    return out.str();
}
static std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), {});
}
static bool VerifyLessonAssetSha256(const std::string& path, const std::string& hash) {
    return std::filesystem::is_regular_file(path) && Hash(Read(path)) == hash;
}
static void DownloadLessonAssetToVerifiedFile(const LessonAssetMutationLease& lease,
    const char*, bool, size_t, const char*, const std::string& url, const std::string& path,
    const std::string& hash, size_t& count) {
    if (!lease) throw std::runtime_error("mutation required");
    ++downloads;
    if (fail_download) throw std::runtime_error("owned injected missing/full SD failure");
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    const auto content = media_files.empty() ? bytes : Read(media_files.at(url));
    std::ofstream(path, std::ios::binary) << content;
    if (!VerifyLessonAssetSha256(path, hash)) throw std::runtime_error("wrong fixture hash");
    count = content.size();
}
static void CopyVerifiedLessonAssetFile(const LessonAssetMutationLease& lease, const char* key,
    const std::string& source, const std::string& path, const std::string& hash, size_t& count) {
    if (!lease) throw std::runtime_error("mutation required");
    const auto content = Read(source);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << content;
    if (!VerifyLessonAssetSha256(path, hash)) throw std::runtime_error("reuse hash mismatch");
    count = content.size();
}
[[noreturn]] static void ThrowLessonAssetMutationRefusal(LessonAssetReservationCode) {
    throw std::runtime_error("mutation refused");
}
static void AddUserOnlyTool(const std::string& name, const std::string&, const PropertyList&,
                           std::function<ReturnValue(const PropertyList&)> callback) {
    callbacks.emplace(name, callback);
}
#include "retained_mcp_handlers.inc"

static void Expect(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("MCP retained check " + std::to_string(checks));
}
static CheckedCJsonPtr Call(const std::string& tool, const char* field, const cJSON* value) {
    PropertyList properties;
    if (field) {
        Property property(field, kPropertyTypeObject);
        char* raw = cJSON_PrintUnformatted(value);
        property.set_value(std::string(raw)); cJSON_free(raw);
        properties.AddProperty(property);
    }
    return CheckedCJsonPtr(std::get<cJSON*>(callbacks.at(tool)(properties)));
}
template<class Fn> static void Refuses(Fn fn) {
    bool refused = false;
    try { fn(); } catch (const std::runtime_error&) { refused = true; }
    Expect(refused);
}
int main(int argc, char** argv) {
    Register();
    if (argc == 6) {
        CheckedCJsonPtr bind(cJSON_Parse(Read(argv[1]).c_str()));
        CheckedCJsonPtr pack(cJSON_Parse(Read(argv[2]).c_str()));
        CheckedCJsonPtr index(cJSON_Parse(Read(argv[3]).c_str()));
        const cJSON* entry;
        cJSON_ArrayForEach(entry, index.get()) media_files.emplace(entry->string, entry->valuestring);
        const auto owner = ParseRetainedDeviceOperation(bind.get()).owner;
        const std::string root = std::string(kLessonAssetPackRoot) + owner.cache_key;
        cJSON_ReplaceItemInObject(pack.get(), "localRoot", cJSON_CreateString(root.c_str()));
        const cJSON* asset;
        cJSON_ArrayForEach(asset, cJSON_GetObjectItem(pack.get(), "assets")) {
            const std::string wire = JsonStringField(asset, "sdPath");
            const std::string prefix = "/sdcard/tbot/lesson-assets/";
            Expect(wire.rfind(prefix, 0) == 0);
            const std::string physical = std::string(kLessonAssetPackRoot) + wire.substr(prefix.size());
            cJSON_ReplaceItemInObject(const_cast<cJSON*>(asset), "sdPath", cJSON_CreateString(physical.c_str()));
        }
        Call("self.lesson_assets.retained_selection", "operation", bind.get());
        const auto result = Call("self.lesson_assets.sync_to_sd", "assetPack", pack.get());
        Expect(cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "ready")) &&
               cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "activated")));
        const auto receipt = cJSON_GetObjectItem(result.get(), "retainedSelection");
        Expect(receipt != nullptr);
        char* encoded = cJSON_PrintUnformatted(receipt);
        std::ofstream(argv[4]) << encoded; cJSON_free(encoded);
        encoded = cJSON_PrintUnformatted(result.get());
        std::ofstream(argv[5]) << encoded; cJSON_free(encoded);
        std::cout << "canonical media through actual native bind/sync callbacks and active pointer; assets="
                  << cJSON_GetArraySize(cJSON_GetObjectItem(pack.get(), "assets")) << "\n";
        return 0;
    }
    if (argc != 2) return 2;
    auto vectors = CheckedCJsonPtr(cJSON_Parse(Read(argv[1]).c_str()));
    const cJSON* bind = nullptr;
    const cJSON* item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(vectors.get(), "valid")) {
        const auto op = cJSON_GetObjectItem(item, "operation");
        if (std::string(JsonStringField(op, "action")) == "bind") bind = op;
    }
    Expect(bind != nullptr);
    const auto operation = ParseRetainedDeviceOperation(bind);
    const auto& owner = operation.owner;
    auto pack = MakeCheckedCJsonObject();
    cJSON_AddStringToObject(pack.get(), "cacheKey", owner.cache_key.c_str());
    cJSON_AddStringToObject(pack.get(), "lessonId", owner.lesson_key.c_str());
    cJSON_AddStringToObject(pack.get(), "manifestChecksum", owner.manifest_checksum.c_str());
    cJSON_AddStringToObject(pack.get(), "localRoot", (std::string(kLessonAssetPackRoot) + owner.cache_key).c_str());
    auto assets = cJSON_AddArrayToObject(pack.get(), "assets");
    auto asset = cJSON_CreateObject(); cJSON_AddItemToArray(assets, asset);
    const std::string path = std::string(kLessonAssetPackRoot) + owner.cache_key + "/fixture.png";
    for (const auto& pair : std::map<std::string, std::string>{{"key", "fixture.png"}, {"path", "fixture.png"},
        {"url", "https://cdn.example.com/fixture.png"}, {"sha256", Hash(bytes)}, {"mediaType", "image/png"}, {"localPath", path}})
        cJSON_AddStringToObject(asset, pair.first.c_str(), pair.second.c_str());
    cJSON_AddNumberToObject(asset, "size", bytes.size()); cJSON_AddBoolToObject(asset, "critical", true);
    Call("self.lesson_assets.retained_selection", "operation", bind);
    Call("self.lesson_assets.retained_selection", "operation", bind);
    Refuses([&] { Call("self.lesson_assets.sync_to_sd", "assetPack", pack.get()); });
    Expect(downloads == 0);
    cJSON_AddNumberToObject(pack.get(), "selectionRevision", owner.selection_revision);
    cJSON_AddItemToObject(pack.get(), "retainedSelection", cJSON_Duplicate(bind, true));
    fail_download = true;
    auto failed = Call("self.lesson_assets.sync_to_sd", "assetPack", pack.get());
    Expect(!cJSON_IsTrue(cJSON_GetObjectItem(failed.get(), "ready")) &&
           !cJSON_GetObjectItem(failed.get(), "retainedSelection") && ReadRetainedSelection().active);
    fail_download = false;
    auto ready = Call("self.lesson_assets.sync_to_sd", "assetPack", pack.get());
    Expect(cJSON_IsTrue(cJSON_GetObjectItem(ready.get(), "ready")) &&
           cJSON_IsTrue(cJSON_GetObjectItem(ready.get(), "activated")));
    Expect(std::string(JsonStringField(cJSON_GetObjectItem(ready.get(), "retainedSelection"), "assignmentId")) == owner.assignment_id);
    auto replay = Call("self.lesson_assets.sync_to_sd", "assetPack", pack.get());
    Expect(cJSON_GetObjectItem(replay.get(), "skippedCount")->valueint == 1);
    Expect(Read(path) == bytes && !EvictLessonAssetCacheKey(owner.cache_key, false).evicted);
    auto release = CheckedCJsonPtr(cJSON_Duplicate(bind, true));
    cJSON_ReplaceItemInObject(release.get(), "action", cJSON_CreateString("release"));
    cJSON_ReplaceItemInObject(release.get(), "requestRevision", cJSON_CreateNumber(owner.request_revision + 1));
    cJSON_ReplaceItemInObject(release.get(), "desiredSelectionRevision", cJSON_CreateNumber(owner.selection_revision + 1));
    cJSON_DeleteItemFromObject(release.get(), "assignmentId"); cJSON_DeleteItemFromObject(release.get(), "assignmentVersion");
    Call("self.lesson_assets.retained_selection", "operation", release.get());
    Refuses([&] { Call("self.lesson_assets.sync_to_sd", "assetPack", pack.get()); });
    Refuses([&] { Call("self.lesson_assets.retained_selection", "operation", bind); });
    Expect(!EvictLessonAssetCacheKey(owner.cache_key, false).evicted && Read(path) == bytes);
    std::cout << "actual MCP handlers/validators OK (" << checks << " checks); injected transfer and SD faults\n";
}
