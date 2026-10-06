// Host test for the renderer v6 self-test media index parser.
// Usage: lesson_original_source_device_selftest_index_test [packed image]
#include "lesson_original_source_device_selftest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using tbot::ParseV6SelfTestMediaIndex;
using tbot::V6SelfTestMediaEntry;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void PutU32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

struct Spec {
    std::string name;
    std::uint32_t offset, size;
};

std::vector<std::uint8_t> Image(const std::vector<Spec>& specs, std::size_t total) {
    std::vector<std::uint8_t> out(total, 0);
    std::memcpy(out.data(), tbot::kV6SelfTestMediaMagic, 8);
    PutU32(out, 8, static_cast<std::uint32_t>(specs.size()));
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const std::size_t at = tbot::kV6SelfTestHeaderBytes + i * tbot::kV6SelfTestEntryBytes;
        std::memcpy(out.data() + at, specs[i].name.data(), std::min<std::size_t>(specs[i].name.size(), 96));
        PutU32(out, at + 96, specs[i].offset);
        PutU32(out, at + 100, specs[i].size);
    }
    return out;
}

std::string Parse(const std::vector<std::uint8_t>& image, std::vector<V6SelfTestMediaEntry>* out = nullptr) {
    std::vector<V6SelfTestMediaEntry> entries;
    std::string error = ParseV6SelfTestMediaIndex(image.data(), image.size(), image.size(), entries);
    if (out) *out = entries;
    return error;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint32_t data = static_cast<std::uint32_t>(tbot::V6SelfTestIndexBytes(2));
    std::vector<V6SelfTestMediaEntry> entries;
    Expect(Parse(Image({{"scene.original-source.v1", data, 10}, {"selftest.json", data + 12, 5}}, 4096), &entries)
               .empty(),
           "valid image accepted");
    Expect(entries.size() == 2 && entries[1].name == "selftest.json" && entries[1].offset == data + 12,
           "entries decoded");

    auto bad_magic = Image({{"selftest.json", data, 5}, {"a", data + 8, 1}}, 4096);
    bad_magic[0] = 'X';
    Expect(!Parse(bad_magic).empty(), "bad magic refused");
    Expect(!Parse(Image({{"selftest.json", data - 1, 5}, {"a", data + 8, 1}}, 4096)).empty(), "index overlap refused");
    Expect(!Parse(Image({{"selftest.json", data, 5}, {"a", data + 4, 4}}, 4096)).empty(), "entry overlap refused");
    Expect(!Parse(Image({{"selftest.json", data, 5}, {"a", 4090, 10}}, 4096)).empty(), "past-the-end refused");
    Expect(!Parse(Image({{"selftest.json", data, 5}, {"../x", data + 8, 1}}, 4096)).empty(), "traversal refused");
    Expect(!Parse(Image({{"selftest.json", data, 5}, {"a/b", data + 8, 1}}, 4096)).empty(), "slash refused");
    Expect(!Parse(Image({{"selftest.json", data, 5}, {"selftest.json", data + 8, 1}}, 4096)).empty(),
           "duplicate refused");
    Expect(!Parse(Image({{"scene", data, 5}, {"a", data + 8, 1}}, 4096)).empty(), "missing plan refused");
    Expect(!Parse(Image({{"selftest.json", data, 0}, {"a", data + 8, 1}}, 4096)).empty(), "empty entry refused");
    Expect(!Parse(Image({{std::string(96, 'a'), data, 5}, {"selftest.json", data + 8, 1}}, 4096)).empty(),
           "unterminated name refused");
    auto many = Image({{"selftest.json", data, 5}}, 4096);
    PutU32(many, 8, tbot::kV6SelfTestMaxEntries + 1);
    Expect(!Parse(many).empty(), "too many entries refused");
    std::vector<std::uint8_t> truncated(Image({{"selftest.json", data, 5}, {"a", data + 8, 1}}, 4096));
    truncated.resize(data - 1);
    Expect(!Parse(truncated).empty(), "truncated index refused");

    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        std::vector<std::uint8_t> packed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<V6SelfTestMediaEntry> real;
        const std::string error = Parse(packed, &real);
        Expect(error.empty(), "packed image accepted");
        std::printf("packed image: %zu entries%s%s\n", real.size(), error.empty() ? "" : ", error: ", error.c_str());
    }
    std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
