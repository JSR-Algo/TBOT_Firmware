#include "json_payload_safety.h"

bool JsonHasForbiddenDecodedNull(const char* json, std::size_t length) {
    if (json == nullptr) return length != 0;

    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(json[index]);
        if (byte == 0) return true;
        if (!in_string) {
            if (byte == '"') in_string = true;
            continue;
        }
        if (escaped) {
            if (byte == 'u' && index + 4 < length &&
                json[index + 1] == '0' && json[index + 2] == '0' &&
                json[index + 3] == '0' && json[index + 4] == '0') {
                return true;
            }
            escaped = false;
            continue;
        }
        if (byte == '\\') {
            escaped = true;
        } else if (byte == '"') {
            in_string = false;
        }
    }
    return false;
}
