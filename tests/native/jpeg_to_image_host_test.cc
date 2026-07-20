#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "jpeg_to_image.h"

namespace {
constexpr uint32_t kPsramCaps = 0x1234u;
std::vector<uint32_t> allocation_caps;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "jpeg_to_image host test failed: " << message << "\n";
        std::exit(1);
    }
}

std::vector<uint8_t> ReadFile(const char* path) {
    FILE* file = std::fopen(path, "rb");
    Check(file != nullptr, "fixture opens");
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    Check(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size(), "fixture reads");
    std::fclose(file);
    return bytes;
}

struct FreeBytes {
    void operator()(uint8_t* bytes) const { std::free(bytes); }
};

void CheckFailureExact(const uint8_t* bytes, size_t size) {
    std::unique_ptr<uint8_t, FreeBytes> exact(static_cast<uint8_t*>(std::malloc(size == 0 ? 1 : size)));
    Check(exact != nullptr, "exact invalid fixture allocates");
    if (size != 0) {
        std::memcpy(exact.get(), bytes, size);
    }
    uint8_t* out = reinterpret_cast<uint8_t*>(1);
    size_t out_len = 1;
    size_t width = 1;
    size_t height = 1;
    size_t stride = 1;
    Check(jpeg_to_image_with_caps(exact.get(), size, &out, &out_len, &width, &height, &stride,
                                  kPsramCaps) != ESP_OK,
          "invalid JPEG is rejected");
    Check(out == nullptr && out_len == 0 && width == 0 && height == 0 && stride == 0,
          "failure clears output contract");
}

void CheckFailureExact(const std::vector<uint8_t>& jpeg) {
    CheckFailureExact(jpeg.data(), jpeg.size());
}

void CheckRejectedBeforeAllocation(const std::vector<uint8_t>& jpeg) {
    const size_t allocations_before = allocation_caps.size();
    CheckFailureExact(jpeg);
    Check(allocation_caps.size() == allocations_before, "invalid pre-SOS structure is rejected before allocation");
}
}  // namespace

extern "C" void* heap_caps_malloc(size_t size, uint32_t caps) {
    allocation_caps.push_back(caps);
    return std::malloc(size);
}

extern "C" void* heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, uint32_t caps) {
    allocation_caps.push_back(caps);
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, count * size) != 0) {
        return nullptr;
    }
    std::memset(ptr, 0, count * size);
    return ptr;
}

extern "C" void heap_caps_free(void* ptr) {
    std::free(ptr);
}

int main(int argc, char** argv) {
    Check(argc == 2, "fixture path argument");
    const auto jpeg = ReadFile(argv[1]);

    uint8_t* out = nullptr;
    size_t out_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    Check(jpeg_to_image_with_caps(jpeg.data(), jpeg.size(), &out, &out_len, &width, &height, &stride,
                                  kPsramCaps) == ESP_OK,
          "baseline fixture decodes");
    Check(width == 46 && height == 46, "fixture dimensions preserved");
    Check(stride == 92 && out_len == 46 * 46 * 2, "RGB565 stride and length preserved");
    Check(out[0] == 0xc3 && out[1] == 0xe1, "RGB565 output is little endian");
    Check(allocation_caps.size() == 2 && allocation_caps[0] == kPsramCaps && allocation_caps[1] == kPsramCaps,
          "work and output buffers use requested PSRAM caps");
    heap_caps_free(out);

    for (size_t truncated_size = 1; truncated_size < 32; ++truncated_size) {
        CheckFailureExact(jpeg.data(), truncated_size);
    }

    const uint8_t malformed_length[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x00};
    CheckFailureExact(malformed_length, sizeof(malformed_length));

    const uint8_t zero_length[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x00};
    CheckFailureExact(zero_length, sizeof(zero_length));

    const uint8_t huge_dimensions[] = {
        0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0xff, 0xff, 0xff, 0xff,
        0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
    };
    CheckFailureExact(huge_dimensions, sizeof(huge_dimensions));

    auto progressive = jpeg;
    bool replaced = false;
    for (size_t i = 0; i + 1 < progressive.size(); ++i) {
        if (progressive[i] == 0xff && progressive[i + 1] == 0xc0) {
            progressive[i + 1] = 0xc2;
            replaced = true;
            break;
        }
    }
    Check(replaced, "baseline fixture contains SOF0");
    CheckFailureExact(progressive);

    size_t sof_offset = 0;
    for (size_t i = 0; i + 3 < jpeg.size(); ++i) {
        if (jpeg[i] == 0xff && jpeg[i + 1] == 0xc0) {
            sof_offset = i;
            break;
        }
    }
    Check(sof_offset != 0, "baseline SOF0 offset found");
    const size_t sof_length = (static_cast<size_t>(jpeg[sof_offset + 2]) << 8) | jpeg[sof_offset + 3];
    const size_t sof_end = sof_offset + 2 + sof_length;
    Check(sof_end <= jpeg.size(), "baseline SOF0 segment is bounded");

    std::vector<uint8_t> missing_sos(jpeg.begin(), jpeg.begin() + sof_end);
    CheckRejectedBeforeAllocation(missing_sos);

    auto truncated_com = missing_sos;
    truncated_com.insert(truncated_com.end(), {0xff, 0xfe, 0x00, 0x10, 0x00});
    CheckRejectedBeforeAllocation(truncated_com);

    auto oversized_app = missing_sos;
    oversized_app.insert(oversized_app.end(), {0xff, 0xe1, 0xff, 0xff});
    CheckRejectedBeforeAllocation(oversized_app);

    std::cout << "jpeg_to_image host external-TJPG parity and ASan boundary checks passed\n";
    return 0;
}
