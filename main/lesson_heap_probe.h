#pragma once

#include <cstddef>

// Records heap state at lesson queue ownership boundaries without inspecting or
// copying lesson content.
void LogLessonHeapBoundary(const char* phase, size_t payload_bytes);
