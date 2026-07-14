#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

inline std::vector<std::string>& HostEspLogs() {
    static std::vector<std::string> logs;
    return logs;
}

inline void HostEspResetLogs() {
    HostEspLogs().clear();
}

inline void HostEspLog(const char* level, const char* tag, const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    HostEspLogs().emplace_back(std::string(level) + " " + tag + " " + message);
}

#define ESP_LOGI(tag, fmt, ...) HostEspLog("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HostEspLog("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) HostEspLog("E", tag, fmt, ##__VA_ARGS__)
