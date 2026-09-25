#ifndef LESSON_ASSET_RETAINED_PARSER_H
#define LESSON_ASSET_RETAINED_PARSER_H
#include "lesson_asset_retained_selection.h"
#include <cJSON.h>
struct RetainedDeviceOperation { RetainedSelectionOwner owner; bool release; };
RetainedDeviceOperation ParseRetainedDeviceOperation(const cJSON* operation);
std::uint64_t ParseRetainedSyncRevision(const cJSON* pack);
void AddRetainedDeviceState(cJSON* response, const RetainedSelectionState& state);
void AddRetainedDeviceReceipt(cJSON* response, const RetainedDeviceOperation& operation);
#endif
