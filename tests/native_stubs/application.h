#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

enum DeviceState {
    kDeviceStateIdle = 0,
    kDeviceStateWifiConfiguring = 1,
};

class Display;

class HostAbortDelay : public std::runtime_error {
public:
    HostAbortDelay() : std::runtime_error("host delay abort") {}
};

struct HostDelayControl {
    int calls = 0;
    int throw_after = -1;
};

HostDelayControl &HostDelayState();

class AudioService {
public:
    bool next_read_ok = false;
    std::vector<int16_t> next_audio;

    bool ReadAudioData(std::vector<int16_t> &out, int, int) {
        if (!next_read_ok) {
            return false;
        }
        out = next_audio;
        next_read_ok = false;
        return true;
    }
};

class Application {
public:
    DeviceState state = kDeviceStateIdle;
    AudioService audio_service;

    DeviceState GetDeviceState() const { return state; }
    AudioService &GetAudioService() { return audio_service; }
};
