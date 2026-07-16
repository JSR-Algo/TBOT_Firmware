#include <cJSON.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_set>

#include "checked_cjson.h"
#include "mcp_server.h"

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

}  // namespace

int main() {
    TestCheckedCallbackTreeStages();
    TestMcpCallOwnsInnerJsonWhenPrintingFails();
    TestMcpCallWrapperAndFinalPrintFailures();
    TestMcpSchemaSerializationUsesTheSameStableOomContract();
    TestPreparedMcpCallAllocatesAllCJsonBeforeMutation();
    TestPreparedMcpCallOomCannotReachMutation();
    std::cout << "MCP cJSON OOM host tests passed\n";
    return 0;
}
