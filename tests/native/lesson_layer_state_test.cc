#include "lesson_layer_state.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int checks = 0;

void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    std::string word;
    Require(NormalizeLessonTeachingWord(" BARN ", word), "ASCII teaching word accepted");
    Require(word == "BARN", "teaching word is trimmed");
    Require(NormalizeLessonTeachingWord("M\xC3\x88O", word), "UTF-8 teaching word accepted");
    Require(LessonVisibleCodepointCount(word) == 3, "UTF-8 counted as codepoints, not bytes");
    Require(NormalizeLessonTeachingWord("\xE2\x98\x83", word), "three-byte codepoint accepted");
    Require(!NormalizeLessonTeachingWord("\xF8", word), "invalid leading byte rejected");
    Require(!NormalizeLessonTeachingWord("\xC0\xAF", word), "overlong UTF-8 rejected");
    Require(!NormalizeLessonTeachingWord("ABCDEFGHIJKLM", word), "thirteen codepoints rejected");
    Require(!NormalizeLessonTeachingWord("bad\x01word", word), "control characters rejected");
    Require(!NormalizeLessonTeachingWord("\xF0\x28\x8C\x28", word), "invalid UTF-8 rejected");

    LessonLayerState layers;
    auto first = layers.Plan(LessonLayer::kBackground, "scene.farm.v1", "sd://farm.png");
    Require(!first.reused && !first.clear_before_load, "empty layer loads without clear");
    layers.Commit(LessonLayer::kBackground, "scene.farm.v1", "sd://farm.png");
    auto same = layers.Plan(LessonLayer::kBackground, "scene.farm.v1", "sd://other-origin.png");
    Require(same.reused && !same.clear_before_load, "equal key reuses decoded layer");
    auto changed = layers.Plan(LessonLayer::kBackground, "scene.farm.v2", "sd://farm-v2.png");
    Require(!changed.reused && changed.clear_before_load, "changed key clears before load");
    layers.Clear(LessonLayer::kBackground);
    Require(!layers.Has(LessonLayer::kBackground), "clear drops layer identity");

    for (size_t size : {size_t{1}, size_t{1024}, size_t{8 * 1024}, size_t{12 * 1024},
                        size_t{16 * 1024}, size_t{16 * 1024 + 1}}) {
        const int caps = LessonAllocationCaps(size);
        Require((caps & MALLOC_CAP_SPIRAM) != 0, "lesson buffers require PSRAM");
        Require((caps & MALLOC_CAP_8BIT) != 0, "lesson buffers remain byte-addressable");
        Require((caps & MALLOC_CAP_INTERNAL) == 0, "lesson buffers never consume internal SRAM");
    }

    const auto geometry = LessonWordPillGeometry(480, 320);
    Require(geometry.x == 166 && geometry.y == 26 && geometry.width == 148 && geometry.height == 42,
            "480x320 word pill matches preview geometry");
    Require(geometry.whole_word_highlight, "word pill highlights the whole word");

    std::cout << "lesson layer state test OK (" << checks << " checks)\n";
}
