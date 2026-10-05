#include "checked_cjson.h"
#include "lesson_original_source_contract.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tbot;

namespace {
int failures = 0;
void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++failures;
    }
}
const cJSON* Get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }
std::string Name(const cJSON* vector) { return Get(vector, "name")->valuestring; }
const char* PhaseName(OriginalSourceRendererPhase phase) {
    static const char* const kNames[] = {"idle", "prepared", "running", "paused"};
    return kNames[static_cast<int>(phase)];
}
const char* VerdictName(OriginalSourceVerdict verdict) {
    static const char* const kNames[] = {"applied", "replayed", "stale", "invalid-state"};
    return kNames[static_cast<int>(verdict)];
}
std::vector<std::string> StringArray(const cJSON* array) {
    std::vector<std::string> values;
    for (const cJSON* item = array->child; item != nullptr; item = item->next) values.push_back(item->valuestring);
    return values;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    CheckedCJsonPtr root(cJSON_Parse(text.str().c_str()));
    if (!root) return 3;
    Expect(std::string(Get(root.get(), "rendererVersion")->valuestring) == kLessonRendererV6, "renderer identity");
    const cJSON* limits = Get(root.get(), "limits");
    Expect(Get(limits, "maxBytes")->valuedouble == kOriginalSourceMaxBytes &&
           Get(limits, "maxDimension")->valuedouble == kOriginalSourceMaxDimension &&
           Get(limits, "maxFrames")->valuedouble == kOriginalSourceMaxFrames &&
           Get(limits, "maxOriginals")->valuedouble == kOriginalSourceMaxOriginals &&
           Get(limits, "maxSceneBytes")->valuedouble == kOriginalSourceSceneMaxBytes, "limits match backend");
    int checks = 0;
    const cJSON* scenes = Get(root.get(), "scenes");
    OriginalSourceSceneInfo farm;
    for (const cJSON* vector = Get(scenes, "valid")->child; vector != nullptr; vector = vector->next, ++checks) {
        OriginalSourceSceneInfo info;
        const char* error = ParseOriginalSourceScene(Get(vector, "scene"), &info);
        Expect(error == nullptr, "valid scene " + Name(vector) + ": " + (error ? error : ""));
        Expect(info.required_capabilities == StringArray(Get(vector, "requiredCapabilities")),
               "capabilities " + Name(vector));
        // The exact pack bytes parse to the same verdict.
        CheckedCJsonPtr canonical(cJSON_Parse(Get(vector, "canonicalJson")->valuestring));
        OriginalSourceSceneInfo again;
        Expect(canonical && ParseOriginalSourceScene(canonical.get(), &again) == nullptr, "canonical " + Name(vector));
        if (farm.originals.empty()) farm = info;
    }
    Expect(farm.originals.size() == 7 && farm.step_count == 2 && farm.originals[0].time_base_den == 12288,
           "farm originals keep rational time and step count");
    for (const cJSON* vector = Get(scenes, "invalid")->child; vector != nullptr; vector = vector->next, ++checks) {
        OriginalSourceSceneInfo info;
        Expect(ParseOriginalSourceScene(Get(vector, "scene"), &info) != nullptr, "invalid scene accepted " + Name(vector));
    }
    for (const cJSON* vector = Get(root.get(), "compatibility")->child; vector != nullptr; vector = vector->next, ++checks) {
        Expect(MissingOriginalSourceCapabilities(farm, StringArray(Get(vector, "advertised"))) ==
               StringArray(Get(vector, "missing")), "compatibility " + Name(vector));
    }
    const cJSON* commands = Get(root.get(), "commands");
    for (const cJSON* vector = Get(commands, "valid")->child; vector != nullptr; vector = vector->next, ++checks) {
        OriginalSourceCommandInfo info;
        const char* error = ParseOriginalSourceCommand(Get(vector, "frameType")->valuestring, Get(vector, "body"), &info);
        Expect(error == nullptr, "valid command " + Name(vector) + ": " + (error ? error : ""));
        const cJSON* expected = Get(vector, "command");
        Expect(info.cue_id == Get(expected, "cueId")->valuestring &&
               static_cast<double>(info.command_sequence_id) == Get(expected, "commandSequenceId")->valuedouble,
               "command fields " + Name(vector));
    }
    for (const cJSON* vector = Get(commands, "invalid")->child; vector != nullptr; vector = vector->next, ++checks) {
        OriginalSourceCommandInfo info;
        const cJSON* frame = Get(vector, "frameType");
        Expect(ParseOriginalSourceCommand(cJSON_IsString(frame) ? frame->valuestring : nullptr, Get(vector, "body"),
                                          &info) != nullptr, "invalid command accepted " + Name(vector));
    }
    for (const cJSON* ordering = Get(root.get(), "orderings")->child; ordering != nullptr; ordering = ordering->next) {
        OriginalSourceControlState state;
        int index = 0;
        for (const cJSON* step = Get(ordering, "steps")->child; step != nullptr; step = step->next, ++index, ++checks) {
            OriginalSourceCommandInfo command;
            Expect(ParseOriginalSourceCommand(Get(step, "frameType")->valuestring, Get(step, "body"), &command) == nullptr,
                   "ordering command " + Name(ordering));
            const OriginalSourceVerdict verdict = ApplyOriginalSourceCommand(&state, command);
            const std::string at = Name(ordering) + "[" + std::to_string(index) + "]";
            Expect(std::string(VerdictName(verdict)) == Get(step, "verdict")->valuestring, "verdict " + at);
            Expect(std::string(PhaseName(state.phase)) == Get(step, "state")->valuestring &&
                   state.cue_id == Get(step, "cueId")->valuestring &&
                   static_cast<double>(state.last_sequence) == Get(step, "lastSequence")->valuedouble, "state " + at);
        }
    }
    const cJSON* frame = Get(root.get(), "frames")->child;
    CheckedCJsonPtr envelope(cJSON_Parse(Get(frame, "json")->valuestring));
    OriginalSourceCommandInfo prepare;
    Expect(envelope && std::string(Get(envelope.get(), "protocolVersion")->valuestring) == kLessonRendererV6 &&
           ParseOriginalSourceCommand(Get(envelope.get(), "type")->valuestring, Get(envelope.get(), "body"),
                                      &prepare) == nullptr &&
           Get(frame, "bytes")->valuedouble <= Get(limits, "maxFrameBytes")->valuedouble, "prepare envelope");
    if (failures != 0) return 1;
    std::printf("PASS original-source contract: %d backend vectors consumed unchanged\n", checks);
    return 0;
}
