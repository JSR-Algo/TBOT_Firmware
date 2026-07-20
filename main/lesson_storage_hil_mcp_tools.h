#ifndef LESSON_STORAGE_HIL_MCP_TOOLS_H
#define LESSON_STORAGE_HIL_MCP_TOOLS_H

#include <string>

struct cJSON;
class McpServer;

bool IsExactLessonStorageHilToolName(const std::string& tool_name);
const char* ValidateLessonStorageHilRawArguments(
    const std::string& tool_name,
    const cJSON* arguments
);
void RegisterLessonStorageHilMcpTools(McpServer& server);

#endif  // LESSON_STORAGE_HIL_MCP_TOOLS_H
