#ifndef LESSON_STORAGE_HIL_U64_FORMAT_H
#define LESSON_STORAGE_HIL_U64_FORMAT_H

#include <cstddef>
#include <cstdint>

struct LessonStorageHilUint64Text {
    char text[21]{};

    const char* c_str() const noexcept { return text; }
};

inline LessonStorageHilUint64Text FormatLessonStorageHilUint64(
    std::uint64_t value
) noexcept {
    LessonStorageHilUint64Text result{};
    std::size_t length = 0;
    do {
        result.text[length++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0);

    for (std::size_t left = 0, right = length - 1; left < right; ++left, --right) {
        const char digit = result.text[left];
        result.text[left] = result.text[right];
        result.text[right] = digit;
    }
    result.text[length] = '\0';
    return result;
}

#endif  // LESSON_STORAGE_HIL_U64_FORMAT_H
