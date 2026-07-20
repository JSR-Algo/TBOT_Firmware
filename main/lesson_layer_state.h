#ifndef LESSON_LAYER_STATE_H
#define LESSON_LAYER_STATE_H

#include <cstddef>
#include <string>

#include <esp_heap_caps.h>

constexpr size_t kLessonMaxTeachingWordCodepoints = 12;

enum class LessonLayer { kBackground = 0, kObject = 1, kOverlay = 2 };

struct LessonLayerPlan {
    bool reused = false;
    bool clear_before_load = false;
};

struct LessonWordPillRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool whole_word_highlight = true;
};

class LessonLayerState {
public:
    LessonLayerPlan Plan(LessonLayer layer, const char* key, const char* source) const;
    void Commit(LessonLayer layer, const char* key, const char* source);
    void Clear(LessonLayer layer);
    void ClearAll();
    bool Has(LessonLayer layer) const;

private:
    struct Identity {
        std::string key;
        std::string source;
        bool loaded = false;
    };
    Identity layers_[3];
};

size_t LessonVisibleCodepointCount(const std::string& value);
bool NormalizeLessonTeachingWord(const char* value, std::string& output);
int LessonAllocationCaps(size_t size);
LessonWordPillRect LessonWordPillGeometry(int display_width, int display_height);

#endif
