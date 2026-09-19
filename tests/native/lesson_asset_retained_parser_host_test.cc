#include "lesson_asset_retained_parser.h"
#include "checked_cjson.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream input(argv[1]);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!root) return 3;
    int checks = 0;
    const auto vectors = cJSON_GetObjectItemCaseSensitive(root.get(), "valid");
    const cJSON* vector;
    cJSON_ArrayForEach(vector, vectors) {
        const auto item = cJSON_GetObjectItemCaseSensitive(vector, "operation");
        const auto action = cJSON_GetObjectItemCaseSensitive(item, "action");
        if (!cJSON_IsString(action) || std::string(action->valuestring) == "acquire" || std::string(action->valuestring) == "reconcile") continue;
        const auto operation = ParseRetainedDeviceOperation(item);
        if (operation.owner.request_id.empty()) return 4;
        ++checks;
        auto copy = cJSON_Duplicate(item, true);
        cJSON_AddStringToObject(copy, "unexpected", "refuse");
        bool refused = false;
        try { (void)ParseRetainedDeviceOperation(copy); } catch (const std::runtime_error&) { refused = true; }
        cJSON_Delete(copy);
        if (!refused) return 5;
        ++checks;
        for (const char* field : {"requestId", "deviceId", "consumerIdentity", "operationId",
                                  "requestRevision", "desiredSelectionRevision"}) {
            auto duplicate = cJSON_Duplicate(item, true);
            cJSON_AddItemToObject(duplicate, field, cJSON_Duplicate(cJSON_GetObjectItem(item, field), true));
            bool rejected = false;
            try { (void)ParseRetainedDeviceOperation(duplicate); } catch (...) { rejected = true; }
            cJSON_Delete(duplicate);
            if (!rejected) return 7;
            ++checks;
        }
        for (const double revision : {0., -1., 0.5, 9007199254740992.}) {
            auto invalid = cJSON_Duplicate(item, true);
            cJSON_ReplaceItemInObject(invalid, "desiredSelectionRevision", cJSON_CreateNumber(revision));
            bool rejected = false;
            try { (void)ParseRetainedDeviceOperation(invalid); } catch (...) { rejected = true; }
            cJSON_Delete(invalid);
            if (!rejected) return 8;
            ++checks;
        }
    }
    std::ifstream devices(argv[2]);
    const std::string device_text((std::istreambuf_iterator<char>(devices)), {});
    CheckedCJsonPtr device_vectors(cJSON_Parse(device_text.c_str()));
    if (!device_vectors) return 9;
    cJSON_ArrayForEach(vector, cJSON_GetObjectItem(device_vectors.get(), "valid")) {
        const auto operation = ParseRetainedDeviceOperation(cJSON_GetObjectItem(vector, "operation"));
        auto actual = MakeCheckedCJsonObject();
        AddRetainedDeviceReceipt(actual.get(), operation);
        if (!cJSON_Compare(actual.get(), cJSON_GetObjectItem(vector, "receipt"), true)) return 10;
        ++checks;
    }
    if (checks != 26) return 6;
    std::cout << "retained parser shared vectors OK (" << checks << " checks)\n";
}
