#pragma once

#include <string>
#include <vector>

inline int& HostHeapPhaseMonitorStarts() {
    static int value = 0;
    return value;
}

inline int& HostHeapPhaseMonitorStops() {
    static int value = 0;
    return value;
}

inline std::vector<std::string>& HostHeapCheckpointPhases() {
    static std::vector<std::string> phases;
    return phases;
}

class SystemInfo {
public:
    static void StartHeapPhaseMonitor() { ++HostHeapPhaseMonitorStarts(); }
    static void StopHeapPhaseMonitor() { ++HostHeapPhaseMonitorStops(); }
    static void PrintHeapCheckpoint(const char* phase) {
        HostHeapCheckpointPhases().emplace_back(phase == nullptr ? "unknown" : phase);
    }
};
