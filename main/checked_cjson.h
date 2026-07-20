#ifndef CHECKED_CJSON_H
#define CHECKED_CJSON_H

#include <cJSON.h>

#include <memory>
#include <stdexcept>
#include <string>

inline constexpr const char* kMcpResponseAllocationError =
    "MCP response allocation failed";

struct CJsonDelete {
    void operator()(cJSON* value) const { cJSON_Delete(value); }
};

struct CJsonFree {
    void operator()(char* value) const { cJSON_free(value); }
};

using CheckedCJsonPtr = std::unique_ptr<cJSON, CJsonDelete>;
using CheckedCJsonStringPtr = std::unique_ptr<char, CJsonFree>;

[[noreturn]] inline void ThrowCJsonAllocationFailure() {
    throw std::runtime_error(kMcpResponseAllocationError);
}

inline CheckedCJsonPtr MakeCheckedCJsonObject() {
    CheckedCJsonPtr value(cJSON_CreateObject());
    if (!value) ThrowCJsonAllocationFailure();
    return value;
}

inline CheckedCJsonPtr MakeCheckedCJsonArray() {
    CheckedCJsonPtr value(cJSON_CreateArray());
    if (!value) ThrowCJsonAllocationFailure();
    return value;
}

inline CheckedCJsonPtr MakeCheckedCJsonString(const char* text) {
    CheckedCJsonPtr value(cJSON_CreateString(text));
    if (!value) ThrowCJsonAllocationFailure();
    return value;
}

inline CheckedCJsonPtr ParseCheckedCJson(const char* text) {
    CheckedCJsonPtr value(cJSON_Parse(text));
    if (!value) ThrowCJsonAllocationFailure();
    return value;
}

inline CheckedCJsonPtr MakeCheckedCJsonNumber(double number) {
    CheckedCJsonPtr value(cJSON_CreateNumber(number));
    if (!value) ThrowCJsonAllocationFailure();
    return value;
}

inline CheckedCJsonPtr MakeCheckedCJsonBool(bool boolean) {
    CheckedCJsonPtr value(cJSON_CreateBool(boolean));
    if (!value) ThrowCJsonAllocationFailure();
    return value;
}

inline void CheckedCJsonAddItemToArray(cJSON* array, CheckedCJsonPtr item) {
    if (array == nullptr || !item || !cJSON_AddItemToArray(array, item.get())) {
        ThrowCJsonAllocationFailure();
    }
    item.release();
}

inline void CheckedCJsonAddItemToObject(
    cJSON* object,
    const char* name,
    CheckedCJsonPtr item
) {
    if (object == nullptr || !item || !cJSON_AddItemToObject(object, name, item.get())) {
        ThrowCJsonAllocationFailure();
    }
    item.release();
}

inline void CheckedCJsonAddStringToObject(
    cJSON* object,
    const char* name,
    const char* text
) {
    CheckedCJsonAddItemToObject(object, name, MakeCheckedCJsonString(text));
}

inline void CheckedCJsonAddNumberToObject(
    cJSON* object,
    const char* name,
    double number
) {
    CheckedCJsonAddItemToObject(object, name, MakeCheckedCJsonNumber(number));
}

inline void CheckedCJsonAddBoolToObject(
    cJSON* object,
    const char* name,
    bool boolean
) {
    CheckedCJsonAddItemToObject(object, name, MakeCheckedCJsonBool(boolean));
}

inline std::string CheckedCJsonPrint(const cJSON* value) {
    CheckedCJsonStringPtr printed(cJSON_PrintUnformatted(value));
    if (!printed) ThrowCJsonAllocationFailure();
    return std::string(printed.get());
}

#endif  // CHECKED_CJSON_H
