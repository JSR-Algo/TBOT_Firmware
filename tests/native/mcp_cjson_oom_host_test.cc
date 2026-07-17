#include <cJSON.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <string>
#include <unordered_set>

#include "checked_cjson.h"
#include "lesson_asset_cache_evict.h"
#include "lesson_storage_hil_mcp_tools.h"
#include "mcp_server.h"

bool g_fail_cpp_allocations = false;

void* operator new(std::size_t size) {
    if (g_fail_cpp_allocations) {
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

// The HIL raw-validator link intentionally supplies only the canonical-key
// boundary needed by lesson_storage_hil_mcp_tools.cc.
bool IsCanonicalLessonCacheKey(const std::string& value) {
    const std::size_t slash = value.find("/v");
    const std::size_t dash = value.find(
        '-', slash == std::string::npos ? 0 : slash + 2);
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

namespace {

constexpr const char* kStableError = "MCP response allocation failed";

struct AllocatorState {
    std::unordered_set<void*> live;
    bool fail_next = false;
    int fail_countdown = 0;
    std::size_t fail_size = 0;
    int fail_size_occurrence = 0;
};

AllocatorState* g_allocator = nullptr;

void* TestMalloc(std::size_t size) {
    if (g_allocator->fail_next) {
        g_allocator->fail_next = false;
        return nullptr;
    }
    if (g_allocator->fail_countdown > 0 && --g_allocator->fail_countdown == 0) {
        return nullptr;
    }
    if (g_allocator->fail_size == size && g_allocator->fail_size_occurrence > 0 &&
        --g_allocator->fail_size_occurrence == 0) {
        return nullptr;
    }
    void* pointer = std::malloc(size);
    if (pointer != nullptr) {
        g_allocator->live.insert(pointer);
    }
    return pointer;
}

void TestFree(void* pointer) {
    if (pointer != nullptr) {
        g_allocator->live.erase(pointer);
    }
    std::free(pointer);
}

class HookScope {
public:
    HookScope() {
        g_allocator = &state_;
        cJSON_Hooks hooks{TestMalloc, TestFree};
        cJSON_InitHooks(&hooks);
    }

    ~HookScope() {
        cJSON_InitHooks(nullptr);
        g_allocator = nullptr;
    }

    AllocatorState& state() { return state_; }

private:
    AllocatorState state_;
};

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpectStableOom(const std::function<void()>& operation, const char* stage) {
    bool threw = false;
    try {
        operation();
    } catch (const std::runtime_error& error) {
        threw = true;
        Expect(std::string(error.what()) == kStableError, stage);
    }
    Expect(threw, stage);
}

std::string HilKey(const char* slug, int version, char checksum) {
    return std::string(slug) + "/v" + std::to_string(version) + "-" +
           std::string(kLessonAssetCacheChecksumHexBytes, checksum);
}

bool ValidHilArguments(const char* tool, const std::string& json) {
    cJSON* arguments = cJSON_Parse(json.c_str());
    Expect(arguments != nullptr, "HIL validation JSON did not parse");
    const bool valid =
        ValidateLessonStorageHilRawArguments(tool, arguments) == nullptr;
    cJSON_Delete(arguments);
    return valid;
}

void TestHilInspectRejectsInvalidSiblingPairsBeforeFilesystemAccess() {
    const std::string primary = HilKey("hil-task14", 1, 'a');
    const std::string sibling = HilKey("hil-task14", 2, 'b');
    const std::string same_version = HilKey("hil-task14", 1, 'c');
    const std::string foreign = HilKey("hil-other", 2, 'd');
    const std::string prefix = "{\"cacheKey\":\"" + primary +
        "\",\"siblingCacheKey\":\"";

    Expect(ValidHilArguments("self.lesson_assets.hil.inspect",
                             prefix + sibling + "\"}"),
           "valid HIL inspection pair rejected");
    Expect(!ValidHilArguments("self.lesson_assets.hil.inspect",
                              prefix + primary + "\"}"),
           "identical HIL inspection pair accepted");
    Expect(!ValidHilArguments("self.lesson_assets.hil.inspect",
                              prefix + same_version + "\"}"),
           "same-version HIL inspection pair accepted");
    Expect(!ValidHilArguments("self.lesson_assets.hil.inspect",
                              prefix + foreign + "\"}"),
           "foreign-slug HIL inspection pair accepted");
}

void TestCheckedCallbackTreeStages() {
    {
        HookScope hooks;
        hooks.state().fail_next = true;
        ExpectStableOom([] { (void)MakeCheckedCJsonObject(); }, "callback root OOM");
        Expect(hooks.state().live.empty(), "callback root leaked");
    }
    {
        HookScope hooks;
        auto root = MakeCheckedCJsonObject();
        hooks.state().fail_next = true;
        ExpectStableOom([] { (void)MakeCheckedCJsonArray(); }, "callback array OOM");
        root.reset();
        Expect(hooks.state().live.empty(), "callback array leaked");
    }
    {
        HookScope hooks;
        auto array = MakeCheckedCJsonArray();
        hooks.state().fail_next = true;
        ExpectStableOom([] { (void)MakeCheckedCJsonObject(); }, "callback item OOM");
        array.reset();
        Expect(hooks.state().live.empty(), "callback item leaked");
    }
    for (int allocation = 1; allocation <= 3; ++allocation) {
        HookScope hooks;
        auto item = MakeCheckedCJsonObject();
        hooks.state().fail_countdown = allocation;
        ExpectStableOom(
            [&] { CheckedCJsonAddStringToObject(item.get(), "state", "DOWNLOADED"); },
            "callback string OOM");
        item.reset();
        Expect(hooks.state().live.empty(), "callback string leaked");
    }
}

void TestMcpCallOwnsInnerJsonWhenPrintingFails() {
    HookScope hooks;
    auto callback_json = MakeCheckedCJsonObject();
    CheckedCJsonAddStringToObject(callback_json.get(), "ready", "yes");
    cJSON* raw = callback_json.release();
    McpTool tool("test.inner", "test", PropertyList(),
                 [raw](const PropertyList&) -> ReturnValue { return raw; });
    hooks.state().fail_size = 256;
    hooks.state().fail_size_occurrence = 1;
    ExpectStableOom([&] { (void)tool.Call(PropertyList()); }, "inner print OOM");
    Expect(hooks.state().live.empty(), "inner print leaked callback or wrapper JSON");
}

void TestMcpCallWrapperAndFinalPrintFailures() {
    {
        HookScope hooks;
        McpTool tool("test.wrapper", "test", PropertyList(),
                     [](const PropertyList&) -> ReturnValue { return true; });
        hooks.state().fail_next = true;
        ExpectStableOom([&] { (void)tool.Call(PropertyList()); }, "wrapper OOM");
        Expect(hooks.state().live.empty(), "wrapper OOM leaked");
    }
    {
        HookScope hooks;
        McpTool tool("test.final", "test", PropertyList(),
                     [](const PropertyList&) -> ReturnValue { return true; });
        hooks.state().fail_size = 256;
        hooks.state().fail_size_occurrence = 1;
        ExpectStableOom([&] { (void)tool.Call(PropertyList()); }, "final print OOM");
        Expect(hooks.state().live.empty(), "final print OOM leaked");
    }
}

void TestMcpSchemaSerializationUsesTheSameStableOomContract() {
    {
        HookScope hooks;
        Property property("assetPack", kPropertyTypeObject);
        hooks.state().fail_size = 256;
        hooks.state().fail_size_occurrence = 1;
        ExpectStableOom([&] { (void)property.to_json(); }, "property final print OOM");
        Expect(hooks.state().live.empty(), "property serialization OOM leaked");
    }
    {
        HookScope hooks;
        PropertyList properties({Property("assetPack", kPropertyTypeObject)});
        McpTool tool("test.schema", "test", properties,
                     [](const PropertyList&) -> ReturnValue { return true; });
        hooks.state().fail_size = 256;
        hooks.state().fail_size_occurrence = 3;
        ExpectStableOom([&] { (void)tool.to_json(); }, "tool schema print OOM");
        Expect(hooks.state().live.empty(), "tool schema OOM leaked");
    }
}

void TestPreparedMcpCallAllocatesAllCJsonBeforeMutation() {
    HookScope hooks;
    int mutations = 0;
    McpTool tool(
        "test.prepared", "test", PropertyList(), PreparedMcpCall{},
        [&](const PropertyList&) -> std::string {
            auto payload = MakeCheckedCJsonObject();
            CheckedCJsonAddNumberToObject(payload.get(), "sequence", 0);
            cJSON* sequence = cJSON_GetObjectItem(payload.get(), "sequence");
            PreparedMcpTextResult response(std::move(payload), 256, 768);

            // Any cJSON allocation after this point would fail and strand the
            // successful side effect without a response.
            hooks.state().fail_next = true;
            ++mutations;
            cJSON_SetNumberValue(sequence, 7);
            return response.Finish();
        });

    const std::string result = tool.Call(PropertyList());
    Expect(mutations == 1, "prepared call did not execute mutation once");
    Expect(result.find("\\\"sequence\\\":7") != std::string::npos,
           "prepared call omitted payload");
    ExpectStableOom([] { (void)MakeCheckedCJsonObject(); },
                    "prepared call consumed post-mutation cJSON allocation");
    Expect(hooks.state().live.empty(), "prepared call leaked");
}

void TestPreparedMcpCallOomCannotReachMutation() {
    bool observed_success = false;
    for (int allocation = 1; allocation <= 24; ++allocation) {
        HookScope hooks;
        int mutations = 0;
        McpTool tool(
            "test.prepared.oom", "test", PropertyList(), PreparedMcpCall{},
            [&](const PropertyList&) -> std::string {
                auto payload = MakeCheckedCJsonObject();
                CheckedCJsonAddStringToObject(payload.get(), "status", "staged");
                PreparedMcpTextResult response(std::move(payload), 256, 768);
                ++mutations;
                return response.Finish();
            });
        hooks.state().fail_countdown = allocation;
        try {
            (void)tool.Call(PropertyList());
            observed_success = true;
            Expect(mutations == 1, "prepared success skipped mutation");
        } catch (const std::runtime_error& error) {
            Expect(std::string(error.what()) == kStableError,
                   "prepared OOM was not sanitized");
            Expect(mutations == 0, "prepared OOM reached mutation");
        }
        Expect(hooks.state().live.empty(), "prepared OOM leaked");
        if (observed_success) {
            break;
        }
    }
    Expect(observed_success, "prepared response never completed");
}

void TestPreparedMutationResponsesFinishWithoutHeapAllocation() {
    for (const char* operation : {"arm", "stage", "cleanup"}) {
        int mutations = 0;
        McpTool tool(
            std::string("test.prepared.") + operation, "test", PropertyList(),
            PreparedMcpCall{}, [&](const PropertyList&) -> std::string {
                auto payload = MakeCheckedCJsonObject();
                CheckedCJsonAddStringToObject(payload.get(), "status", operation);
                PreparedMcpTextResult response(std::move(payload), 256, 768);
                ++mutations;
                g_fail_cpp_allocations = true;
                return response.Finish();
            });
        std::string result;
        try {
            result = tool.Call(PropertyList());
        } catch (...) {
            g_fail_cpp_allocations = false;
            throw;
        }
        g_fail_cpp_allocations = false;
        Expect(mutations == 1, "prepared mutation did not execute once");
        Expect(result.find(operation) != std::string::npos,
               "prepared mutation response missing");
    }
}

}  // namespace

int main() {
    TestHilInspectRejectsInvalidSiblingPairsBeforeFilesystemAccess();
    TestCheckedCallbackTreeStages();
    TestMcpCallOwnsInnerJsonWhenPrintingFails();
    TestMcpCallWrapperAndFinalPrintFailures();
    TestMcpSchemaSerializationUsesTheSameStableOomContract();
    TestPreparedMcpCallAllocatesAllCJsonBeforeMutation();
    TestPreparedMcpCallOomCannotReachMutation();
    TestPreparedMutationResponsesFinishWithoutHeapAllocation();
    std::cout << "MCP cJSON OOM host tests passed\n";
    return 0;
}
