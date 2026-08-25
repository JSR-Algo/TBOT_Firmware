#include "audio/wake_words/wake_word_model_map.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "wake word model map test failed: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    const std::vector<std::vector<std::string>> models = {
        {"model1-word1", "model1-word2"},
        {"model2-word1"},
    };

    const std::string* model2_word1 = ResolveWakeWordLabel(models, 2, 1);
    Require(model2_word1 != nullptr && *model2_word1 == "model2-word1",
            "model 2 word 1 resolves inside model 2 rather than the flattened model 1 list");
    Require(ResolveWakeWordLabel(models, 3, 1) == nullptr,
            "an impossible third model is rejected for a two-slot configuration");
    Require(ResolveWakeWordLabel(models, 1, 3) == nullptr,
            "a word index is validated within its selected model");
    Require(ResolveWakeWordLabel(models, 0, 1) == nullptr &&
                ResolveWakeWordLabel(models, 1, 0) == nullptr,
            "model and word indices are one-based");

    std::cout << "wake word model map test OK\n";
    return 0;
}
