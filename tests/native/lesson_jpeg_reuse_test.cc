#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "jpeg_to_image.h"

namespace {
std::size_t allocations = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson_jpeg_reuse test failed: " << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> ReadFile(const char* path) {
    FILE* file = std::fopen(path, "rb");
    Check(file != nullptr, "fixture opens");
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    Check(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size(), "fixture reads");
    std::fclose(file);
    return bytes;
}
}  // namespace

extern "C" void* heap_caps_malloc(size_t size, uint32_t) {
    ++allocations;
    return std::malloc(size);
}

extern "C" void* heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, uint32_t) {
    ++allocations;
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
    jpeg_reusable_decoder_t decoder = {};
    Check(jpeg_reusable_decoder_prepare(&decoder, 46, 46, 0x1234u) == ESP_OK, "decoder prepares once");
    Check(allocations == 2, "prepare allocates work and RGB565 buffers");
    const std::size_t prepared_allocations = allocations;

    for (int i = 0; i < 10; ++i) {
        uint8_t* pixels = nullptr;
        size_t pixels_len = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;
        Check(jpeg_reusable_decoder_decode(&decoder, jpeg.data(), jpeg.size(), &pixels, &pixels_len,
                                           &width, &height, &stride) == ESP_OK,
              "repeated frame decodes");
        Check(pixels == decoder.output_buffer, "decode returns caller-owned reusable output");
        Check(width == 46 && height == 46 && stride == 92 && pixels_len == 46 * 46 * 2,
              "decode metadata is preserved");
        Check(allocations == prepared_allocations, "steady-state decode does not allocate");
    }

    jpeg_reusable_decoder_destroy(&decoder);
    Check(decoder.output_buffer == nullptr && decoder.working_buffer == nullptr, "destroy clears ownership");
    std::cout << "lesson_jpeg_reuse test passed\n";
    return 0;
}
