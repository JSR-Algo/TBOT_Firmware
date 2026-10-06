#ifndef LESSON_ORIGINAL_SOURCE_DEVICE_SELFTEST_H
#define LESSON_ORIGINAL_SOURCE_DEVICE_SELFTEST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tbot {

// Attended-lab-only on-device qualification of renderer v6
// (CONFIG_TBOT_LESSON_RENDERER_V6_DEVICE_SELFTEST, default off). A dedicated
// "v6media" flash partition carries the published original-source scene, its
// originals and the cue list; the self-test copies them into an SD pack and plays
// every cue in real time through the production v6 runtime, logging V6SELFTEST lines.
//
// Media image layout (little endian):
//   header  magic "TBV6MED1" | u32 entry count | u32 reserved (0)
//   entries name[96] (NUL padded) | u32 offset | u32 size | sha256[32]
//   data    entry bytes at their offsets, after the index, non-overlapping
inline constexpr char kV6SelfTestMediaMagic[8] = {'T', 'B', 'V', '6', 'M', 'E', 'D', '1'};
inline constexpr std::size_t kV6SelfTestHeaderBytes = 16;
inline constexpr std::size_t kV6SelfTestNameBytes = 96;
inline constexpr std::size_t kV6SelfTestEntryBytes = kV6SelfTestNameBytes + 4 + 4 + 32;
inline constexpr std::uint32_t kV6SelfTestMaxEntries = 16;
inline constexpr char kV6SelfTestPlanName[] = "selftest.json";

struct V6SelfTestMediaEntry {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint8_t sha256[32] = {};
};

// Bytes of index needed for `count` entries.
inline constexpr std::size_t V6SelfTestIndexBytes(std::uint32_t count) {
    return kV6SelfTestHeaderBytes + static_cast<std::size_t>(count) * kV6SelfTestEntryBytes;
}

// Reads the entry count from a header; 0 when the header is not a media image.
std::uint32_t V6SelfTestMediaEntryCount(const std::uint8_t* header, std::size_t header_bytes);

// Validates the index of a media image of `image_bytes`. Returns "" on success,
// otherwise the reason; `entries` is only filled on success.
std::string ParseV6SelfTestMediaIndex(const std::uint8_t* index, std::size_t index_bytes, std::size_t image_bytes,
                                      std::vector<V6SelfTestMediaEntry>& entries);

// Starts the self-test task once (no-op unless the lab option is built).
void StartOriginalSourceDeviceSelfTest();

}  // namespace tbot

#endif
