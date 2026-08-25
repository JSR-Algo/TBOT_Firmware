#ifndef WAKE_WORD_MODEL_MAP_H_
#define WAKE_WORD_MODEL_MAP_H_

#include <string>
#include <vector>

inline const std::string* ResolveWakeWordLabel(
        const std::vector<std::vector<std::string>>& wake_words_by_model,
        int model_index, int wake_word_index) {
    if (model_index < 1 ||
        model_index > static_cast<int>(wake_words_by_model.size())) {
        return nullptr;
    }
    const auto& wake_words = wake_words_by_model[model_index - 1];
    if (wake_word_index < 1 ||
        wake_word_index > static_cast<int>(wake_words.size())) {
        return nullptr;
    }
    return &wake_words[wake_word_index - 1];
}

#endif
