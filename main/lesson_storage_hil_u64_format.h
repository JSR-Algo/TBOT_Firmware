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

// Hex twin of the above. Both exist because the target's printf has no 64-bit
// integer conversions: using one desyncs the varargs walk and can null-deref on
// the following string conversion. Formatting to text first sidesteps that, and
// test_realtime_voice_state.py enforces the rule across main/.
inline LessonStorageHilUint64Text FormatLessonStorageHilUint64Hex(
    std::uint64_t value
) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    LessonStorageHilUint64Text result{};
    std::size_t length = 0;
    do {
        result.text[length++] = kDigits[value & 0xFU];
        value >>= 4U;
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
