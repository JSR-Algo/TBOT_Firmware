#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "jpeg_to_image.h"

namespace {
struct AllocationCounts {
    std::size_t heap_allocs = 0;
    std::size_t heap_frees = 0;
    std::size_t mallocs = 0;
    std::size_t callocs = 0;
    std::size_t reallocs = 0;
    std::size_t frees = 0;
    std::size_t news = 0;
    std::size_t deletes = 0;
};

AllocationCounts counts;
bool track_allocations = false;

void ResetCounts() {
    counts = {};
}

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

extern "C" void tbot_allocation_probe(void);

extern "C" void* tbot_test_malloc(size_t size) {
    if (track_allocations) ++counts.mallocs;
    return std::malloc(size);
}

extern "C" void* tbot_test_calloc(size_t count, size_t size) {
    if (track_allocations) ++counts.callocs;
    return std::calloc(count, size);
}

extern "C" void* tbot_test_realloc(void* ptr, size_t size) {
    if (track_allocations) ++counts.reallocs;
    return std::realloc(ptr, size);
}

extern "C" void tbot_test_free(void* ptr) {
    if (track_allocations) ++counts.frees;
    std::free(ptr);
}

void* operator new(std::size_t size) {
    if (track_allocations) ++counts.news;
    if (void* ptr = std::malloc(size)) return ptr;
    std::abort();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* ptr) noexcept {
    if (track_allocations) ++counts.deletes;
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    ::operator delete(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}

extern "C" void* heap_caps_malloc(size_t size, uint32_t) {
    if (track_allocations) ++counts.heap_allocs;
    return std::malloc(size);
}

extern "C" void* heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, uint32_t) {
    if (track_allocations) ++counts.heap_allocs;
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, count * size) != 0) {
        return nullptr;
    }
    std::memset(ptr, 0, count * size);
    return ptr;
}

extern "C" void heap_caps_free(void* ptr) {
    if (track_allocations) ++counts.heap_frees;
    std::free(ptr);
}

int main(int argc, char** argv) {
    Check(argc == 2, "fixture path argument");
    const auto jpeg = ReadFile(argv[1]);
    jpeg_reusable_decoder_t decoder = {};
    Check(jpeg_reusable_decoder_prepare(&decoder, 46, 46, 0x1234u) == ESP_OK, "decoder prepares once");

    track_allocations = true;
    tbot_allocation_probe();
    volatile auto* one = new std::uint8_t(1);
    delete one;
    volatile auto* many = new std::uint8_t[2];
    delete[] many;
    track_allocations = false;
    Check(counts.mallocs == 1 && counts.callocs == 1 && counts.reallocs == 1 && counts.frees == 3,
          "libc allocation interception probe is active");
    Check(counts.news == 2 && counts.deletes == 2, "C++ allocation interception probe is active");
    ResetCounts();
    track_allocations = true;

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
    }
    track_allocations = false;
    Check(counts.heap_allocs == 0 && counts.heap_frees == 0, "steady-state decode does not use heap_caps");
    Check(counts.mallocs == 0 && counts.callocs == 0 && counts.reallocs == 0 && counts.frees == 0,
          "steady-state decoder and linked esp_jpeg code do not use libc allocation");
    Check(counts.news == 0 && counts.deletes == 0, "steady-state decode does not use C++ allocation");

    jpeg_reusable_decoder_destroy(&decoder);
    Check(decoder.output_buffer == nullptr && decoder.working_buffer == nullptr, "destroy clears ownership");

    jpeg_reusable_decoder_t caller_owned = {};
    Check(jpeg_reusable_decoder_prepare_workspace(&caller_owned, 46, 46, 0x1234u) == ESP_OK,
          "workspace-only decoder prepares without a second output allocation");
    Check(caller_owned.output_buffer == nullptr && caller_owned.working_buffer != nullptr,
          "workspace-only decoder leaves presentation ownership with caller");
    std::vector<std::uint8_t> caller_pixels(46 * 46 * 2);
    ResetCounts();
    track_allocations = true;
    size_t caller_len = 0;
    size_t caller_width = 0;
    size_t caller_height = 0;
    size_t caller_stride = 0;
    Check(jpeg_reusable_decoder_decode_into(
              &caller_owned, jpeg.data(), jpeg.size(), caller_pixels.data(), caller_pixels.size(),
              &caller_len, &caller_width, &caller_height, &caller_stride) == ESP_OK,
          "decoder writes directly into caller-owned RGB565 framebuffer");
    track_allocations = false;
    Check(caller_len == caller_pixels.size() && caller_width == 46 && caller_height == 46 &&
              caller_stride == 92,
          "caller-owned decode preserves RGB565 metadata");
    Check(counts.heap_allocs == 0 && counts.mallocs == 0 && counts.callocs == 0 &&
              counts.reallocs == 0 && counts.news == 0,
          "caller-owned decode has no steady-state allocation");
    jpeg_reusable_decoder_destroy(&caller_owned);
    std::cout << "lesson_jpeg_reuse test passed\n";
    return 0;
}
