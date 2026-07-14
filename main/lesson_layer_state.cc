#include "lesson_layer_state.h"

#include <cstdint>

namespace {
size_t Index(LessonLayer layer) {
    return static_cast<size_t>(layer);
}

bool IsSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

bool NextCodepoint(const std::string& value, size_t& offset, uint32_t& codepoint) {
    if (offset >= value.size()) return false;
    const auto first = static_cast<unsigned char>(value[offset]);
    size_t length = 0;
    if (first <= 0x7f) {
        length = 1;
        codepoint = first;
    } else if ((first & 0xe0) == 0xc0) {
        length = 2;
        codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        length = 3;
        codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        length = 4;
        codepoint = first & 0x07;
    } else {
        return false;
    }
    if (offset + length > value.size()) return false;
    for (size_t i = 1; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(value[offset + i]);
        if ((continuation & 0xc0) != 0x80) return false;
        codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return false;
    }
    offset += length;
    return true;
}
}  // namespace

LessonLayerPlan LessonLayerState::Plan(LessonLayer layer, const char* key,
                                       const char* source) const {
    const Identity& current = layers_[Index(layer)];
    const std::string next_key = key == nullptr ? "" : key;
    const std::string next_source = source == nullptr ? "" : source;
    const bool same_identity = !next_key.empty() ? current.key == next_key
                                                 : current.source == next_source;
    return {current.loaded && same_identity, current.loaded && !same_identity};
}

void LessonLayerState::Commit(LessonLayer layer, const char* key, const char* source) {
    Identity& current = layers_[Index(layer)];
    current.key = key == nullptr ? "" : key;
    current.source = source == nullptr ? "" : source;
    current.loaded = true;
}

void LessonLayerState::Clear(LessonLayer layer) {
    layers_[Index(layer)] = Identity{};
}

void LessonLayerState::ClearAll() {
    Clear(LessonLayer::kBackground);
    Clear(LessonLayer::kObject);
    Clear(LessonLayer::kOverlay);
}

bool LessonLayerState::Has(LessonLayer layer) const {
    return layers_[Index(layer)].loaded;
}

size_t LessonVisibleCodepointCount(const std::string& value) {
    size_t offset = 0;
    size_t count = 0;
    uint32_t codepoint = 0;
    while (offset < value.size()) {
        if (!NextCodepoint(value, offset, codepoint)) return SIZE_MAX;
        ++count;
    }
    return count;
}

bool NormalizeLessonTeachingWord(const char* value, std::string& output) {
    output = value == nullptr ? "" : value;
    size_t first = 0;
    while (first < output.size() && IsSpace(output[first])) ++first;
    size_t last = output.size();
    while (last > first && IsSpace(output[last - 1])) --last;
    output = output.substr(first, last - first);
    if (output.empty()) return true;

    size_t offset = 0;
    size_t count = 0;
    uint32_t codepoint = 0;
    while (offset < output.size()) {
        if (!NextCodepoint(output, offset, codepoint)) return false;
        if (codepoint < 0x20 || codepoint == 0x7f) return false;
        if (++count > kLessonMaxTeachingWordCodepoints) return false;
    }
    return true;
}

int LessonAllocationCaps(size_t) {
    return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
}

LessonWordPillRect LessonWordPillGeometry(int display_width, int display_height) {
    return {display_width * 166 / 480, display_height * 26 / 320,
            display_width * 148 / 480, display_height * 42 / 320, true};
}
