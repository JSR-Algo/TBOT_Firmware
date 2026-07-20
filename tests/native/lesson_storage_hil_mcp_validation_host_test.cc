#include <cJSON.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "lesson_asset_cache_evict.h"
#include "lesson_storage_hil_mcp_tools.h"

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string Key(const char* slug, int version, char checksum) {
    return std::string(slug) + "/v" + std::to_string(version) + "-" +
           std::string(kLessonAssetCacheChecksumHexBytes, checksum);
}

bool Valid(const char* tool, const std::string& json) {
    cJSON* arguments = cJSON_Parse(json.c_str());
    Expect(arguments != nullptr, "test JSON did not parse");
    const bool valid =
        ValidateLessonStorageHilRawArguments(tool, arguments) == nullptr;
    cJSON_Delete(arguments);
    return valid;
}

void TestExactNamesAndEmptyStatus() {
    Expect(IsExactLessonStorageHilToolName("self.lesson_assets.hil.status"),
           "exact status name rejected");
    Expect(!IsExactLessonStorageHilToolName("self.lesson_assets.hil.status.extra"),
           "prefix tool name accepted");
    Expect(Valid("self.lesson_assets.hil.status", "{}"), "empty status rejected");
    Expect(!Valid("self.lesson_assets.hil.status", "{\"extra\":1}"),
           "status accepted unknown field");
}

void TestStatusAndInspectSchemaVersionIsExact() {
    const std::string key = Key("hil-task14", 1, 'a');
    Expect(Valid("self.lesson_assets.hil.status", "{}"),
           "status omitted schema rejected");
    Expect(Valid("self.lesson_assets.hil.status", "{\"schemaVersion\":1}"),
           "status schema 1 rejected");
    Expect(Valid("self.lesson_assets.hil.status", "{\"schemaVersion\":2}"),
           "status schema 2 rejected");
    Expect(!Valid("self.lesson_assets.hil.status", "{\"schemaVersion\":1.5}"),
           "status fractional schema accepted");
    Expect(!Valid("self.lesson_assets.hil.status", "{\"schemaVersion\":true}"),
           "status bool schema accepted");
    Expect(!Valid("self.lesson_assets.hil.status", "{\"schemaVersion\":3}"),
           "status unknown schema accepted");
    Expect(!Valid("self.lesson_assets.hil.status",
                  "{\"schemaVersion\":1,\"schemaVersion\":2}"),
           "status duplicate schema accepted");

    const std::string inspect = "{\"cacheKey\":\"" + key + "\"";
    Expect(Valid("self.lesson_assets.hil.inspect", inspect + "}"),
           "inspect omitted schema rejected");
    Expect(Valid("self.lesson_assets.hil.inspect", inspect + ",\"schemaVersion\":1}"),
           "inspect schema 1 rejected");
    Expect(Valid("self.lesson_assets.hil.inspect", inspect + ",\"schemaVersion\":2}"),
           "inspect schema 2 rejected");
    Expect(!Valid("self.lesson_assets.hil.inspect", inspect + ",\"schemaVersion\":false}"),
           "inspect bool schema accepted");
}

void TestArmRejectsAmbiguousJsonBeforeConversion() {
    const std::string key = Key("hil-task14", 1, 'd');
    const std::string prefix = "{\"cacheKey\":\"" + key +
        "\",\"operation\":\"evict\",\"checkpoint\":\"after_unlinks\",";
    Expect(Valid("self.lesson_assets.hil.arm_fault",
                 prefix + "\"action\":\"fail\",\"threshold\":1}"),
           "valid arm rejected");
    Expect(!Valid("self.lesson_assets.hil.arm_fault",
                  prefix + "\"action\":\"fail\",\"threshold\":true}"),
           "bool threshold accepted");
    Expect(!Valid("self.lesson_assets.hil.arm_fault",
                  prefix + "\"action\":\"fail\",\"threshold\":1.5}"),
           "fractional threshold accepted");
    Expect(!Valid("self.lesson_assets.hil.arm_fault",
                  prefix + "\"action\":\"fail\",\"threshold\":0}"),
           "invalid checkpoint threshold accepted");
    Expect(!Valid("self.lesson_assets.hil.arm_fault",
                  prefix + "\"action\":\"erase\",\"threshold\":1}"),
           "unknown action accepted");
    Expect(!Valid("self.lesson_assets.hil.arm_fault",
                  prefix + "\"action\":\"fail\",\"threshold\":1,\"x\":0}"),
           "unknown arm field accepted");
    Expect(!Valid("self.lesson_assets.hil.arm_fault",
                  prefix + "\"action\":\"fail\",\"action\":\"pause\","
                           "\"threshold\":1}"),
           "duplicate arm field accepted");
}

void TestFixturePairsAreExact() {
    const std::string primary = Key("hil-task14", 1, 'a');
    const std::string sibling = Key("hil-task14", 2, 'b');
    const std::string same_version = Key("hil-task14", 1, 'c');
    const std::string foreign = Key("hil-other", 2, 'd');
    const std::string base = "{\"cacheKey\":\"" + primary +
        "\",\"fixture\":\"preservation_set\",\"siblingCacheKey\":\"";
    Expect(Valid("self.lesson_assets.hil.stage_fixture", base + sibling + "\"}"),
           "valid preservation pair rejected");
    Expect(!Valid("self.lesson_assets.hil.stage_fixture",
                  base + same_version + "\"}"),
           "same-version preservation pair accepted");
    Expect(!Valid("self.lesson_assets.hil.cleanup_fixture", base + foreign + "\"}"),
           "foreign-slug preservation pair accepted");
    Expect(!Valid("self.lesson_assets.hil.stage_fixture",
                  "{\"cacheKey\":\"" + primary +
                  "\",\"fixture\":\"nested_directory\","
                  "\"siblingCacheKey\":\"" + sibling + "\"}"),
           "single-key fixture accepted sibling");
    Expect(!Valid("self.lesson_assets.hil.stage_fixture",
                  base + sibling + "\",\"schemaVersion\":2}"),
           "stage fixture accepted schema version");
    Expect(!Valid("self.lesson_assets.hil.cleanup_fixture",
                  base + sibling + "\",\"schemaVersion\":2}"),
           "cleanup fixture accepted schema version");
}

}  // namespace

// The validation binary intentionally links only the raw validator. The full
// canonical-key implementation is covered by its own native suite.
bool IsCanonicalLessonCacheKey(const std::string& value) {
    const std::size_t slash = value.find("/v");
    const std::size_t dash = value.find('-', slash == std::string::npos ? 0 : slash + 2);
    if (slash == std::string::npos || dash == std::string::npos || slash == 0 ||
        value.size() > kLessonAssetCacheKeyMaxBytes ||
        value.size() - dash - 1 != kLessonAssetCacheChecksumHexBytes) {
        return false;
    }
    for (std::size_t index = dash + 1; index < value.size(); ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

int main() {
    TestExactNamesAndEmptyStatus();
    TestStatusAndInspectSchemaVersionIsExact();
    TestArmRejectsAmbiguousJsonBeforeConversion();
    TestFixturePairsAreExact();
    std::cout << "Lesson storage HIL MCP validation host tests passed\n";
    return 0;
}
