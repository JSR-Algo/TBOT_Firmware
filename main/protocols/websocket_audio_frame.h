#pragma once

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "protocol.h"

// Shared scratch for binary audio frames, defined in websocket_protocol.cc
// (PSRAM .bss on firmware).
extern uint8_t g_websocket_audio_frame_scratch[];
extern const size_t kWebsocketAudioFrameScratchBytes;
extern std::mutex g_websocket_audio_frame_scratch_mutex;

// Frames `payload` for protocol `version` and hands the bytes to `send`.
// Version 1 sends the payload as-is. The scratch is only try-locked, so a
// concurrent sender or an oversized frame uses a heap buffer instead (which
// may throw std::bad_alloc).
template <typename Send>
bool SendWebsocketAudioFrame(int version, uint32_t timestamp,
                             const std::vector<uint8_t>& payload, Send&& send) {
    const size_t header = version == 2 ? sizeof(BinaryProtocol2)
                        : version == 3 ? sizeof(BinaryProtocol3) : 0;
    if (header == 0) return send(payload.data(), payload.size());
    const size_t frame_size = header + payload.size();
    std::unique_lock<std::mutex> lock(g_websocket_audio_frame_scratch_mutex, std::try_to_lock);
    std::vector<uint8_t> heap_frame;
    uint8_t* frame = g_websocket_audio_frame_scratch;
    if (!lock.owns_lock() || frame_size > kWebsocketAudioFrameScratchBytes) {
        if (lock.owns_lock()) lock.unlock();
        heap_frame.resize(frame_size);
        frame = heap_frame.data();
    }
    if (version == 2) {
        auto* bp2 = reinterpret_cast<BinaryProtocol2*>(frame);
        bp2->version = htons(version);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(timestamp);
        bp2->payload_size = htonl(payload.size());
        memcpy(bp2->payload, payload.data(), payload.size());
    } else {
        auto* bp3 = reinterpret_cast<BinaryProtocol3*>(frame);
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(payload.size());
        memcpy(bp3->payload, payload.data(), payload.size());
    }
    return send(frame, frame_size);
}
