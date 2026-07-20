#include "lesson_heap_probe.h"

#include <esp_log.h>

#include <cstdio>
#include <initializer_list>
#include <string>

int main() {
    HostEspResetLogs();
    LogLessonHeapBoundary("worker.before_parse", 6307);

    if (HostEspLogs().size() != 1) {
        std::fprintf(stderr, "expected one heap boundary log, got %zu\n", HostEspLogs().size());
        return 1;
    }

    const std::string expected =
        "I LessonHeap lesson_heap phase=worker.before_parse payload_bytes=6307 "
        "internal_free=131072 lifetime_min_internal=98304";
    if (HostEspLogs().front() != expected) {
        std::fprintf(stderr, "unexpected heap boundary log: %s\n", HostEspLogs().front().c_str());
        return 1;
    }

    for (const char* forbidden : {"assignmentId", "sessionId", "transcript", "http://"}) {
        if (HostEspLogs().front().find(forbidden) != std::string::npos) {
            std::fprintf(stderr, "private field leaked into heap boundary log: %s\n", forbidden);
            return 1;
        }
    }

    std::puts("lesson heap probe host test passed");
    return 0;
}
